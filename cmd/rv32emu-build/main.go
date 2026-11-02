// rv32emu-build - one portable build entry for rv32emu-c (replaces build.ps1
// and build_asan.ps1; PowerShell is gone).  Builds the emulator, the DLL used
// by the leak drivers, the ASan flavour, the leak drivers and the Go bench
// generators/tool, on any platform that has gcc (or clang) and Go.
//
//	go run ./cmd/rv32emu-build           build everything
//	go run ./cmd/rv32emu-build emu       only build/rv32emu-c(.exe)
package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
)

var (
	root  = findRoot()
	build = filepath.Join(root, "build")
)

func findRoot() string {
	dir, err := os.Getwd()
	if err != nil {
		dir = "."
	}
	for {
		if _, err := os.Stat(filepath.Join(dir, "src", "main.c")); err == nil {
			return dir
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return "."
		}
		dir = parent
	}
}

func exeName(base string) string {
	if runtime.GOOS == "windows" {
		return base + ".exe"
	}
	return base
}

func srcFiles() []string {
	matches, _ := filepath.Glob(filepath.Join(root, "src", "*.c"))
	// leakunit provides its own main() and must not see src/main.c
	var out []string
	for _, m := range matches {
		if filepath.Base(m) == "main.c" {
			continue
		}
		out = append(out, m)
	}
	return out
}

func ccCandidates() []string {
	if p := os.Getenv("CC"); p != "" {
		return []string{p}
	}
	cands := []string{"gcc", "clang"}
	if runtime.GOOS == "windows" {
		cands = append(cands, filepath.Join(`C:\msys64\ucrt64\bin`, "gcc.exe"))
	}
	return cands
}

func findCC() string {
	for _, c := range ccCandidates() {
		if strings.Contains(c, `\`) || strings.Contains(c, "/") {
			if _, err := os.Stat(c); err == nil {
				return c
			}
			continue
		}
		if p, err := exec.LookPath(c); err == nil {
			return p
		}
	}
	return ""
}

func findClang() string {
	if p, err := exec.LookPath("clang"); err == nil {
		return p
	}
	if runtime.GOOS == "windows" {
		if _, err := os.Stat(filepath.Join(`C:\msys64\ucrt64\bin`, "clang.exe")); err == nil {
			return filepath.Join(`C:\msys64\ucrt64\bin`, "clang.exe")
		}
	}
	return ""
}

func run(name string, args ...string) error {
	cmd := exec.Command(name, args...)
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("%s: %w", name, err)
	}
	return nil
}

func buildEmulator() error {
	cc := findCC()
	if cc == "" {
		return fmt.Errorf("no C compiler found (gcc or clang); install MSYS2 UCRT64 or set CC")
	}
	out := filepath.Join(build, exeName("rv32emu-c"))
	fmt.Printf("CC  = %s\nOUT = %s\n", cc, out)
	src := filepath.Join(root, "src", "*.c")
	if err := run(cc, "-std=c11", "-O3", "-DNDEBUG", "-flto", "-Wall", "-Wextra",
		"-o", out, src); err != nil {
		return err
	}
	fmt.Println("OK")
	return nil
}

func buildDLL() error {
	if runtime.GOOS != "windows" {
		return nil
	}
	cc := findCC()
	if cc == "" {
		return fmt.Errorf("no C compiler found")
	}
	out := filepath.Join(build, "rv32emu-c.dll")
	src := filepath.Join(root, "src", "*.c")
	if err := run(cc, "-std=c11", "-O3", "-DNDEBUG", "-DBUILDING_EMU", "-shared",
		"-o", out, src); err != nil {
		return err
	}
	fmt.Println("OK (dll)")
	return nil
}

func buildASan() error {
	clang := findClang()
	if clang == "" {
		fmt.Println("skip ASan: clang not found")
		return nil
	}
	if runtime.GOOS != "windows" {
		fmt.Println("skip ASan: Windows-only build recipe")
		return nil
	}
	out := filepath.Join(build, "asan")
	if err := os.MkdirAll(out, 0o755); err != nil {
		return err
	}
	exe := filepath.Join(out, exeName("rv32emu-c-asan"))
	src := filepath.Join(root, "src", "*.c")
	if err := run(clang, "-fsanitize=address", "-fuse-ld=lld", "-g", "-O1",
		"-o", exe, src); err != nil {
		return err
	}
	rt := filepath.Join(`C:\msys64\ucrt64\bin`, "libclang_rt.asan_dynamic-x86_64.dll")
	if _, err := os.Stat(rt); err == nil {
		data, err := os.ReadFile(rt)
		if err == nil {
			_ = os.WriteFile(filepath.Join(out, filepath.Base(rt)), data, 0o644)
		}
	}
	fmt.Println("OK (asan)")
	return nil
}

func buildTools() error {
	cc := findCC()
	if cc == "" {
		return fmt.Errorf("no C compiler found")
	}
	if err := os.MkdirAll(build, 0o755); err != nil {
		return err
	}
	// leakrun: Windows-only memory accounting (psapi)
	leakrun := filepath.Join(build, exeName("leakrun"))
	if runtime.GOOS == "windows" {
		if err := run(cc, "-O2", "-o", leakrun,
			filepath.Join(root, "tools", "leakrun.c")); err != nil {
			return err
		}
	}
	// leakunit: the core without src/main.c (it provides its own main)
	args := append([]string{"-O2", "-o", filepath.Join(build, exeName("leakunit"))},
		filepath.Join(root, "tools", "leakunit.c"))
	args = append(args, srcFiles()...)
	if err := run(cc, args...); err != nil {
		return err
	}
	fmt.Println("OK (tools)")
	return nil
}

func goRun(dir string, args ...string) error {
	cmd := exec.Command("go", args...)
	cmd.Dir = dir
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("go in %s: %w", dir, err)
	}
	return nil
}

func buildGenerators() error {
	benchdata := filepath.Join(root, "bench", "benchdata")
	for _, name := range []string{"genbench", "genfib", "genwork"} {
		out := filepath.Join(build, exeName(name))
		if err := goRun(benchdata, "build", "-buildvcs=false", "-o", out, "./"+name); err != nil {
			return err
		}
	}
	return nil
}

func buildBenchTool() error {
	benchdata := filepath.Join(root, "bench", "benchdata")
	out := filepath.Join(build, exeName("rv32emu-bench"))
	if err := goRun(benchdata, "build", "-buildvcs=false", "-o", out, "./cmd/bench"); err != nil {
		return err
	}
	return nil
}

func main() {
	if err := os.MkdirAll(build, 0o755); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	target := "all"
	if len(os.Args) > 1 {
		target = os.Args[1]
	}
	steps := map[string][]func() error{
		"emu":   {buildEmulator},
		"all":   {buildEmulator, buildDLL, buildASan, buildTools, buildGenerators, buildBenchTool},
		"dll":   {buildDLL},
		"asan":  {buildASan},
		"tools": {buildTools},
		"gen":   {buildGenerators},
		"bench": {buildBenchTool},
	}
	list, ok := steps[target]
	if !ok {
		fmt.Fprintf(os.Stderr, "unknown target %q (emu|all|dll|asan|tools|gen|bench)\n", target)
		os.Exit(2)
	}
	for _, fn := range list {
		if err := fn(); err != nil {
			fmt.Fprintln(os.Stderr, "build failed:", err)
			os.Exit(1)
		}
	}
}
