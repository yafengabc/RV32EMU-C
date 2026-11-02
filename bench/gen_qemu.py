#!/usr/bin/env python3
"""Generate QEMU virt (qemu-system-riscv32) benchmark images.

The same three workloads as the Go/C comparison, laid out for QEMU's virt
machine (RV32IM, -bios none -kernel):

    0x80000000  reset: j start
    0x80000080  start: li a0, ITERS ; li a1, DATA ; li a2, UART0
    0x80000100  loop body
    0x80001000  DATA (4 KiB)
    0x800FC000  stack top

After the loop the program prints a0 on the 16550 UART (THR = 0x10000000)
and halts in `j .`.

    python gen_qemu.py OUTDIR [--iters N]
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from elfutil import i, r, s, b, u, j, words, make_elf  # noqa: E402

BASE = 0x80000000
UART = 0x10000000
DATA = 0x80001000
ITERS = 1_000_000


def program(body):
    """reset jump, start prologue (a0=ITERS, a1=DATA, a2=UART, sp), body."""
    img = bytearray(0x1100)
    img[0x000:0x004] = words(j(0, 0x80))
    img[0x080:0x0A0] = words(
        u(10, (ITERS >> 12) & 0xFFFFF), i(10, 10, ITERS & 0xFFF),
        u(11, DATA >> 12),
        u(12, UART >> 12),
        u(2, 0x800FC),
        i(2, 2, 0))
    img[0x100:0x100 + len(body)] = body
    return bytes(img)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    # 6 instructions per iteration
    alu = words(
        i(5, 5, 1),
        r(6, 5, 10, 4),
        i(7, 6, 1, 1),
        i(7, 7, 1, 5),
        i(10, 10, -1),
        b(10, 0, -20, 1))
    make_elf(os.path.join(args.out, "alu_qemu.elf"), BASE, [(BASE, program(alu))])

    # 6 instructions per iteration, 256 words per pass
    mem = words(
        i(13, 11, 0),
        i(14, 11, 0x400),
        i(10, 0, 256),
        i(15, 13, 0, 2, 0x03),
        r(9, 9, 15, 0),
        s(13, 9, 0, 2),
        i(13, 13, 4),
        i(10, 10, -1),
        b(10, 0, -20, 1),
        b(13, 14, -36, 1))
    make_elf(os.path.join(args.out, "mem_qemu.elf"), BASE, [(BASE, program(mem))])

    # 7 instructions per iteration, M extension
    pi = words(
        i(6, 0, 0),
        i(7, 0, 3),
        r(8, 6, 6, 0, 0x01),
        r(9, 8, 7, 5, 0x01),
        r(10, 10, 9, 0),
        i(6, 6, 1),
        b(10, 0, 0, 0),   # placeholder, replaced below
        )
    # replace the last instruction: loop back while a0 != 0 (a0 is the counter)
    pi = words(
        i(6, 0, 0),
        i(7, 0, 3),
        r(8, 6, 6, 0, 0x01),
        r(9, 8, 7, 5, 0x01),
        r(10, 10, 9, 0),
        i(6, 6, 1),
        b(10, 0, -16, 4))  # blt a0, x0, loop?? -- fixed below
    # a0 is the countdown: use it as the loop condition directly
    pi = words(
        r(8, 6, 6, 0, 0x01),   # 0x100: x8 = x6 * x6
        r(9, 8, 7, 5, 0x01),   # 0x104: x9 = x8 / 3
        r(10, 10, 9, 0),       # 0x108: a0 += x9  (a0 is the countdown: fine)
        i(6, 6, 1),            # 0x10c: x6++
        r(10, 10, 10, 0),      # 0x110: a0 += a0 (doubles the countdown)
        b(10, 0, 0, 0))        # placeholder
    # simpler and correct: counter in a0, decrement it, do the M ops on x6
    pi = words(
        r(8, 6, 6, 0, 0x01),   # 0x100: x8 = x6 * x6
        r(9, 8, 7, 5, 0x01),   # 0x104: x9 = x8 / 3
        i(6, 6, 1),            # 0x108: x6++
        r(5, 5, 9, 0),         # 0x10c: x5 += x9
        i(10, 10, -1),         # 0x110: a0--
        b(10, 0, -16, 1))      # 0x114: bne a0, x0, 0x100
    make_elf(os.path.join(args.out, "pi_qemu.elf"), BASE, [(BASE, program(pi))])
    print("generated alu_qemu.elf mem_qemu.elf pi_qemu.elf in", args.out)


if __name__ == "__main__":
    main()
