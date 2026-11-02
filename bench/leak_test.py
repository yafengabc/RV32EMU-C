#!/usr/bin/env python3
"""Leak regression for rv32emu-c: repeated full sessions in one process.

    python bench/leak_test.py [-emu build/rv32emu-c.exe] [-runs 10]

Each run releases the VM and reloads it; the working set, committed bytes and
handle counts are printed and an assertion fails the script when a run leaks
more than a small tolerance.  Pass -join to route _shutdown through
EnterCriticalSection (MSVC CRT / FindCollision detects double frees).
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TINYOS = os.path.join(os.path.expanduser("~"),
                      "Documents", "Projects", "TinyOS", "build")


def parse(line):
    m = re.search(r"ws=\s*(\d+) KiB private=\s*(\d+) KiB committed=\s*(\d+) KiB "
                  r"handles=(\d+) gdi=(\d+) user=(\d+)", line)
    return tuple(int(x) for x in m.groups()) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-emu", default=os.path.join(ROOT, "build", "rv32emu.dll"))
    ap.add_argument("-leakrun", default=os.path.join(ROOT, "build", "leakrun.exe"))
    ap.add_argument("-runs", type=int, default=10)
    ap.add_argument("-max", type=int, default=2_000_000)
    ap.add_argument("-join", action="store_true")
    args = ap.parse_args()

    emu = args.emu if args.emu.endswith(".dll") else args.emu[:-4] + ".dll"
    elf = os.path.join(TINYOS, "kernel-esp32c3.elf")
    rd = os.path.join(TINYOS, "ramdisk-esp32c3.bin")
    if not os.path.exists(elf):
        print("TinyOS firmware not found:", elf)
        return 1
    cmd = [args.leakrun, emu, elf, "-ramdisk", rd, "-max", str(args.max),
           "-runs", str(args.runs)]
    if args.join:
        cmd.append("-join")
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    print(out)
    rows = [parse(l) for l in out.splitlines() if "[leak]" in l and "ws=" in l]
    rows = [r for r in rows if r]
    if len(rows) < 2:
        print("no stats produced")
        return 1
    # rows: before, after run i..., after shutdown, after FreeLibrary
    before, after_runs = rows[0], rows[1:-2]
    ws_growth = [r[0] - before[0] for r in after_runs]
    handle_growth = [r[3] - before[3] for r in after_runs]
    worst_ws = max(ws_growth) if ws_growth else 0
    worst_handle = max(handle_growth) if handle_growth else 0
    print(f"runs={len(after_runs)} ws growth (KiB) {ws_growth}")
    print(f"handle growth {handle_growth}")
    ok = worst_handle <= 0 and worst_ws < 2048  # one 2 MiB flash + bus window
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
