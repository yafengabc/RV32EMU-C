package main

import (
	"flag"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"time"
)

const flashSize = 4 * 1024 * 1024
const fsOffset = 0x100000

func makeFlash(kernelPath, ramdiskPath, outPath string) (int, int, error) {
	kernel, err := os.ReadFile(kernelPath)
	if err != nil {
		return 0, 0, err
	}
	ramdisk, err := os.ReadFile(ramdiskPath)
	if err != nil {
		return 0, 0, err
	}
	img := make([]byte, flashSize)
	if len(kernel) > fsOffset {
		return 0, 0, fmt.Errorf("kernel does not fit below the filesystem (%d > %d)", len(kernel), fsOffset)
	}
	copy(img[:len(kernel)], kernel)
	if fsOffset+len(ramdisk) > flashSize {
		return 0, 0, fmt.Errorf("ramdisk does not fit in the 4 MiB flash")
	}
	copy(img[fsOffset:fsOffset+len(ramdisk)], ramdisk)
	if err := os.WriteFile(outPath, img, 0o644); err != nil {
		return 0, 0, err
	}
	return len(kernel), len(ramdisk), nil
}

// cmdTinyOS ports bench/tinyos_qemu.py: boot TinyOS on Espressif's esp32c3
// QEMU from a real flash image and time a bench run.  QEMU is functional-only
// here; `--tb-stats` reports how often the bench loop is retranslated.
func cmdTinyOS(args []string) int {
	fs := flag.NewFlagSet("tinyos", flag.ExitOnError)
	kernelBin := fs.String("kernel-bin", filepath.Join(defaultTinyOS, "esp32c3.bin"), "TinyOS kernel image")
	ramdisk := fs.String("ramdisk", filepath.Join(defaultTinyOS, "ramdisk-esp32c3.bin"), "littlefs image")
	espQemu := fs.String("esp-qemu", os.Getenv("ESP_QEMU"), "Espressif qemu (default: "+defaultEspQEMU+")")
	bootTimeout := fs.Float64("boot-timeout", 20.0, "seconds to wait for the prompt")
	bench := fs.String("bench", "1024 4", "arguments for the guest bench command")
	noBench := fs.Bool("no-bench", false, "skip the bench run")
	tbStats := fs.Bool("tb-stats", false, "also log -d in_asm and report the most retranslated TBs")
	_ = fs.Parse(args)

	for _, p := range []struct{ name, path string }{{"kernel", *kernelBin}, {"ramdisk", *ramdisk}} {
		if !fileExists(p.path) {
			fmt.Fprintf(os.Stderr, "missing %s: %s\n", p.name, p.path)
			return 2
		}
	}
	qemu := findBinary(*espQemu, []string{defaultEspQEMU, "qemu-system-riscv32"})
	if qemu == "" {
		fmt.Fprintln(os.Stderr, "qemu-system-riscv32 not found (install Espressif's QEMU)")
		return 2
	}

	if err := os.MkdirAll(build, 0o755); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	flash := filepath.Join(build, "tinyos-flash.bin")
	klen, rlen, err := makeFlash(*kernelBin, *ramdisk, flash)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	fmt.Printf("flash: %d MiB (kernel %d B @0, ramdisk %d B @0x%X)\n",
		flashSize/(1024*1024), klen, rlen, fsOffset)

	var tbLog string
	qemuArgs := []string{"-M", "esp32c3", "-m", "8M",
		"-drive", fmt.Sprintf("file=%s,if=mtd,format=raw", flash),
		"-display", "none", "-serial", "stdio", "-monitor", "none"}
	if *tbStats {
		tbLog = filepath.Join(build, "tinyos-tb.log")
		_ = os.Remove(tbLog)
		qemuArgs = append(qemuArgs, "-d", "in_asm", "-D", tbLog)
	}

	cmd := exec.Command(qemu, qemuArgs...)
	s, err := startSession(cmd)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	defer s.close()

	ok, hit := s.waitFor([]byte("tos> "), time.Duration(*bootTimeout)*time.Second)
	if !ok {
		fmt.Printf("no prompt in %.0fs; last output:\n", *bootTimeout)
		fmt.Println(s.tail(-1))
		return 1
	}
	fmt.Printf("boot to prompt: %.2f s\n", hit.Seconds())

	if *noBench {
		return 0
	}

	var before int64
	if tbLog != "" {
		if fi, err := os.Stat(tbLog); err == nil {
			before = fi.Size()
		}
	}
	_ = s.sendLine("bench "+*bench, "\r\n")
	t1 := time.Now()
	ok, hit2 := s.waitFor([]byte("done"), 300*time.Second)
	if !ok {
		fmt.Printf("bench '%s' did not finish\n", *bench)
		return 1
	}
	fmt.Printf("bench %-10q: %.1f ms\n", *bench, hit2.Seconds()*1000)

	if tbLog != "" {
		data, err := os.ReadFile(tbLog)
		if err != nil {
			return 0
		}
		tail := data[before:]
		re := regexp.MustCompile(`(?m)^(0x[0-9a-f]{8}):\s`)
		counts := map[string]int{}
		for _, m := range re.FindAllSubmatch(tail, -1) {
			counts[string(m[1])]++
		}
		type kv struct {
			addr string
			n    int
		}
		var top []kv
		for a, n := range counts {
			top = append(top, kv{a, n})
		}
		sort.Slice(top, func(i, j int) bool { return top[i].n > top[j].n })
		fmt.Printf("TB translations during bench: %d distinct, %d total\n", len(counts), len(tail))
		_ = t1
		for i := 0; i < len(top) && i < 6; i++ {
			fmt.Printf("  %s retranslated x%d\n", top[i].addr, top[i].n)
		}
	}
	return 0
}
