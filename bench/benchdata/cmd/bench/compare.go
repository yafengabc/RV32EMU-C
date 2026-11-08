package main

import (
	"bufio"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"strconv"
	"strings"
)

type workItem struct {
	name, file string
	ramdisk    string
	best       bool
}

type row struct {
	name          string
	inst          int64
	goMips, cMips float64
	ratio         float64
	note          string
}

// cmdCompare ports bench/compare.ps1: the Go vs C emulator throughput table.
// The Go reference emulator lives in its own repository (RV32EMU); its
// internal packages are copied into a scratch module so `go build` works
// without modifying the checkout.
func cmdCompare(args []string) int {
	fs := flag.NewFlagSet("compare", flag.ExitOnError)
	reps := fs.Int("reps", 7, "repetitions per workload (median)")
	max := fs.Uint64("max", 20000000, "instruction budget for non-self-terminating workloads")
	includeTinyOS := fs.Bool("tinyos", false, "also time a TinyOS boot")
	includeUnicorn := fs.Bool("unicorn", false, "also run the Unicorn C harness")
	tinyosElf := fs.String("tinyos-elf", filepath.Join(defaultTinyOS, "kernel-esp32c3.elf"), "")
	tinyosRamdisk := fs.String("tinyos-ramdisk", filepath.Join(defaultTinyOS, "ramdisk-esp32c3.bin"), "")
	goRepo := fs.String("go-repo", "", "checkout of github.com/yafengabc/RV32EMU")
	skipBuild := fs.Bool("skip-build", false, "reuse existing build/rv32emu-c.exe and generators")
	_ = fs.Parse(args)

	if !*skipBuild {
		if err := buildCEmulator(); err != nil {
			fmt.Fprintln(os.Stderr, err)
			return 1
		}
	}

	repo := resolveGoRepo(*goRepo)
	if repo == "" {
		fmt.Fprintln(os.Stderr, "Go checkout not found. Pass -go-repo <path> or set RV32EMU_GO_REPO.")
		return 1
	}
	fmt.Println("Go repo:", repo)

	// generators
	for _, name := range []string{"genbench", "genfib", "genwork"} {
		out := filepath.Join(build, exeName(name))
		if _, err := runOutDir(benchdata, "go", "build", "-buildvcs=false", "-o", out, "./"+name); err != nil {
			fmt.Fprintln(os.Stderr, err)
			return 1
		}
	}

	goBench := filepath.Join(build, exeName("benchrun-go"))
	if err := stageGoBench(repo, goBench); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}

	// workloads
	data := filepath.Join(build, "benchdata")
	if err := os.MkdirAll(data, 0o755); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	if _, err := runOut(genbench, "-out", data); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	if _, err := runOut(genfib, "-out", data); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	if _, err := runOut(genwork, "-out", data, "-n", strconv.Itoa(2_000_000)); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}

	workloads := []workItem{
		{"fib(28)", "fib.elf", "", false},
		{"pi (Leibniz)", "pi_big.elf", "", true},
		{"ALU loop", "alu_loop.elf", "", true},
		{"memory loop", "mem_loop.elf", "", true},
		{"bubble sort", "bench_sort.elf", "", false},
		{"sieve", "bench_sieve.elf", "", false},
	}
	if *includeTinyOS && fileExists(*tinyosElf) && fileExists(*tinyosRamdisk) {
		workloads = append(workloads, workItem{"TinyOS boot", *tinyosElf, *tinyosRamdisk, false})
	}

	var rows []row

	for _, w := range workloads {
		file := filepath.Join(data, w.file)
		if w.ramdisk != "" {
			file = w.file
		}
		if !fileExists(file) {
			fmt.Printf("bench %s ... (missing %s, skipped)\n", w.name, file)
			continue
		}
		fmt.Printf("bench %s ...\n", w.name)
		workMax := *max
		if w.best && (workMax == 0 || workMax > 2_000_000) {
			workMax = 2_000_000
		}
		goArgs := []string{"-elf", file, "-max", strconv.FormatUint(workMax, 10), "-reps", strconv.Itoa(*reps)}
		cArgs := []string{"-elf", file, "-bench", strconv.FormatUint(workMax, 10), "-reps", strconv.Itoa(*reps), "-quiet"}
		if w.ramdisk != "" {
			goArgs = append(goArgs, "-ramdisk", w.ramdisk)
			cArgs = append(cArgs, "-ramdisk", w.ramdisk)
		}
		goMips, _, gok := getBench(goBench, goArgs, w.best)
		cMips, cInst, cok := getBench(cExe, cArgs, w.best)
		if !gok || !cok {
			fmt.Printf("  (benchmark failed: go=%v c=%v)\n", gok, cok)
			continue
		}
		name := w.name
		if w.best {
			name = fmt.Sprintf("%s (%.1fM inst)", w.name, float64(workMax)/1e6)
		}
		r := row{name: name, inst: cInst, goMips: goMips, cMips: cMips, ratio: cMips / goMips}
		rows = append(rows, r)
	}

	if *includeUnicorn {
		uc := runUnicornC(workloads)
		rows = append(rows, uc...)
	}

	// report
	cpuName := "unknown CPU"
	lines := []string{
		"# Go vs C emulator benchmark",
		"",
		fmt.Sprintf("Host: %s, %d logical CPUs", cpuName, runtime.NumCPU()),
		fmt.Sprintf("Reps: %d (median), pure in-process RunHot/1000 batches", *reps),
		"",
		"| workload | instructions | Go MIPS | C MIPS | C/Go | Unicorn |",
		"|---|---:|---:|---:|---:|---|",
	}
	for _, r := range rows {
		goS, cS, ratio, note := "-", "-", "-", "-"
		if r.goMips > 0 {
			goS = fmt.Sprintf("%.2f", r.goMips)
		}
		if r.cMips > 0 {
			cS = fmt.Sprintf("%.2f", r.cMips)
		}
		if r.ratio > 0 {
			ratio = fmt.Sprintf("%.2fx", r.ratio)
		}
		if r.note != "" {
			note = r.note
		}
		lines = append(lines, fmt.Sprintf("| %s | %d | %s | %s | %s | %s |", r.name, r.inst, goS, cS, ratio, note))
	}
	lines = append(lines,
		"",
		"Ratio > 1 means the C port is faster. Tight loops run in a 2M-instruction",
		"window (best of three runs) because Go occasionally slows down when run",
		"right up to a 20M cap; self-terminating programs use their natural length.",
	)
	if *includeUnicorn {
		lines = append(lines,
			"Unicorn 2.1.4 cannot run these images (branch handling inside long TBs);",
			"see bench/README.md for the evidence and for the straight-line probe.")
	}
	report := filepath.Join(root, "bench", "results.md")
	if err := os.WriteFile(report, []byte(strings.Join(lines, "\n")+"\n"), 0o644); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	fmt.Println(strings.Join(lines, "\n"))
	fmt.Println()
	fmt.Println("saved:", report)
	return 0
}

