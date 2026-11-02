package main

import (
	"bytes"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"
)

// cmdMem ports bench/memcheck.py (+ asan_test.py, leak_test.py): the ASan and
// leak regression in one run.  Needs build/asan/rv32emu-c-asan.exe,
// build/rv32emu-c.dll, build/leakrun.exe and build/leakunit.exe, all produced
// by `go run ./cmd/rv32emu-build`.
func cmdMem(args []string) int {
	fs := flag.NewFlagSet("mem", flag.ExitOnError)
	asanExe := fs.String("asan-exe", filepath.Join(build, "asan", exeName("rv32emu-c-asan")), "")
	runs := fs.Int("runs", 10, "leakrun repetitions")
	max := fs.Int("max", 2_000_000, "leakrun instruction budget")
	_ = fs.Parse(args)

	ok := true

	fmt.Println("== AddressSanitizer ==")
	if fileExists(*asanExe) {
		asanFail := runASan(*asanExe)
		ok = ok && !asanFail
	} else {
		fmt.Printf("skip: %s missing (run 'go run ./cmd/rv32emu-build asan')\n", *asanExe)
	}

	fmt.Println("\n== sessions in one process (DLL, TinyOS boot x6) ==")
	dll := filepath.Join(build, "rv32emu-c.dll")
	leakrun := filepath.Join(build, exeName("leakrun"))
	elf := filepath.Join(defaultTinyOS, "kernel-esp32c3.elf")
	rd := filepath.Join(defaultTinyOS, "ramdisk-esp32c3.bin")
	if fileExists(leakrun) && fileExists(dll) && fileExists(elf) {
		out, err := runOut(leakrun, dll, elf, "-ramdisk", rd, "-max", strconv.Itoa(*max),
			"-runs", strconv.Itoa(*runs), "-join")
		if err != nil {
			fmt.Println(err)
			ok = false
		} else {
			fmt.Println(out)
			if !leakStable(out) {
				fmt.Println("-> GROWTH DETECTED")
				ok = false
			} else {
				fmt.Println("-> stable across runs")
			}
		}
	} else {
		fmt.Println("skip: build leakrun.exe / rv32emu-c.dll first")
	}

	fmt.Println("\n== core: fresh machine per round, one shared bus ==")
	unit := filepath.Join(build, exeName("leakunit"))
	if fileExists(unit) {
		out, err := runOut(unit)
		unitOK := err == nil
		if err != nil {
			fmt.Println(err)
		} else {
			for _, l := range strings.Split(out, "\n") {
				if strings.Contains(l, "private=") {
					fmt.Println(l)
				}
			}
			if strings.Contains(out, "RESULT:") {
				unitOK = unitOK && strings.Contains(out, "PASS")
			}
		}
		if !unitOK {
			ok = false
		}
	} else {
		fmt.Println("skip: build tools/leakunit.c first")
	}

	fmt.Printf("\nRESULT: %s\n", boolStrMem(ok))
	if !ok {
		return 1
	}
	return 0
}

func boolStrMem(b bool) string {
	if b {
		return "PASS"
	}
	return "FAIL"
}

func runWithStdin(name string, args []string, stdin []byte) (string, error) {
	var stdout, stderr bytes.Buffer
	cmd := exec.Command(name, args...)
	cmd.Stdin = bytes.NewReader(stdin)
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr
	err := cmd.Run()
	return stdout.String() + stderr.String(), err
}

func asanEnv() []string {
	env := os.Environ()
	env = append(env, "ASAN_OPTIONS=abort_on_error=1:halt_on_error=1")
	env = append(env, "PATH="+filepath.Join(`C:\msys64\ucrt64\bin`)+string(os.PathListSeparator)+os.Getenv("PATH"))
	return env
}

