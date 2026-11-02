#!/usr/bin/env python3
"""Run the benchmark programs on QEMU, verify their results and time them.

Two machines are supported:

  esp32c3   Espressif's QEMU (github.com/espressif/qemu), which models the real
            ESP32-C3 memory map: code at 0x40000000, DRAM at
            0x3FC80000..0x3FCDFFFF, UART0 at 0x60000000. The images keep the
            same addresses as the rv32emu ones; only the exit sequence differs
            (print a0, then spin - QEMU is stopped by the harness).
            Install: C:/Users/SONG/Tools/esp-qemu (or set ESP_QEMU).

  virt      Upstream qemu-system-riscv32. Its DRAM is at 0x80000000, so the
            programs are re-linked for it and the exit is a SiFive test-device
            poweroff at 0x100000 after printing on the 16550 UART.

In both cases the compute part is the same instruction stream as the images
used by compare.ps1, so the printed value must match the Go/C emulators and an
independent Python model of each algorithm.

Timing: QEMU is a JIT (TCG), so its MIPS is not comparable to the interpreter
numbers from compare.ps1 - it is the other end of the range. Each wall time is
taken as the best of -reps runs minus the wall time of an almost-empty image
(fib(0)), which removes QEMU startup and the harness' own overhead. Instruction
counts are measured by running the same program in halt mode on rv32emu-c.

    python bench/qemu_check.py [-machine auto|esp32c3|virt] [-reps N]
                               [--no-build] [-qemu PATH] [-esp-qemu PATH]
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD = os.path.join(ROOT, "build")
BENCHDATA = os.path.join(HERE, "benchdata")
C_EXE = os.path.join(BUILD, "rv32emu-c.exe" if os.name == "nt" else "rv32emu-c")

DEFAULT_ESP_QEMU = r"C:\Users\SONG\Tools\esp-qemu\qemu\bin\qemu-system-riscv32.exe"

MACHINES = {
    "esp32c3": dict(
        base=0x40000000, stack=0x3FCDF000, data=0x3FC80000,
        uart=0x60000, exit="spin", machine="esp32c3",
        extra=["-m", "8M"], stop="kill",
    ),
    "virt": dict(
        base=0x80000000, stack=0x80100000, data=0x80010000,
        uart=0x10000, exit="uart", machine="virt",
        extra=["-m", "128M", "-bios", "none"], stop="exit",
    ),
}


def exe(name):
    return os.path.join(BUILD, name + (".exe" if os.name == "nt" else ""))


def find_binary(explicit, candidates):
    for cand in ([explicit] if explicit else []) + candidates:
        if not cand:
            continue
        if os.path.isabs(cand):
            if os.path.exists(cand):
                return cand
        else:
            found = shutil.which(cand)
            if found:
                return found
    return None


def supports_esp32c3(qemu):
    try:
        r = subprocess.run([qemu, "-M", "help"], capture_output=True, text=True, timeout=20)
    except (OSError, subprocess.SubprocessError):
        return False
    return "esp32c3" in r.stdout


def build_go():
    go = shutil.which("go")
    if not go:
        print("go not found: cannot build the generators", file=sys.stderr)
        return False
    for name in ("genbench", "genfib"):
        r = subprocess.run([go, "build", "-buildvcs=false", "-o", exe(name), "./" + name],
                           cwd=BENCHDATA, capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stderr, file=sys.stderr)
            return False
    return True


def ensure_c_emulator():
    if os.path.exists(C_EXE):
        return True
    script = os.path.join(ROOT, "build.ps1")
    if not os.path.exists(script):
        return False
    r = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                        "-File", script], capture_output=True, text=True)
    return r.returncode == 0 and os.path.exists(C_EXE)


def generate(machine, out, work):
    m = MACHINES[machine]
    os.makedirs(out, exist_ok=True)
    common = ["-base", hex(m["base"]), "-stack", hex(m["stack"]),
              "-uart", hex(m["uart"]), "-exit", m["exit"]]
    r = subprocess.run([exe("genbench"), "-out", out, "-data", hex(m["data"]),
                        "-work", str(work)] + common,
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr, file=sys.stderr)
        return False
    r = subprocess.run([exe("genfib"), "-out", os.path.join(out, "fib.elf"),
                        "-n", str(28 if work <= 1 else 34)] + common,
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr, file=sys.stderr)
        return False
    # near-empty image used to subtract QEMU startup from every measurement
    r = subprocess.run([exe("genfib"), "-out", os.path.join(out, "fib0.elf"), "-n", "0"] + common,
                       capture_output=True, text=True)
    return r.returncode == 0


def generate_halt(out, work):
    """Halt-mode copies (ebreak) used only to count instructions with rv32emu-c."""
    os.makedirs(out, exist_ok=True)
    common = ["-base", hex(0x40000000), "-stack", hex(0x3FCDF000), "-exit", "halt"]
    r = subprocess.run([exe("genbench"), "-out", out, "-data", hex(0x3FC80000),
                        "-work", str(work)] + common, capture_output=True, text=True)
    if r.returncode != 0:
        return False
    r = subprocess.run([exe("genfib"), "-out", os.path.join(out, "fib.elf"),
                        "-n", str(28 if work <= 1 else 34)] + common,
                       capture_output=True, text=True)
    return r.returncode == 0


def count_instructions(halt_dir, expect):
    """inst per workload, measured on rv32emu-c (halt mode)."""
    counts = {}
    for name, label, _want in expect:
        elf = os.path.join(halt_dir, name)
        if not os.path.exists(elf):
            counts[name] = None
            continue
        r = subprocess.run([C_EXE, "-elf", elf, "-quiet", "-regs"],
                           capture_output=True, text=True, timeout=600)
        m = re.search(r"stopped after (\d+) inst", r.stdout)
        counts[name] = int(m.group(1)) if m else None
    return counts


# ---- reference values, computed from the algorithm definitions ------------
def fib(n=28):
    a, b = 0, 1
    for _ in range(n):
        a, b = b, a + b
    return a


def leibniz(n=10000):
    """bench_pi: Q16.16 sum of the Leibniz series, then a0 = sum << 2."""
    total = 0
    for k in range(n):
        term = 0x10000 // (2 * k + 1)
        total = (total - term) if (k & 1) else (total + term)
    return (total << 2) & 0xFFFFFFFF


def sieve(limit=1001):
    flags = [True] * limit
    flags[0] = flags[1] = False
    for p in range(2, limit):
        if flags[p]:
            for j in range(p * p, limit, p):
                flags[j] = False
    return sum(1 for f in flags if f)


def expectations(work):
    fib_n = 28 if work <= 1 else 34
    return [
        ("fib.elf", "fib(%d)" % fib_n, fib(fib_n)),
        ("bench_pi.elf", "pi (Q16.16, N=10000)", leibniz()),
        ("bench_sort.elf", "bubble sort", 0),
        ("bench_sieve.elf", "sieve(1001)", sieve()),
    ]


def base_cmd(qemu, machine, elf):
    m = MACHINES[machine]
    return [qemu, "-M", m["machine"], "-kernel", elf,
            "-display", "none", "-serial", "stdio", "-monitor", "none"] + m["extra"]


def read_until_newline(proc, deadline):
    out = b""
    fd = proc.stdout
    while time.time() < deadline:
        ch = fd.read(1)
        if not ch:
            break
        if ch == b"\n":
            break
        out += ch
    return out


def run_once(qemu, machine, elf, timeout):
    """Return (value or None, wall seconds)."""
    cmd = base_cmd(qemu, machine, elf)
    t0 = time.perf_counter()
    if MACHINES[machine]["stop"] == "exit":
        try:
            r = subprocess.run(cmd, capture_output=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            return None, time.perf_counter() - t0
        text = r.stdout.decode("ascii", "replace").strip()
    else:
        # esp32c3: the image spins after printing, so read one line and kill it
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        try:
            out = read_until_newline(p, time.time() + timeout)
        finally:
            p.terminate()
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                p.kill()
        text = out.decode("ascii", "replace").strip()
    dt = time.perf_counter() - t0
    try:
        return int(text, 16), dt
    except ValueError:
        return None, dt


def best_time(qemu, machine, elf, reps, timeout):
    best = None
    value = None
    for _ in range(max(1, reps)):
        v, dt = run_once(qemu, machine, elf, timeout)
        if v is not None:
            value = v
        if best is None or dt < best:
            best = dt
    return value, best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-machine", choices=["auto", "esp32c3", "virt"], default="auto")
    ap.add_argument("-qemu", default="", help="qemu-system-riscv32 for -machine virt")
    ap.add_argument("-esp-qemu", default=os.environ.get("ESP_QEMU", ""),
                    help="Espressif qemu-system-riscv32 (default: %s)" % DEFAULT_ESP_QEMU)
    ap.add_argument("-reps", type=int, default=3)
    ap.add_argument("--no-build", action="store_true", help="reuse existing generators")
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("--no-time", action="store_true", help="skip the timing pass")
    ap.add_argument("-work", type=int, default=15,
                    help="multiply the workload size so the run is long enough to time")
    args = ap.parse_args()

    esp = find_binary(args.esp_qemu,
                      [DEFAULT_ESP_QEMU, "qemu-system-riscv32-esp", "qemu-system-riscv32"])
    if esp and not supports_esp32c3(esp):
        esp = None
    virt = find_binary(args.qemu, ["qemu-system-riscv32"])

    machine = args.machine
    if machine == "auto":
        machine = "esp32c3" if esp else "virt"
    qemu = esp if machine == "esp32c3" else virt
    if not qemu:
        print("no QEMU with -M %s found" % machine, file=sys.stderr)
        if machine == "esp32c3":
            print("install Espressif's QEMU or set ESP_QEMU (see the bench README)", file=sys.stderr)
        return 2

    if not args.no_build and not build_go():
        return 2
    out_dir = os.path.join(BUILD, "qemu-" + machine)
    if not generate(machine, out_dir, args.work):
        return 2
    expect = expectations(args.work)

    m = MACHINES[machine]
    print("machine: %s   qemu: %s" % (machine, qemu))
    print("image base=0x%08X stack=0x%08X uart=0x%08X exit=%s" % (
        m["base"], m["stack"], m["uart"], m["exit"]))

    counts = {}
    if not args.no_time:
        halt_dir = os.path.join(BUILD, "qemu-count")
        if ensure_c_emulator() and generate_halt(halt_dir, args.work):
            counts = count_instructions(halt_dir, expect)
        else:
            print("(rv32emu-c unavailable: instruction counts and MIPS skipped)")
        _, base_sec = best_time(qemu, machine, os.path.join(out_dir, "fib0.elf"), args.reps, args.timeout)
        print("startup baseline (fib(0)): %.3f s" % (base_sec or 0))
    print()
    if args.no_time:
        print("%-22s %-12s %-12s  %s" % ("workload", "expected", "qemu", "result"))
    else:
        print("%-22s %-12s %-12s %10s %8s %9s  %s" % (
            "workload", "expected", "qemu", "inst", "sec", "MIPS", "result"))

    failed = 0
    for name, label, want in expect:
        elf = os.path.join(out_dir, name)
        got, sec = best_time(qemu, machine, elf, args.reps, args.timeout)
        ok = got == want
        failed += 0 if ok else 1
        if args.no_time:
            print("%-22s 0x%08X   0x%08X   %s" % (
                label, want, got if got is not None else 0,
                "PASS" if ok else ("FAIL (no result)" if got is None else "FAIL")))
            continue
        inst = counts.get(name)
        mips = None
        if inst and sec and base_sec is not None and sec > base_sec:
            mips = inst / (sec - base_sec) / 1e6
        print("%-22s 0x%08X   0x%08X  %10s %8.3f %9s   %s" % (
            label, want, got if got is not None else 0,
            inst if inst else "-", sec or 0,
            ("%.1f" % mips) if mips else "-",
            "PASS" if ok else ("FAIL (no result)" if got is None else "FAIL")))

    print()
    if failed:
        print("%d workload(s) failed" % failed)
        return 1
    print("all %d workloads match the reference values" % len(expect))
    if not args.no_time and counts:
        print("MIPS = instructions / (wall - fib(0) baseline); QEMU is TCG, not an interpreter")
    return 0


if __name__ == "__main__":
    sys.exit(main())
