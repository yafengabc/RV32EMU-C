#!/usr/bin/env python3
"""Build the quick-compare images (freestanding RV32IMC).

Same algorithms as firmware/bench (the TinyGo example), plain RV32IMC so every
emulator can load them with -elf:

    quick_fib.elf     fib(28)
    quick_sort.elf    bubble sort x 20000
    quick_sieve.elf   sieve(50000) x 50

Each image prints "D" then a0 (as hex) then "DONE" on a byte FIFO at
0x60000000 (UART0 of the C3), or 0x10000000 (QEMU virt) when the byte
0x00000013 is at the image base -- see bin_patch for the QEMU flavour.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from elfutil import i, r, s, b, u, j, words, make_elf  # noqa: E402

BASE = 0x40000000


def build(out, name, body, text_base=BASE):
    """entry 0x00: li a2, UART ; li sp ; j body ; body ; tail."""
    pre = words(u(12, 0x60000))           # 0x00: a2 = 0x60000000
    pre += words(u(2, 0x3FCFF))           # 0x04: sp = 0x3FCFF000
    pre += words(j(0, 0x20 - 0x08))       # 0x08: j 0x20
    for _ in range(5):
        pre += words(0x00000013)          # 0x0c..0x1c nops (8-byte aligned)
    body_off = 0x20
    make_elf(os.path.join(out, name), text_base, [(text_base, pre + body)])


# ---- print helper: a0 -> hex + "DONE", in 0x100..0x1ff --------------------
def printer():
    # a1 = nibble string, a3 = temp, a4 = count
    ws = []
    ws.append(u(5, 0x40020))              # a1 = 0x40020000 (table in a second page)
    return None


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out, exist_ok=True)

    # ---------------- fib(28) -------------------------------------------
    fib_body = [i(2, 2, -16)]
    body = words(
        i(10, 0, 28),                     # 0x20: a0 = 28
        j(1, 0x2c - 0x24),                # 0x24: jal fib @0x2c
        0x00100073,                       # 0x28: ebreak
        # fib @ 0x2c
        i(2, 2, -16),                     # 0x2c: sp -= 16
        s(2, 1, 12),                      # 0x30: sw ra, 12(sp)
        s(2, 9, 8),                       # 0x34: sw s0, 8(sp)
        i(9, 10, 0),                      # 0x38: s0 = a0
        i(5, 0, 2),                       # 0x3c: t0 = 2
        b(10, 5, 0x54 - 0x40, 4),         # 0x40: blt a0, 2, base @0x54
        i(10, 10, -1),                    # 0x44: a0-1
        j(1, 0x2c - 0x48),                # 0x48: jal fib
        s(2, 10, 4),                      # 0x4c: sw a0, 4(sp)
        i(10, 9, -2),                     # 0x50: a0 = s0-2
        i(6, 10, 0),                      # 0x54: t1 = a0  (placeholder, fixed below)
    )
    # (re)build fib cleanly with fixed addresses
    fib = [
        # 0x2c
        i(2, 2, -16),
        s(2, 1, 12),
        s(2, 9, 8),
        i(9, 10, 0),
        i(5, 0, 2),
        b(10, 5, 0x68 - 0x40, 4),         # 0x40 -> 0x68 (base)
        i(10, 10, -1),
        j(1, 0x2c - 0x48),                # 0x48 -> 0x2c
        s(2, 10, 4),                      # 0x4c
        i(10, 9, -2),                     # 0x50
        j(1, 0x2c - 0x54),                # 0x54 -> 0x2c
        i(6, 10, 0),                      # 0x58: t1 = a0
        i(5, 2, 4, 2, 0x03),              # 0x5c: t0 = lw 4(sp)
        r(10, 5, 6, 0),                   # 0x60: a0 = t0 + t1
        j(0, 0x68 - 0x64),                # 0x64: j base
        i(10, 9, 0),                      # 0x68: base: a0 = s0
        i(1, 2, 12, 2, 0x03),             # 0x6c: ra = lw 12(sp)
        i(9, 2, 8, 2, 0x03),              # 0x70: s0 = lw 8(sp)
        i(2, 2, 16),                      # 0x74: sp += 16
        i(0, 1, 0, 0, 0x67),              # 0x78: jalr x0, 0(ra)
    ]
    code = words(i(10, 0, 28), j(1, 0x2c - 0x24), 0x00100073) + words(*fib)
    build(out, "quick_fib.elf", code)
    print("wrote quick_fib.elf")


if __name__ == "__main__":
    main()
