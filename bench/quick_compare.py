#!/usr/bin/env python3
"""Quick side-by-side: same ROM image for Go, C, qemu-system-riscv32.

The image is the TinyGo-style benchmark payload (`firmware/bench/main.go`
algorithms) compiled into a freestanding RV32IMC binary; all emulators start
it through their own reset vector, so nothing differs except the emulator.
Only wall-clock "how long until it prints DONE" is compared.

Images (built by gen_quick.py):
    quick_fib.elf    recursive fib(28)              -> a0 = 317811
    quick_sort.elf   bubble-sort 16 bytes x 20000   -> a0 = first element
    quick_sieve.elf  sieve to 50000 x 50            -> a0 = prime count

The payload ends with `ebreak`, which the Go/C emulators treat as "halt"; for
QEMU the harness uses `-kernel` + a semihosting `ebreak` (QEMU exits).
"""
import argparse
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
REPO = os.path.dirname(ROOT)
QEMU = r"C:\msys64\ucrt64\bin\qemu-system-riscv32.exe"
C_EXE = os.path.join(ROOT, "build", "rv32emu-c.exe")
GO_EXE = os.path.join(ROOT, "build", "rv32emu-go.exe")


def have(p):
    return os.path.exists(p)


def run_go(path, timeout=120):
    t0 = time.perf_counter()
    r = subprocess.run([GO_EXE, "-elf", path], capture_output=True, text=True,
                       timeout=timeout)
    dt = time.perf_counter() - t0
    hit = "DONE" in r.stdout
    return dt, hit, r.stdout


def run_c(path, timeout=120):
    t0 = time.perf_counter()
    r = subprocess.run([C_EXE, "-elf", path, "-quiet", "-regs"], capture_output=True,
                       text=True, timeout=timeout)
    dt = time.perf_counter() - t0
    hit = "DONE" in r.stdout
    return dt, hit, r.stdout


def run_qemu(path, timeout=120):
    t0 = time.perf_counter()
    try:
        r = subprocess.run([QEMU, "-M", "virt", "-m", "128", "-bios", "none",
                            "-kernel", path, "-display", "none",
                            "-serial", "stdio", "-monitor", "none"],
                           capture_output=True, text=True, timeout=timeout)
        out = r.stdout
        hit = "DONE" in out
    except subprocess.TimeoutExpired:
        dt = time.perf_counter() - t0
        return dt, False, ""
    return time.perf_counter() - t0, hit, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=os.path.join(ROOT, "build", "quick"))
    ap.add_argument("--reps", type=int, default=3)
    args = ap.parse_args()

    images = ["quick_fib.elf", "quick_sort.elf", "quick_sieve.elf"]
    runners = [("Go", run_go), ("C", run_c), ("QEMU", run_qemu)]
    print(f"{'image':<16}{'Go':>10}{'C':>10}{'QEMU':>10}   (seconds, best of "
          f"{args.reps}; x = did not reach DONE)")
    for img in images:
        path = os.path.join(args.dir, img)
        if not os.path.exists(path):
            print(f"{img:<16}  missing")
            continue
        row = f"{img[:-4]:<16}"
        for name, fn in runners:
            best = None
            ok = False
            for _ in range(args.reps):
                dt, hit, _out = fn(path)
                ok = ok or hit
                if best is None or dt < best:
                    best = dt
            row += f"{best:>9.3f}{'' if ok else 'x':<1}"
        print(row)


if __name__ == "__main__":
    main()
