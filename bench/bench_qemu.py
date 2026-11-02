#!/usr/bin/env python3
"""Run benchmark images under qemu-system-riscv32 -M virt.

qemu-system-riscv32 is installed with MSYS2 (11.x); -kernel loads an ELF32
RV32IMC image at 0x80000000, UART0 is a 16550 at 0x10000000 (`-serial stdio`).

    python bench_qemu.py -elf FILE [-reps R]

Measured: wall clock of the whole QEMU process, from start to the image's
final `ecall`/`ebreak` (QEMU exits non-zero / empty console after it), so the
number includes QEMU start-up and I/O. For QEMU 11 the translation cache is
already warm for the second run; `-reps` reports every run.
"""
import argparse
import subprocess
import sys
import time

QEMU = r"C:\msys64\ucrt64\bin\qemu-system-riscv32.exe"
BASE = 0x80000000
RAM = 128 * 1024 * 1024


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-elf", required=True)
    ap.add_argument("-reps", type=int, default=5)
    ap.add_argument("-label", default="")
    args = ap.parse_args()
    times = []
    for _ in range(args.reps):
        cmd = [QEMU, "-M", "virt", "-m", str(RAM // (1024 * 1024)),
               "-bios", "none", "-kernel", args.elf,
               "-display", "none", "-serial", "none", "-monitor", "none"]
        t0 = time.perf_counter()
        try:
            subprocess.run(cmd, timeout=120, capture_output=True)
            ok = True
        except subprocess.TimeoutExpired:
            ok = False
        dt = time.perf_counter() - t0
        times.append(dt)
        if not ok:
            print("  (timeout: the image did not reach its exit)")
    best = min(times)
    print("[qemu] %s sec=%.6f (best of %d)" % (args.label or args.elf, best, len(times)))


if __name__ == "__main__":
    main()
