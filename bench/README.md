# Go vs C emulator benchmark (with a Unicorn probe)

Benchmarks the Go emulator, the C port and (where possible) Unicorn 2.1.4 on
the same RISC-V binaries.

```
gen_workloads.py    alu_loop / mem_loop / pi_big (Go vs C numbers)
compare.ps1         builds both, runs them, writes results.md
qemu_check.py       runs fib/pi/sort/sieve on QEMU, verifies and times them
tinyos_qemu.py      boots TinyOS from a flash image on esp32c3 and times bench
go-repo.txt         where the Go emulator (RV32EMU) checkout lives
benchdata/          Go sources compiled for the run:
  benchrun/           timing harness (same loop as the Go emulator's main.go)
  genbench/, genfib/  hand-assembled benchmark ELFs
bench_unicorn.c     Unicorn C harness (-max N = uc_emu_start count)
bench_unicorn.py    Unicorn straight-line speed probe
```

Run:

```powershell
cd rv32emu-c
.\bench\compare.ps1 -Reps 7 -IncludeTinyOS
```

The Go side needs a checkout of <https://github.com/yafengabc/RV32EMU>. The
script looks for it (in this order) at `-GoRepo <path>`, the path in
`bench/go-repo.txt`, `$env:RV32EMU_GO_REPO`, and finally the two common layouts
(`RV32EMU/rv32emu-c` or a sibling `RV32EMU/`). Default is `../RV32EMU` relative
to `bench/`, i.e. a C clone placed inside the Go repository.

`benchrun` lives in `benchdata/benchrun/main.go` and is a verbatim copy of the
Go repository's `tools/benchrun/main.go`. On each run the script:

1. creates a scratch directory with a `go.mod` that says `module rv32emu`,
2. copies the checkout's `internal/{cpu,elf,mem,perip,rom}` packages into it,
3. copies the harness to `tools/benchrun/main.go`, and
4. runs `go build -buildvcs=false ./tools/benchrun` there.

The Go checkout is never modified, so it can stay read-only, and the harness
always matches the revision in the checkout. Build both sides from the same
commit when comparing, otherwise the numbers mix versions.

Keep the harness byte-identical to `tools/benchrun/main.go`: restructuring the
hot loop (for example wrapping it in `measure()`/`measureBody()` helpers) makes
the Go side about 1.7x slower and inflates every ratio in the table.

## QEMU: two machines

`qemu_check.py` verifies the same four programs on QEMU. Two machines are
supported and the results must match both the Go/C emulators and a Python
model of each algorithm:

* **`esp32c3`** - Espressif's QEMU (`github.com/espressif/qemu`). It models the
  real C3 map (code at 0x40000000, DRAM 0x3FC80000..0x3FCDFFFF, UART0 at
  0x60000000), so the images keep the same base as the rv32emu ones; only the
  exit sequence differs (print a0 on UART0, then spin, and the harness stops
  QEMU once it has the line). Windows builds are published in the
  `espressif/qemu` releases as `qemu-riscv32-softmmu-*-x86_64-w64-mingw32.tar.xz`;
  point `ESP_QEMU` at `qemu/bin/qemu-system-riscv32.exe`.
* **`virt`** - upstream qemu-system-riscv32. DRAM starts at 0x80000000, so the
  generators are re-invoked with `-base/-stack/-data`; the exit is a 16550 hex
  print followed by a write to the SiFive test device at 0x100000.

```powershell
python bench\qemu_check.py          # picks esp32c3 when available
python bench\qemu_check.py -machine virt
```

Timing: each image is run `-reps` times and the best wall time is kept, minus the
wall time of an almost-empty image (`fib(0)`), which removes QEMU startup and the
harness' own overhead. Instruction counts come from running the same program in
halt mode on `build/rv32emu-c.exe`, so the reported MIPS is a rate, not an
estimate. `-work 15` (default) scales the repeat counts and uses fib(34) so every
run lasts 0.1-0.3 s; a single 90k-instruction pass is below process-timer noise.
QEMU is TCG, so these numbers sit one order of magnitude above the interpreters.
The startup baseline differs a lot between builds (Espressif 9.2.2: ~0.023 s,
upstream 11.1.3: ~0.091 s), which is exactly why it has to be subtracted: without
it the upstream build looks 3x slower even though both run pi at ~6.6 GIPS. Only
the recursive fib differs (upstream ~780 vs Espressif ~1115 MIPS).