func buildCEmulator() error {
	cmd := exec.Command("go", "run", "./cmd/rv32emu-build")
	cmd.Dir = root
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	return cmd.Run()
}

func resolveGoRepo(explicit string) string {
	raw := explicit
	if raw == "" {
		raw = firstNonComment(filepath.Join(root, "bench", "go-repo.txt"))
	}
	if raw == "" {
		raw = os.Getenv("RV32EMU_GO_REPO")
	}
	for _, c := range []string{raw, filepath.Join(root, "..", raw)} {
		if c != "" && fileExists(filepath.Join(c, "go.mod")) {
			if abs, err := filepath.Abs(c); err == nil {
				return abs
			}
		}
	}
	if c := filepath.Join(root, "..", "RV32EMU"); fileExists(filepath.Join(c, "go.mod")) {
		return c
	}
	if c := filepath.Join(root, ".."); fileExists(filepath.Join(c, "go.mod")) {
		return c
	}
	return ""
}

func firstNonComment(path string) string {
	f, err := os.Open(path)
	if err != nil {
		return ""
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line != "" && !strings.HasPrefix(line, "#") {
			return line
		}
	}
	return ""
}

// stageGoBench copies the Go repo's internal packages and tools/benchrun into
// a scratch module named "rv32emu" and builds build/benchrun-go(.exe).  The
// benchrun harness is copied verbatim so codegen matches the Go repository.
func stageGoBench(repo, out string) error {
	mod := goDirective(repo)
	stage, err := os.MkdirTemp("", "rv32emu-bench-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(stage)

	goMod := "module rv32emu\n\n" + mod + "\n"
	if err := os.WriteFile(filepath.Join(stage, "go.mod"), []byte(goMod), 0o644); err != nil {
		return err
	}
	for _, pkg := range []string{"cpu", "elf", "mem", "perip", "rom", "soc"} {
		from := filepath.Join(repo, "internal", pkg)
		if !fileExists(filepath.Join(from, pkg+".go")) && !dirHasGo(from) {
			return fmt.Errorf("missing emulator package: %s (is -go-repo a checkout of RV32EMU?)", from)
		}
		if err := copyDir(from, filepath.Join(stage, "internal", pkg)); err != nil {
			return err
		}
	}
	dst := filepath.Join(stage, "tools", "benchrun")
	if err := os.MkdirAll(dst, 0o755); err != nil {
		return err
	}
	src := filepath.Join(benchdata, "benchrun", "main.go")
	data, err := os.ReadFile(src)
	if err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(dst, "main.go"), data, 0o644); err != nil {
		return err
	}
	_, err = runOutDir(stage, "go", "build", "-buildvcs=false", "-o", out, "./tools/benchrun")
	return err
}

