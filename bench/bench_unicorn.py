#!/usr/bin/env python3
"""Unicorn (2.1.4) speed probe for the rv32emu comparison.

The RISC-V port of Unicorn 2.1.4 cannot run the rv32emu workload images:
every TB that ends in a taken branch executes the branch target in place of
the branch when the branch is 15+ bytes into the TB, EBREAK is not decoded,
and a counted `uc_emu_start` stops at the last branch.  What it does run is
straight-line code inside one page.  This probe times a plain
`addi x1, x1, 1` stream (16 KiB page, up to `-n` <= 16384 instructions) so
the JIT's best case can be set next to the interpreters' ~0.5 GMIPS:

    python bench_unicorn.py [-reps 7] [-n 16000]

The reported MIPS is n / wall clock with no hooks: an upper bound for
Unicorn, not a firmware number.
"""
import argparse
import struct
import time

from unicorn import *
from unicorn.riscv_const import *

BASE = 0x40000000
PAGE = 0x10000


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-reps", type=int, default=7)
    ap.add_argument("-n", type=int, default=16000)
    args = ap.parse_args()
    n = min(args.n, PAGE // 4)
    code = struct.pack("<I", 0x00108093) * n  # addi x1, x1, 1
    times = []
    for _ in range(args.reps):
        uc = Uc(UC_ARCH_RISCV, UC_MODE_RISCV32)
        uc.mem_map(BASE, PAGE)
        uc.mem_write(BASE, code)
        t0 = time.perf_counter()
        try:
            uc.emu_start(BASE, 0, 0, n)
        except UcError as e:
            print("unicorn stopped:", e)
            return
        times.append(time.perf_counter() - t0)
        del uc
    best = min(times)
    print("[bench] unicorn straight-line n=%d sec=%.8f mips=%.0f"
          % (n, best, n / best / 1e6))

    # Same run with a per-instruction hook: the honest comparison with the
    # Go/C harnesses, which count instructions as they execute.
    counts = [0]

    def on_code(uc, address, size, user):
        counts[0] += 1

    times = []
    for _ in range(args.reps):
        uc = Uc(UC_ARCH_RISCV, UC_MODE_RISCV32)
        uc.mem_map(BASE, PAGE)
        uc.mem_write(BASE, code)
        uc.hook_add(UC_HOOK_CODE, on_code)
        counts[0] = 0
        t0 = time.perf_counter()
        try:
            uc.emu_start(BASE, 0, 0, n)
        except UcError as e:
            print("unicorn (hooked) stopped:", e)
            break
        times.append(time.perf_counter() - t0)
        del uc
    if times:
        best = min(times)
        print("[bench] unicorn straight-line (hook) n=%d sec=%.8f mips=%.2f"
              % (n, best, n / best / 1e6))


if __name__ == "__main__":
    main()
