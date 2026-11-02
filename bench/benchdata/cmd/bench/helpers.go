package main

import (
	"bytes"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"time"
)

// ---- paths ----

func exeName(base string) string {
	if runtime.GOOS == "windows" {
		return base + ".exe"
	}
	return base
}

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

var (
	root      = findRoot()
	build     = filepath.Join(root, "build")
	benchdata = filepath.Join(root, "bench", "benchdata")
	cExe      = filepath.Join(build, exeName("rv32emu-c"))
	genbench  = filepath.Join(build, exeName("genbench"))
	genfib    = filepath.Join(build, exeName("genfib"))
	genwork   = filepath.Join(build, exeName("genwork"))
	defaultEspQEMU = filepath.Join(`C:\Users\SONG\Tools\esp-qemu\qemu\bin`, exeName("qemu-system-riscv32"))
	defaultTinyOS  = filepath.Join(`C:\Users\SONG\Documents\Projects\TinyOS`, "build")
)

// ---- subprocess helpers ----

func runOut(name string, args ...string) (string, error) {
	var stdout, stderr bytes.Buffer
	cmd := exec.Command(name, args...)
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr
	err := cmd.Run()
	if err != nil {
		return stdout.String(), fmt.Errorf("%s: %v: %s", name, err, strings.TrimSpace(stderr.String()))
	}
	return stdout.String(), nil
}

func runOutDir(dir string, name string, args ...string) (string, error) {
	var stdout, stderr bytes.Buffer
	cmd := exec.Command(name, args...)
	cmd.Dir = dir
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr
	err := cmd.Run()
	if err != nil {
		return stdout.String(), fmt.Errorf("%s: %v: %s", name, err, strings.TrimSpace(stderr.String()))
	}
	return stdout.String(), nil
}

func fileExists(p string) bool {
	_, err := os.Stat(p)
	return err == nil
}

func findBinary(explicit string, candidates []string) string {
	if explicit != "" {
		if fileExists(explicit) || strings.ContainsRune(explicit, os.PathSeparator) {
			if fileExists(explicit) {
				return explicit
			}
		} else if p, err := exec.LookPath(explicit); err == nil {
			return p
		}
	}
	for _, c := range candidates {
		if c == "" {
			continue
		}
		if filepath.IsAbs(c) {
			if fileExists(c) {
				return c
			}
		} else if p, err := exec.LookPath(c); err == nil {
			return p
		}
	}
	return ""
}

func supportsEsp32C3(qemu string) bool {
	out, err := runOut(qemu, "-M", "help")
	if err != nil {
		return false
	}
	return strings.Contains(out, "esp32c3")
}

// ---- instruction counting on rv32emu-c (halt mode) ----

var stoppedRe = regexp.MustCompile(`stopped after (\d+) inst`)

func countInstructions(elf string) (int64, bool) {
	if !fileExists(cExe) || !fileExists(elf) {
		return 0, false
	}
	out, err := runOut(cExe, "-elf", elf, "-quiet", "-regs")
	if err != nil {
		return 0, false
	}
	m := stoppedRe.FindStringSubmatch(out)
	if m == nil {
		return 0, false
	}
	n, _ := strconv.ParseInt(m[1], 10, 64)
	return n, true
}

// ---- pump: drain a subprocess' stdout on a goroutine ----

type pump struct {
	ch chan []byte
}

func newPump(r io.Reader) *pump {
	p := &pump{ch: make(chan []byte, 16)}
	go func() {
		buf := make([]byte, 65536)
		for {
			n, err := r.Read(buf)
			if n > 0 {
				b := make([]byte, n)
				copy(b, buf[:n])
				p.ch <- b
			}
			if err != nil {
				break
			}
		}
		close(p.ch)
	}()
	return p
}

func (p *pump) read(timeout time.Duration) ([]byte, bool) {
	if timeout < 0 {
		timeout = 0
	}
	t := time.NewTimer(timeout)
	defer t.Stop()
	select {
	case b, ok := <-p.ch:
		if !ok {
			return nil, false
		}
		return b, true
	case <-t.C:
		return nil, true
	}
}

// session is a subprocess with pipes, used by the QEMU/TinyOS drivers.
type session struct {
	cmd   *exec.Cmd
	stdin io.WriteCloser
	pump  *pump
	buf   []byte
	mu    sync.Mutex
	eof   bool
}

func startSession(cmd *exec.Cmd) (*session, error) {
	stdin, err := cmd.StdinPipe()
	if err != nil {
		return nil, err
	}
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return nil, err
	}
	stderr, err := cmd.StderrPipe()
	if err != nil {
		return nil, err
	}
	if err := cmd.Start(); err != nil {
		return nil, err
	}
	return &session{cmd: cmd, stdin: stdin, pump: newPump(io.MultiReader(stdout, stderr))}, nil
}

func (s *session) read(timeout time.Duration) ([]byte, bool) {
	b, ok := s.pump.read(timeout)
	if b != nil {
		s.mu.Lock()
		s.buf = append(s.buf, b...)
		s.mu.Unlock()
	}
	if !ok {
		s.eof = true
	}
	return b, ok
}

func (s *session) waitFor(needle []byte, timeout time.Duration) (bool, time.Duration) {
	start := time.Now()
	deadline := start.Add(timeout)
	for {
		remaining := time.Until(deadline)
		if remaining <= 0 {
			break
		}
		if remaining > 100*time.Millisecond {
			remaining = 100 * time.Millisecond
		}
		_, ok := s.read(remaining)
		if !ok {
			break
		}
		s.mu.Lock()
		hit := bytes.Contains(s.buf, needle)
		s.mu.Unlock()
		if hit {
			return true, time.Since(start)
		}
	}
	return false, time.Since(start)
}

func (s *session) text() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return string(s.buf)
}

// tail returns the last n bytes (all when n <= 0).
func (s *session) tail(n int) string {
	s.mu.Lock()
	defer s.mu.Unlock()
	if n <= 0 || len(s.buf) <= n {
		return string(s.buf)
	}
	return string(s.buf[len(s.buf)-n:])
}

func (s *session) sendLine(line, newline string) error {
	_, err := s.stdin.Write([]byte(line + newline))
	return err
}

func (s *session) close() {
	if s.cmd.Process != nil {
		_ = s.cmd.Process.Kill()
		_ = s.cmd.Wait()
	}
	_ = s.stdin.Close()
}
