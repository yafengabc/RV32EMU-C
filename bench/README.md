# Go vs C emulator benchmark (with a Unicorn probe)

Benchmarks the Go emulator, the C port and (where possible) Unicorn 2.1.4 on
the same RISC-V binaries.

```
gen_workloads.py    alu_loop / mem_loop / pi_big (Go vs C numbers)
compare.ps1         builds both, runs them, writes results.md
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