func goDirective(repo string) string {
	data, err := os.ReadFile(filepath.Join(repo, "go.mod"))
	if err != nil {
		return "go 1.22"
	}
	re := regexp.MustCompile(`(?m)^go\s+(.+)$`)
	if m := re.FindSubmatch(data); m != nil {
		return "go " + strings.TrimSpace(string(m[1]))
	}
	return "go 1.22"
}

func dirHasGo(dir string) bool {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return false
	}
	for _, e := range entries {
		if !e.IsDir() && strings.HasSuffix(e.Name(), ".go") {
			return true
		}
	}
	return false
}

func copyDir(from, to string) error {
	if err := os.MkdirAll(to, 0o755); err != nil {
		return err
	}
	entries, err := os.ReadDir(from)
	if err != nil {
		return err
	}
	for _, e := range entries {
		s := filepath.Join(from, e.Name())
		d := filepath.Join(to, e.Name())
		if e.IsDir() {
			if err := copyDir(s, d); err != nil {
				return err
			}
		} else {
			data, err := os.ReadFile(s)
			if err != nil {
				return err
			}
			if err := os.WriteFile(d, data, 0o644); err != nil {
				return err
			}
		}
	}
	return nil
}

var medianRe = regexp.MustCompile(`\[bench\] median sec=[0-9.]+ inst=(\d+) mips=([0-9.]+)`)

// getBench runs one emulator binary and returns (mips, inst, ok).  Tight loops
// take the best of three runs (their numbers are the noisiest).
func getBench(exe string, args []string, best bool) (float64, int64, bool) {
	run := func() (float64, int64, bool) {
		out, err := runOut(exe, args...)
		if err != nil {
			return 0, 0, false
		}
		m := medianRe.FindStringSubmatch(out)
		if m == nil {
			return 0, 0, false
		}
		inst, _ := strconv.ParseInt(m[1], 10, 64)
		mips, _ := strconv.ParseFloat(m[2], 64)
		return mips, inst, true
	}
	if !best {
		return run()
	}
	bestMips, inst, ok := 0.0, int64(0), false
	for i := 0; i < 3; i++ {
		mips, in, k := run()
		if k && (i == 0 || mips > bestMips) {
			bestMips, inst, ok = mips, in, true
		}
	}
	return bestMips, inst, ok
}

// runUnicornC builds bench/bench_unicorn.c (needs the unicorn dev library)
// and runs it over the workloads that are not TinyOS.  Unicorn 2.1.4 cannot
// run these images, so the rows are mostly "did not run" notes.
func runUnicornC(workloads []workItem) []row {
	var rows []row
	cc := ""
	if p, err := exec.LookPath("gcc"); err == nil {
		cc = p
	}
	if cc == "" {
		fmt.Println("(bench_unicorn.exe not available: gcc not found)")
		return rows
	}
	out := filepath.Join(build, exeName("bench_unicorn"))
	src := filepath.Join(root, "bench", "bench_unicorn.c")
	if _, err := runOut(cc, "-O2", "-o", out, src,
		"-I", `C:/msys64/ucrt64/include`, "-L", `C:/msys64/ucrt64/lib`, "-lunicorn"); err != nil {
		fmt.Println("(bench_unicorn.exe not available: unicorn dev files missing)")
		return rows
	}
	for _, w := range workloads {
		if w.ramdisk != "" {
			continue
		}
		file := filepath.Join(build, "benchdata", w.file)
		if !fileExists(file) {
			continue
		}
		instReq := uint64(2_000_000)
		outText, err := runOut(out, "-elf", file, "-reps", "5", "-max", strconv.FormatUint(instReq, 10))
		if err != nil {
			continue
		}
		note := "did not run (Unicorn 2.1.4 branch bug)"
		if m := regexp.MustCompile(`counted=(\d+)`).FindStringSubmatch(outText); m != nil {
			counted, _ := strconv.ParseUint(m[1], 10, 64)
			if counted >= instReq {
				if mm := regexp.MustCompile(`\[bench\] unicorn median.*?mips=([0-9.]+)`).FindStringSubmatch(outText); mm != nil {
					note = mm[1] + " MIPS"
				}
			}
		}
		rows = append(rows, row{name: w.name, inst: int64(instReq), note: note})
	}
	return rows
}
