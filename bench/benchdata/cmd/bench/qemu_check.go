package main

import (
	"flag"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

type machineSpec struct {
	base, stack, data uint32
	uart              uint64
	exit              string
	machine           string
	extra             []string
	stop              string // "exit" = process exits itself, "kill" = read+kill
}

var machines = map[string]machineSpec{
	"esp32c3": {base: 0x40000000, stack: 0x3FCDF000, data: 0x3FC80000,
		uart: 0x60000, exit: "spin", machine: "esp32c3", extra: []string{"-m", "8M"}, stop: "kill"},
	"virt": {base: 0x80000000, stack: 0x80100000, data: 0x80010000,
		uart: 0x10000, exit: "uart", machine: "virt", extra: []string{"-m", "128M", "-bios", "none"}, stop: "exit"},
}

// ---- reference values, computed from the algorithm definitions ----

func fib(n int) uint32 {
	a, b := 0, 1
	for i := 0; i < n; i++ {
		a, b = b, a+b
	}
	return uint32(a)
}

func leibniz() uint32 {
	var total int64
	for k := 0; k < 10000; k++ {
		term := int64(0x10000 / (2*k + 1))
		if k&1 == 1 {
			total -= term
		} else {
			total += term
		}
	}
	return uint32(total << 2)
}

func sieve() uint32 {
	const limit = 1001
	flags := make([]bool, limit)
	for i := 0; i < limit; i++ {
		flags[i] = true
	}
	flags[0], flags[1] = false, false
	for p := 2; p < limit; p++ {
		if flags[p] {
			for j := p * p; j < limit; j += p {
				flags[j] = false
			}
		}
	}
	var n uint32
	for _, f := range flags {
		if f {
			n++
		}
	}
	return n
}

func expectations(work int) []workload {
	fibN := 28
	if work > 1 {
		fibN = 34
	}
	return []workload{
		{"fib.elf", fmt.Sprintf("fib(%d)", fibN), fib(fibN)},
		{"bench_pi.elf", "pi (Q16.16, N=10000)", leibniz()},
		{"bench_sort.elf", "bubble sort", 0},
		{"bench_sieve.elf", "sieve(1001)", sieve()},
	}
}

type workload struct {
	name  string
	label string
	want  uint32
}

func buildGenerators() error {
	for _, name := range []string{"genbench", "genfib"} {
		out := filepath.Join(build, exeName(name))
		if _, err := runOutDir(benchdata, "go", "build", "-buildvcs=false", "-o", out, "./"+name); err != nil {
			return err
		}
	}
	return nil
}

func generate(machine string, out string, work int) error {
	m := machines[machine]
	if err := os.MkdirAll(out, 0o755); err != nil {
		return err
	}
	common := []string{"-base", fmt.Sprintf("0x%X", m.base), "-stack", fmt.Sprintf("0x%X", m.stack),
		"-uart", fmt.Sprintf("0x%X", m.uart), "-exit", m.exit}
	genArgs := append([]string{"-out", out, "-data", fmt.Sprintf("0x%X", m.data),
		"-work", strconv.Itoa(work)}, common...)
	if _, err := runOut(genbench, genArgs...); err != nil {
		return err
	}
	fibN := 28
	if work > 1 {
		fibN = 34
	}
	fibArgs := append([]string{"-out", filepath.Join(out, "fib.elf"), "-n", strconv.Itoa(fibN)}, common...)
	if _, err := runOut(genfib, fibArgs...); err != nil {
		return err
	}
	fib0Args := append([]string{"-out", filepath.Join(out, "fib0.elf"), "-n", "0"}, common...)
	_, err := runOut(genfib, fib0Args...)
	return err
}

func generateHalt(out string, work int) error {
	if err := os.MkdirAll(out, 0o755); err != nil {
		return err
	}
	common := []string{"-base", "0x40000000", "-stack", "0x3FCDF000", "-exit", "halt"}
	genArgs := append([]string{"-out", out, "-data", "0x3FC80000",
		"-work", strconv.Itoa(work)}, common...)
	if _, err := runOut(genbench, genArgs...); err != nil {
		return err
	}
	fibN := 28
	if work > 1 {
		fibN = 34
	}
	fibArgs := append([]string{"-out", filepath.Join(out, "fib.elf"), "-n", strconv.Itoa(fibN)}, common...)
	_, err := runOut(genfib, fibArgs...)
	return err
}

func runOnce(qemu string, machine string, elf string, timeout time.Duration) (uint32, bool, float64) {
	m := machines[machine]
	args := []string{"-M", m.machine, "-kernel", elf, "-display", "none",
		"-serial", "stdio", "-monitor", "none"}
	args = append(args, m.extra...)
	cmd := exec.Command(qemu, args...)
	t0 := time.Now()

	var text string
	if m.stop == "exit" {
		out, err := cmd.CombinedOutput()
		dt := time.Since(t0).Seconds()
		text = strings.TrimSpace(string(out))
		_ = err
		v, perr := strconv.ParseUint(text, 16, 32)
		return uint32(v), perr == nil, dt
	}

	// esp32c3: the image spins after printing a0, so read one line and kill.
	stdout, _ := cmd.StdoutPipe()
	cmd.Stderr = io.Discard
	_ = cmd.Start()
	var buf []byte
	done := make(chan struct{})
	go func() {
		one := make([]byte, 1)
		for {
			n, err := stdout.Read(one)
			if n > 0 {
				buf = append(buf, one[:n]...)
				if one[0] == '\n' {
					break
				}
			}
			if err != nil {
				break
			}
		}
		close(done)
	}()
	select {
	case <-done:
	case <-time.After(timeout):
	}
	_ = cmd.Process.Kill()
	_ = cmd.Wait()
	dt := time.Since(t0).Seconds()
	text = strings.TrimSpace(string(buf))
	v, perr := strconv.ParseUint(text, 16, 32)
	return uint32(v), perr == nil, dt
}

func bestTime(qemu string, machine string, elf string, reps int, timeout time.Duration) (uint32, bool, float64) {
	best := 0.0
	value := uint32(0)
	got := false
	for i := 0; i < reps; i++ {
		v, ok, dt := runOnce(qemu, machine, elf, timeout)
		if ok {
			value = v
			got = true
		}
		if i == 0 || dt < best {
			best = dt
		}
	}
	return value, got, best
}

func cmdCheck(args []string) int {
	fs := flag.NewFlagSet("check", flag.ExitOnError)
	machine := fs.String("machine", "auto", "auto|esp32c3|virt")
	qemu := fs.String("qemu", "", "qemu-system-riscv32 for -machine virt")
	espQemu := fs.String("esp-qemu", os.Getenv("ESP_QEMU"), "Espressif qemu (default: "+defaultEspQEMU+")")
	reps := fs.Int("reps", 3, "repetitions")
	work := fs.Int("work", 15, "workload multiplier")
	noBuild := fs.Bool("no-build", false, "reuse existing generators")
	noTime := fs.Bool("no-time", false, "skip the timing pass")
	timeout := fs.Float64("timeout", 120.0, "per-run timeout (s)")
	_ = fs.Parse(args)

	esp := findBinary(*espQemu, []string{defaultEspQEMU, "qemu-system-riscv32-esp", "qemu-system-riscv32"})
	if esp != "" && !supportsEsp32C3(esp) {
		esp = ""
	}
	virt := findBinary(*qemu, []string{"qemu-system-riscv32"})

	m := *machine
	if m == "auto" {
		if esp != "" {
			m = "esp32c3"
		} else {
			m = "virt"
		}
	}
	q := esp
	if m == "virt" {
		q = virt
	}
	if q == "" {
		fmt.Fprintf(os.Stderr, "no QEMU with -M %s found\n", m)
		if m == "esp32c3" {
			fmt.Fprintln(os.Stderr, "install Espressif's QEMU or set ESP_QEMU")
		}
		return 2
	}

	if !*noBuild {
		if err := buildGenerators(); err != nil {
			fmt.Fprintln(os.Stderr, err)
			return 2
		}
	}
	outDir := filepath.Join(build, "qemu-"+m)
	if err := generate(m, outDir, *work); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 2
	}
	expect := expectations(*work)
	spec := machines[m]
	fmt.Printf("machine: %s   qemu: %s\n", m, q)
	fmt.Printf("image base=0x%08X stack=0x%08X uart=0x%08X exit=%s\n",
		spec.base, spec.stack, spec.uart, spec.exit)

	counts := map[string]int64{}
	var baseSec float64
	if !*noTime {
		haltDir := filepath.Join(build, "qemu-count")
		if fileExists(cExe) && generateHalt(haltDir, *work) == nil {
			for _, w := range expect {
				elf := filepath.Join(haltDir, w.name)
				if n, ok := countInstructions(elf); ok {
					counts[w.name] = n
				}
			}
		} else {
			fmt.Println("(rv32emu-c unavailable: instruction counts and MIPS skipped)")
		}
		_, _, baseSec = bestTime(q, m, filepath.Join(outDir, "fib0.elf"), *reps, time.Duration(*timeout)*time.Second)
		fmt.Printf("startup baseline (fib(0)): %.3f s\n", baseSec)
	}
	fmt.Println()

	failed := 0
	if *noTime {
		fmt.Printf("%-22s %-12s %-12s  %s\n", "workload", "expected", "qemu", "result")
	} else {
		fmt.Printf("%-22s %-12s %-12s %10s %8s %9s  %s\n",
			"workload", "expected", "qemu", "inst", "sec", "MIPS", "result")
	}
	for _, w := range expect {
		elf := filepath.Join(outDir, w.name)
		got, ok, sec := bestTime(q, m, elf, *reps, time.Duration(*timeout)*time.Second)
		pass := ok && got == w.want
		if !pass {
			failed++
		}
		var inst string
		mips := "-"
		if !*noTime {
			if n, has := counts[w.name]; has {
				inst = fmt.Sprintf("%d", n)
				if sec > baseSec {
					mips = fmt.Sprintf("%.1f", float64(n)/(sec-baseSec)/1e6)
				}
			} else {
				inst = "-"
			}
			res := "FAIL"
			if pass {
				res = "PASS"
			} else if !ok {
				res = "FAIL (no result)"
			}
			fmt.Printf("%-22s 0x%08X   0x%08X  %10s %8.3f %9s   %s\n",
				w.label, w.want, got, inst, sec, mips, res)
		} else {
			res := "FAIL"
			if pass {
				res = "PASS"
			} else if !ok {
				res = "FAIL (no result)"
			}
			fmt.Printf("%-22s 0x%08X   0x%08X   %s\n", w.label, w.want, got, res)
		}
	}

	fmt.Println()
	if failed > 0 {
		fmt.Printf("%d workload(s) failed\n", failed)
		return 1
	}
	fmt.Printf("all %d workloads match the reference values\n", len(expect))
	if !*noTime && len(counts) > 0 {
		fmt.Println("MIPS = instructions / (wall - fib(0) baseline); QEMU is TCG, not an interpreter")
	}
	return 0
}