func runASan(exe string) bool {
	elf := filepath.Join(defaultTinyOS, "kernel-esp32c3.elf")
	rd := filepath.Join(defaultTinyOS, "ramdisk-esp32c3.bin")
	bad := filepath.Join(build, "asan", "bad.elf")

	type caseItem struct {
		label string
		argv  []string
		stdin []byte
	}
	var cases []caseItem
	if fileExists(elf) {
		cases = append(cases, caseItem{"TinyOS boot", []string{"-elf", elf, "-ramdisk", rd,
			"-max", "5000000", "-quiet"}, nil})
		script := []byte(strings.Repeat("cat readme.txt\nuptime\nls\nreboot\nhelp\nexit\n", 4))
		cases = append(cases, caseItem{"TinyOS console script",
			[]string{"-elf", elf, "-ramdisk", rd, "-console", "-max", "3000000"}, script})
	}
	cases = append(cases, caseItem{"missing elf", []string{"-elf", "does-not-exist.elf"}, nil})
	_ = os.WriteFile(bad, append([]byte("\x7fELF\x01\x01\x01"), append(make([]byte, 8), bytes.Repeat([]byte{0xff}, 40)...)...), 0o644)
	cases = append(cases, caseItem{"corrupt elf", []string{"-elf", bad}, nil})
	_ = os.WriteFile(bad, []byte("\x7fELF\x01\x01\x01"), 0o644)
	cases = append(cases, caseItem{"truncated elf", []string{"-elf", bad}, nil})
	cases = append(cases, caseItem{"no input", nil, nil})

	data := filepath.Join(build, "benchdata")
	for _, name := range []string{"fib.elf", "alu_loop.elf", "mem_loop.elf", "pi_big.elf",
		"bench_sort.elf", "bench_sieve.elf"} {
		p := filepath.Join(data, name)
		if fileExists(p) {
			cases = append(cases, caseItem{name, []string{"-elf", p, "-max", "2000000", "-quiet"}, nil})
		}
	}

	failed := 0
	for _, c := range cases {
		cmd := exec.Command(exe, c.argv...)
		cmd.Stdin = bytes.NewReader(c.stdin)
		var out bytes.Buffer
		cmd.Stdout = &out
		cmd.Stderr = &out
		cmd.Env = asanEnv()
		done := make(chan error, 1)
		t0 := time.Now()
		if err := cmd.Start(); err != nil {
			fmt.Printf("[asan] %-24s START FAIL\n", c.label)
			failed++
			continue
		}
		go func() { done <- cmd.Wait() }()
		select {
		case rcErr := <-done:
			text := out.String()
			sanitizer := strings.Contains(text, "AddressSanitizer") && strings.Contains(text, "ERROR")
			rc := 0
			if rcErr != nil {
				rc = 1
			}
			ok := !sanitizer && (rc == 0 || c.label == "missing elf" || c.label == "corrupt elf" ||
				c.label == "truncated elf" || c.label == "no input")
			fmt.Printf("[asan] %-24s rc=%d %7.2fs %s\n", c.label, rc, time.Since(t0).Seconds(), okStr(ok))
			if sanitizer {
				for _, l := range strings.Split(text, "\n")[:40] {
					fmt.Println(l)
				}
			}
			if !ok {
				failed++
			}
		case <-time.After(300 * time.Second):
			_ = cmd.Process.Kill()
			fmt.Printf("[asan] %-24s TIMEOUT\n", c.label)
			failed++
		}
	}

	fmt.Printf("RESULT: %s\n", boolStrMem(failed == 0))
	return failed != 0
}

func okStr(b bool) string {
	if b {
		return "PASS"
	}
	return "FAIL"
}

// leakStable checks the "[leak]" ws=... lines: the first row is "before";
// handle counts must not grow and the working set must stay within 2 MiB.
func leakStable(out string) bool {
	re := regexp.MustCompile(`ws=\s*(\d+) KiB private=\s*(\d+) KiB committed=\s*(\d+) KiB handles=(\d+)`)
	var rows [][]int
	for _, l := range strings.Split(out, "\n") {
		if !strings.Contains(l, "[leak]") || !strings.Contains(l, "ws=") {
			continue
		}
		m := re.FindStringSubmatch(l)
		if m == nil {
			continue
		}
		row := make([]int, 4)
		for i := 0; i < 4; i++ {
			row[i], _ = strconv.Atoi(m[i+1])
		}
		rows = append(rows, row)
	}
	if len(rows) < 3 {
		return false
	}
	before := rows[0]
	worstWS, worstHandles := 0, 0
	for _, r := range rows[1 : len(rows)-2] {
		if g := r[0] - before[0]; g > worstWS {
			worstWS = g
		}
		if g := r[3] - before[3]; g > worstHandles {
			worstHandles = g
		}
	}
	return worstHandles <= 0 && worstWS < 2048
}
