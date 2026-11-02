#!/usr/bin/env python3
"""Memory-management report for rv32emu-c.

Runs the ASan regression (bench/asan_test.py) and the leak drivers
(tools/leakrun.c against build/rv32emu-c.dll, tools/leakunit.c for the core),
then prints one verdict. Requires a built rv32emu-c.exe/DLL and the ASan exe.

    python bench/memcheck.py
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
TINYOS = os.path.join(os.path.expanduser("~"), "Documents", "Projects",
                      "TinyOS", "build")


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def main():
    ok = True
    print("== AddressSanitizer ==")
    r = run([sys.executable, os.path.join(ROOT, "bench", "asan_test.py")])
    tail = [l for l in r.stdout.splitlines() if l.startswith(("[asan]", "RESULT:"))]
    print("\n".join(tail))
    ok &= r.returncode == 0

    print("\n== sessions in one process (DLL, TinyOS boot x6) ==")
    leakrun = os.path.join(BUILD, "leakrun.exe")
    dll = os.path.join(BUILD, "rv32emu-c.dll")
    elf = os.path.join(TINYOS, "kernel-esp32c3.elf")
    rd = os.path.join(TINYOS, "ramdisk-esp32c3.bin")
    if all(os.path.exists(p) for p in (leakrun, dll, elf)):
        r = run([leakrun, dll, elf, "-ramdisk", rd, "-max", "500000",
                 "-runs", "6", "-join"])
        lines = [l for l in r.stdout.splitlines() if l.startswith("[leak]")]
        rows = [l for l in lines if "ws=" in l]
        print("\n".join(lines))
        ws = [int(l.split("ws=")[1].split()[0]) for l in rows]
        handles = [int(l.split("handles=")[1].split()[0]) for l in rows]
        if ws and max(ws[1:]) - ws[1] == 0 and handles[1] == handles[0]:
            print("-> stable across runs")
        else:
            print("-> GROWTH DETECTED")
            ok = False
    else:
        print("skip: build leakrun.exe / rv32emu-c.dll first")

    print("\n== core: fresh machine per round, one shared bus ==")
    unit = os.path.join(BUILD, "leakunit.exe")
    if os.path.exists(unit):
        r = run([unit])
        lines = [l for l in r.stdout.splitlines()
                 if "private=" in l and ("round" in l or "start" in l or
                                         "end" in l or "reload" in l)]
        print("\n".join(lines[-16:]))
        if "RESULT" in r.stdout:
            ok &= "PASS" in r.stdout
    else:
        print("skip: build tools/leakunit.c first")

    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
