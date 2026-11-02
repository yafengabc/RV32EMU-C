#!/usr/bin/env python3
"""Small ELF32 writer + RV32IM encoder shared by the QEMU tests.

make_elf(path, entry, segments) where each segment is (paddr, bytes).
"""
import struct

EM_RISCV = 243


def make_elf(path, entry, segments, phdr_off=52):
    nseg = len(segments)
    data_off = phdr_off + 32 * nseg
    blobs = []
    off = data_off
    for _pa, blob in segments:
        blobs.append((off, blob))
        off += len(blob)
    ehdr = struct.pack(
        "<16sHHIIIIIHHHHHH",
        b"\x7fELF\x01\x01\x01" + b"\x00" * 9,
        2, EM_RISCV, 1, entry, phdr_off, 0, 0, 52, 32, nseg, 0, 0, 0)
    ph = b""
    for (pa, blob), (o, _b) in zip(segments, blobs):
        ph += struct.pack("<IIIIIIII", 1, o, pa, pa, len(blob), len(blob), 7, 4)
    with open(path, "wb") as f:
        f.write(ehdr + ph)
        for _o, blob in blobs:
            f.write(blob)


# ---- RV32IM encoder ---------------------------------------------------
def i(rd, rs1, imm, f3=0, op=0x13):
    return ((imm & 0xFFF) << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op


def r(rd, rs1, rs2, f3, f7=0):
    return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | 0x33


def s(rs1, rs2, imm, f3=0):
    u = imm & 0xFFF
    return (((u >> 5) & 0x7F) << 25) | (rs2 << 20) | (rs1 << 15) | \
           (f3 << 12) | ((u & 0x1F) << 7) | 0x23


def b(rs1, rs2, imm, f3=0):
    u = imm & 0x1FFF
    return (((u >> 12) & 1) << 31 | ((u >> 5) & 0x3F) << 25 |
            (rs2 << 20) | (rs1 << 15) | (f3 << 12) |
            ((u >> 1) & 0xF) << 8 | ((u >> 11) & 1) << 7 | 0x63)


def u(rd, imm20, op=0x37):
    return (imm20 << 12) | (rd << 7) | op


def j(rd, imm):
    x = imm & 0x1FFFFF
    return (((x >> 20) & 1) << 31 | ((x >> 1) & 0x3FF) << 21 |
            ((x >> 11) & 1) << 20 | ((x >> 12) & 0xFF) << 12 | (rd << 7) | 0x6F)


def words(*ws):
    return b"".join(struct.pack("<I", w & 0xFFFFFFFF) for w in ws)
