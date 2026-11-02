# Go vs C emulator benchmark (with a Unicorn probe)

Benchmarks the Go emulator, the C port and (where possible) Unicorn 2.1.4 on
the same RISC-V binaries.

```
benchdata/          Go module for everything below:
  cmd/bench/          the driver: check / tinyos / compare / mem
  benchrun/           Go timing harness (byte-identical to RV32EMU tools/benchrun)
  genbench/, genfib/  hand-assembled benchmark ELFs
  genwork/            alu_loop / mem_loop / pi_big + -max parity images
go-repo.txt         where the Go emulator (RV32EMU) checkout lives
bench_unicorn.c     Unicorn C harness (-max N = uc_emu_start count)
```

Build and run (all commands are one Go tool; there are no Python or PowerShell
scripts in this directory any more):

```powershell
cd rv32emu-c
go run ./cmd/rv32emu-build          # emulator, DLL, ASan, leak drivers, generators
build\rv32emu-bench.exe compare -reps 7 -tinyos
build\rv32emu-bench.exe check       # QEMU correctness + MIPS
build\rv32emu-bench.exe tinyos --tb-stats
build\rv32emu-bench.exe mem
```

The Go side needs a checkout of <https://github.com/yafengabc/RV32EMU>. The
driver looks for it (in this order) at `-go-repo <path>`, the path in
`bench/go-repo.txt`, `$env:RV32EMU_GO_REPO`, and finally the two common layouts
(`RV32EMU/rv32emu-c` or a sibling `RV32EMU/`). Default is `../RV32EMU` relative
to `bench/`, i.e. a C clone placed inside the Go repository.

`benchrun` lives in `benchdata/benchrun/main.go` and is a verbatim copy of the
Go repository's `tools/benchrun/main.go`. On each run `compare`:

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

`rv32emu-bench check` verifies the same four programs on QEMU. Two machines are
supported and the results must match both the Go/C emulators and an independent
Go model of each algorithm:

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
build\rv32emu-bench.exe check                 # picks esp32c3 when available
build\rv32emu-bench.exe check -machine virt
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

Scripted console sessions work without `-console`: `hostin.c` polls a pipe or
redirected file at every input-feed boundary and pushes it into the UART, the
same way the Go build's reader goroutine does.

The C3 machine implements 384 KiB of DRAM (up to 0x3FCDFFFF), so the stack is
placed at 0x3FCDF000. rv32emu models a 512 KiB window and also accepts
0x3FCFF000; real hardware and QEMU do not.

TinyOS on the same machine: `tinyos` builds a real flash image (kernel
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

What does work is straight-line code inside one page. A one-off Python probe
timed 16,000 `nop`s in a row (through the Python bindings; the probe was
removed with the other scripts):

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
