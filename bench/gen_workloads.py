#!/usr/bin/env python3
"""Generate the -max parity images and the real-program image.

Parity images (for comparing Go/C/Unicorn at the same `-max N`):

    <program body at 0x40000000>
    <N-K instructions of filler>            (K = body length)
    ecall

The body runs once (or in a loop until it would reach the filler); the
instruction budget then runs out inside the filler. Go and C execute the
same text with `-max N`; Unicorn executes it with
`bench_unicorn -max N` (uc_emu_start count = N).

The real-program images (no -max) end with ECALL and are used to time a
complete firmware-like run.
"""
import os
import struct
import sys

BASE = 0x40000000


def i(rd, rs1, imm, f3=0, op=0x13):
    return ((imm & 0xFFF) << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op


def r(rd, rs1, rs2, f3, f7=0):
    return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | 0x33


def b(rs1, rs2, imm, f3):
    u = imm & 0x1FFF
    return (((u >> 12) & 1) << 31 | ((u >> 5) & 0x3F) << 25 |
            (rs2 << 20) | (rs1 << 15) | (f3 << 12) |
            ((u >> 1) & 0xF) << 8 | ((u >> 11) & 1) << 7 | 0x63)


def lui(rd, imm20):
    return (imm20 << 12) | (rd << 7) | 0x37


FILLER = i(5, 5, 1)   # addi x5, x5, 1 (compact, always legal, no faults)


def write(name, out, words):
    text = b"".join(struct.pack("<I", w & 0xFFFFFFFF) for w in words)
    ehdr = struct.pack(
        "<16sHHIIIIIHHHHHH",
        b"\x7fELF\x01\x01\x01" + b"\x00" * 9,
        2, 243, 1, BASE, 52, 0, 0, 52, 32, 1, 0, 0, 0)
    phdr = struct.pack("<IIIIIIII", 1, 84, BASE, BASE, len(text), len(text), 7, 4)
    with open(os.path.join(out, name), "wb") as f:
        f.write(ehdr + phdr + text)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    N = int(sys.argv[2]) if len(sys.argv) > 2 else 2_000_000
    os.makedirs(out, exist_ok=True)

    # 4-instruction ALU loop: the branch jumps back to word 0.
    alu = [i(5, 5, 1), i(6, 0, 1), b(5, 6, -8, 4), 0x00000013]
    write("alu_n.elf", out, alu + [FILLER] * (N - 4) + [0x00000073])

    # 3-instruction memory loop over one page, unsigned +: lw, add, sw, bne.
    mem = [
        lui(8, 0x40000),          # x8 = 0x40000000 (data + code page)
        i(9, 0, 1),               # step = 1
        i(10, 0, 0),              # sum = 0
        i(14, 8, 0, 2, 0x03),     # loop: lw x14, 0(x8)
        r(10, 10, 14, 0),         # sum += x14
        i(0, 8, 0, 2, 0x23),      # sw x0, 0(x8)   (memory aligned, stays 0)
        i(8, 8, 4),               # ptr += 4
        i(9, 9, 1),               # n++ (wrap-safe not needed: budget is small)
        b(9, 0, -20, 1),          # bne n, x0 -> loop (always taken)
    ]
    write("mem_n.elf", out, mem + [FILLER] * (N - len(mem)) + [0x00000073])

    # M-extension loop: mul, divu and a branch.
    pi = [
        lui(5, 0x20000),          # x5 = 0x20000000
        i(6, 0, 1),               # x6 = 1
        i(7, 0, 0),               # x7 = 0
        r(9, 8, 6, 0, 0x01),      # loop: mul x9, x8, x6
        r(10, 9, 6, 5, 0x01),     # divu x10, x9, x6
        r(7, 7, 10, 0),           # add x7, x7, x10
        i(6, 6, 1),               # x6++
        b(6, 0, -16, 1),          # bne x6, x0 -> loop
    ]
    write("pi_n.elf", out, pi + [FILLER] * (N - len(pi)) + [0x00000073])

    # Real program, for the ECALL-terminated run.
    write("alu_run.elf", out, alu + [0x00000073])
    write("mem_run.elf", out, mem + [0x00000073])
    write("pi_run.elf", out, pi + [0x00000073])
    print("generated -max parity images and run images in", out)


if __name__ == "__main__":
    main()
