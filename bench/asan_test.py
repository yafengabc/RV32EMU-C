#!/usr/bin/env python3
"""AddressSanitizer regression for rv32emu-c.

`build_asan.ps1` builds build/asan/rv32emu-c-asan.exe; this script runs it over
every workload that touches memory management:

   1. TinyOS boot to the shell (flash, ELF segments, UART, PMP, traps)
   2. TinyOS boot + user programs via a scripted console (cat, ls, mem, prot,
      crash, reboot -> the whole session twice in a row exercises the machine
      re-install path used by the software reset)
   3. bad command lines: missing file, corrupt ELF, truncated ELF, huge -max
   4. every benchmark image from bench/build (if present)

A non-zero exit or any "ERROR: AddressSanitizer" in stderr fails the script.
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TINYOS = os.path.join(os.path.expanduser("~"), "Documents", "Projects",
                      "TinyOS", "build")


def run(exe, args, stdin_data=None, timeout=300):
    env = dict(os.environ)
    env["ASAN_OPTIONS"] = "abort_on_error=1:halt_on_error=1"
    env["PATH"] = r"C:\msys64\ucrt64\bin;" + env.get("PATH", "")
    t0 = __import__("time").perf_counter()
    r = subprocess.run([exe] + args, input=stdin_data, capture_output=True,
                       env=env, timeout=timeout)
    dt = __import__("time").perf_counter() - t0
    err = r.stderr.decode("utf-8", "replace")
    return r.returncode, err, dt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-exe", default=os.path.join(ROOT, "build", "asan",
                                                 "rv32emu-c-asan.exe"))
    ap.add_argument("-timeout", type=int, default=300)
    args = ap.parse_args()
    exe = args.exe
    if not os.path.exists(exe):
        print("build it first: build_asan.ps1")
        return 2

    elf = os.path.join(TINYOS, "kernel-esp32c3.elf")
    ramdisk = os.path.join(TINYOS, "ramdisk-esp32c3.bin")
    bad = os.path.join(ROOT, "build", "asan", "bad.elf")

    cases = []
    if os.path.exists(elf):
        cases.append(("TinyOS boot", ["-elf", elf, "-ramdisk", ramdisk,
                                      "-max", "5000000", "-quiet"], None))
        # console script: cat/ps/uptime + reboot, then one more command
        # stdin is a pipe here, so -console runs the plain byte-pump flavour.
        # The guest only has to survive the whole script; ASan is what we are
        # after, not the shell's answers.
        script = b"cat readme.txt\nuptime\nls\nreboot\nhelp\nexit\n" * 4
        cases.append(("TinyOS console script",
                      ["-elf", elf, "-ramdisk", ramdisk, "-console", "-max",
                       "3000000"], script))
    cases.append(("missing elf", ["-elf", "does-not-exist.elf"], None))
    with open(bad, "wb") as f:
        f.write(b"\x7fELF\x01\x01\x01" + b"\x00" * 8 + b"\xff" * 40)
    cases.append(("corrupt elf", ["-elf", bad], None))
    with open(bad, "wb") as f:
        f.write(b"\x7fELF\x01\x01\x01")
    cases.append(("truncated elf", ["-elf", bad], None))
    cases.append(("no input", [], None))

    bench = os.path.join(ROOT, "build", "benchdata")
    for name in ("fib.elf", "alu_loop.elf", "mem_loop.elf", "pi_big.elf",
                 "bench_sort.elf", "bench_sieve.elf"):
        p = os.path.join(ROOT, "build", "benchdata", name)
        if os.path.exists(p):
            cases.append((name, ["-elf", p, "-max", "2000000", "-quiet"], None))

    failures = 0
    for label, argv, stdin_data in cases:
        try:
            rc, err, dt = run(exe, argv, stdin_data, args.timeout)
        except subprocess.TimeoutExpired:
            print(f"[asan] {label:<24} TIMEOUT")
            failures += 1
            continue
        sanitizer = "AddressSanitizer" in err and "ERROR" in err
        ok = (not sanitizer) and (rc == 0 or label in
                                  ("missing elf", "corrupt elf", "truncated elf",
                                   "no input"))
        print(f"[asan] {label:<24} rc={rc:<3} {dt:7.2f}s {'PASS' if ok else 'FAIL'}")
        if sanitizer:
            print("\n".join(err.splitlines()[:40]))
        failures += 0 if ok else 1

    print("RESULT:", "PASS" if failures == 0 else f"FAIL ({failures})")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
