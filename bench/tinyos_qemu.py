#!/usr/bin/env python3
"""Boot TinyOS on Espressif's esp32c3 QEMU and time a `bench` run.

Not a performance benchmark: it documents that QEMU can run TinyOS (from a real
flash image) but that its C3 memory/SPI-flash path is very slow, so QEMU is only
usable for functional checks. The numbers printed here are meant to be compared
with rv32emu on the same firmware.

The flash layout matches what scripts/esp-c3/flash.py writes on real hardware:

    0x000000  esp32c3.bin        (TinyOS kernel)
    0x100000  ramdisk-esp32c3.bin (littlefs)

`-kernel` cannot be used: QEMU would load the ELF into the ROM window and the
kernel crashes at 0x40000134.

    python bench/tinyos_qemu.py -kernel-bin .../esp32c3.bin -ramdisk .../ramdisk-esp32c3.bin
"""

import argparse
import collections
import os
import queue
import re
import shutil
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD = os.path.join(ROOT, "build")

DEFAULT_ESP_QEMU = r"C:\Users\SONG\Tools\esp-qemu\qemu\bin\qemu-system-riscv32.exe"
DEFAULT_TINYOS = r"C:\Users\SONG\Documents\Projects\TinyOS\build"

FLASH_SIZE = 4 * 1024 * 1024
FS_OFFSET = 0x100000


def find_binary(explicit, candidates):
    for cand in ([explicit] if explicit else []) + candidates:
        if not cand:
            continue
        if os.path.isabs(cand):
            if os.path.exists(cand):
                return cand
        else:
            found = shutil.which(cand)
            if found:
                return found
    return None


def make_flash(kernel_path, ramdisk_path, out_path):
    kernel = open(kernel_path, "rb").read()
    ramdisk = open(ramdisk_path, "rb").read()
    img = bytearray(FLASH_SIZE)
    if len(kernel) > FS_OFFSET:
        raise SystemExit("kernel does not fit below the filesystem")
    img[0:len(kernel)] = kernel
    if FS_OFFSET + len(ramdisk) > FLASH_SIZE:
        raise SystemExit("ramdisk does not fit in the 4 MiB flash")
    img[FS_OFFSET:FS_OFFSET + len(ramdisk)] = ramdisk
    with open(out_path, "wb") as f:
        f.write(img)
    return len(kernel), len(ramdisk)


class Reader:
    """Drain the guest's stdout on a thread so the main loop can time things."""

    def __init__(self, proc):
        self.q = queue.Queue()
        threading.Thread(target=self._pump, args=(proc,), daemon=True).start()

    def _pump(self, proc):
        while True:
            try:
                data = os.read(proc.stdout.fileno(), 65536)
            except OSError:
                data = b""
            if not data:
                self.q.put(b"")
                return
            self.q.put(data)

    def wait_for(self, needle, timeout, sink=None):
        deadline = time.perf_counter() + timeout
        buf = b""
        while time.perf_counter() < deadline:
            try:
                data = self.q.get(timeout=0.05)
            except queue.Empty:
                data = b""
            if data:
                buf += data
                if sink is not None:
                    sink.extend(data)
            if needle in buf:
                return time.perf_counter()
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-kernel-bin", default=os.path.join(DEFAULT_TINYOS, "esp32c3.bin"))
    ap.add_argument("-ramdisk", default=os.path.join(DEFAULT_TINYOS, "ramdisk-esp32c3.bin"))
    ap.add_argument("-esp-qemu", default=os.environ.get("ESP_QEMU", DEFAULT_ESP_QEMU))
    ap.add_argument("-boot-timeout", type=float, default=20.0)
    ap.add_argument("-bench", default="1024 4", help="arguments for the guest bench command")
    ap.add_argument("--no-bench", action="store_true")
    ap.add_argument("--tb-stats", action="store_true",
                    help="also log -d in_asm and report the most retranslated TBs")
    args = ap.parse_args()

    for label, path in (("kernel", args.kernel_bin), ("ramdisk", args.ramdisk)):
        if not os.path.exists(path):
            print("missing %s: %s" % (label, path), file=sys.stderr)
            return 2

    qemu = find_binary(args.esp_qemu, [DEFAULT_ESP_QEMU, "qemu-system-riscv32"])
    if not qemu:
        print("qemu-system-riscv32 not found (install Espressif's QEMU)", file=sys.stderr)
        return 2

    os.makedirs(BUILD, exist_ok=True)
    flash = os.path.join(BUILD, "tinyos-flash.bin")
    klen, rlen = make_flash(args.kernel_bin, args.ramdisk, flash)
    print("flash: %d MiB (kernel %d B @0, ramdisk %d B @0x%X)"
          % (FLASH_SIZE // (1024 * 1024), klen, rlen, FS_OFFSET))

    tb_log = None
    cmd = [qemu, "-M", "esp32c3", "-m", "8M",
           "-drive", "file=%s,if=mtd,format=raw" % flash,
           "-display", "none", "-serial", "stdio", "-monitor", "none"]
    if args.tb_stats:
        tb_log = os.path.join(BUILD, "tinyos-tb.log")
        try:
            os.remove(tb_log)
        except OSError:
            pass
        cmd += ["-d", "in_asm", "-D", tb_log]
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, bufsize=0)
    reader = Reader(proc)
    try:
        sink = bytearray()
        t0 = time.perf_counter()
        hit = reader.wait_for(b"tos> ", args.boot_timeout, sink)
        if hit is None:
            print("no prompt in %.0fs; last output:" % args.boot_timeout)
            print(sink[-600:].decode("ascii", "replace"))
            return 1
        print("boot to prompt: %.2f s" % (hit - t0))

        if args.no_bench:
            return 0

        before = os.path.getsize(tb_log) if tb_log else 0
        proc.stdin.write(("bench " + args.bench).strip().encode() + b"\r\n")
        proc.stdin.flush()
        t1 = time.perf_counter()
        sink = bytearray()
        hit = reader.wait_for(b"done", 300.0, sink)
        if hit is None:
            print("bench '%s' did not finish" % args.bench)
            return 1
        print("bench %-10r: %.1f ms" % (args.bench, (hit - t1) * 1000))

        if tb_log:
            with open(tb_log, "r", encoding="utf-8", errors="replace") as f:
                f.seek(before)
                tail = f.read()
            starts = re.findall(r"^(0x[0-9a-f]{8}):\s", tail, re.M)
            top = collections.Counter(starts).most_common(6)
            print("TB translations during bench: %d distinct, %d total"
                  % (len(set(starts)), len(starts)))
            for addr, n in top:
                print("  %s retranslated x%d" % (addr, n))
    finally:
        proc.kill()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