The C3 machine implements 384 KiB of DRAM (up to 0x3FCDFFFF), so the stack is
placed at 0x3FCDF000. rv32emu models a 512 KiB window and also accepts
0x3FCFF000; real hardware and QEMU do not.

TinyOS on the same machine: `tinyos_qemu.py` builds a real flash image (kernel
at 0, littlefs at 0x100000) and boots it with `-drive file=...if=mtd`; `-kernel`
crashes in the ROM area. It reaches `tos>` in ~0.22 s and works, but the C3
memory/SPI-flash path is pathologically slow: `bench 1024 4` takes ~200 ms and
the default 256 KiB `bench` ~9.1 s, versus 8 ms / 25 ms on rv32emu. That is a
QEMU machine-model problem, so QEMU is used for functional checks only.

## Unicorn 2.1.4: not usable for these workloads

The RISC-V port (MSYS2 `mingw-w64-ucrt-x86_64-unicorn`, QEMU TCG) fails on
the images used here. All of this was reproduced with small harnesses that
print every executed address:

* **taken branches in long TBs**: when a TB is 15+ bytes and the branch is
  taken, Unicorn executes the *branch target* in place of the branch and then
  faults at the end of the TB (`UC_ERR_EXCEPTION`, PC at the instruction after
  the target). Seen with `j +12`, `beq +8` and the first branch of every loop.
  A 4-instruction loop therefore stops after the first iteration; of all the
  images, only `fib` and the tight ALU loop manage a long counted run (2M
  instructions at 90 and 279 MIPS respectively, both stopped early), the rest
  end after a handful of instructions;
* **ECALL** reaches `UC_HOOK_INTR`, **EBREAK** is decoded as an invalid
  instruction (`UC_ERR_INSN_INVALID`) - the port has no SYSTEM support;
* **data page aliasing**: a load/store that misses its 4 KiB page silently
  touches the neighbouring page, so the sort/sieve images with data at
  `0x3FC80000` read garbage 12 KiB further up;
* **counted runs** (`uc_emu_start(..., count=N)`) stop at the TB boundary that
  contains the N-th instruction; the last TB is cut so its tail branch never
  executes, so a counted run of a loop image falls out of the loop;
* with `count=0` a run to the end of the last mapped page keeps fetching the
  next page's zeros and never returns to the caller.

What does work is straight-line code inside one page. `bench_unicorn.py`
times 16,000 `nop`s in a row (through the Python bindings):

```
[bench] unicorn straight-line n=16000 sec=0.01778680 mips=1
[bench] unicorn straight-line (hook) n=16000 sec=0.02148020 mips=0.74
```

The first line is the raw JIT (the Python call overhead of 0.018 s dominates
these 16k instructions); with a `UC_HOOK_CODE` callback, i.e. the same
per-instruction accounting the Go/C harnesses do, it drops below 1 MIPS.
Either way Unicorn cannot run a firmware with branches in 2.1.4, so no
apples-to-apples firmware number can be produced with this version.

For context, the Go README's Unicorn numbers (13.8x on a tight ALU loop,
2x on real algorithms) were measured with the Python bindings and a
`UC_HOOK_INTR`-based exit; on a real firmware with memory traffic and
branches the JIT advantage shrinks to about 2x.

## Reproducing the Unicorn harness

```powershell
gcc -O2 -o build/bench_unicorn.exe bench/bench_unicorn.c ^
    -I C:/msys64/ucrt64/include -L C:/msys64/ucrt64/lib -lunicorn
build\bench_unicorn.exe -elf build\benchdata\fib.elf -reps 5 -max 2000000
```

Straight-line probe:

```powershell
python bench/bench_unicorn.py -n 16000 -reps 7
```
