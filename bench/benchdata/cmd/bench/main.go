// rv32emu-bench - the consolidated benchmark/test driver for rv32emu-c (Go
// port of the old bench/*.py and compare.ps1 scripts).
//
//   rv32emu-bench check    QEMU correctness + timing on esp32c3/virt
//   rv32emu-bench tinyos   boot TinyOS on the esp32c3 QEMU (TB retranslation)
//   rv32emu-bench compare  Go vs C emulator throughput table
//   rv32emu-bench mem      ASan + leak regression
package main

import (
	"fmt"
	"os"
)

const usage = `rv32emu-bench - rv32emu-c benchmark/test driver

usage:
  rv32emu-bench check   [-machine auto|esp32c3|virt] [-reps N] [-work N]
                        [-qemu PATH] [-esp-qemu PATH] [--no-build] [--no-time]
                        run the benchmark programs on QEMU, verify against the
                        reference values and time them (MIPS = inst/(wall-fib0))
  rv32emu-bench tinyos  [-kernel-bin FILE] [-ramdisk FILE] [-esp-qemu PATH]
                        [-bench "1024 4"] [--no-bench] [--tb-stats]
                        boot TinyOS on the esp32c3 QEMU from a real flash image
  rv32emu-bench compare [-reps 7] [-max 20000000] [-tinyos] [-unicorn]
                        [-go-repo PATH] [-skip-build]
                        Go vs C emulator throughput table (writes results.md)
  rv32emu-bench mem     [-asan-exe PATH] [-runs N] [-max N]
                        ASan + leak regression (build/asan, leakrun, leakunit)
`

func main() {
	if len(os.Args) < 2 {
		fmt.Fprint(os.Stderr, usage)
		os.Exit(2)
	}
	rc := 2
	switch os.Args[1] {
	case "check":
		rc = cmdCheck(os.Args[2:])
	case "tinyos":
		rc = cmdTinyOS(os.Args[2:])
	case "compare":
		rc = cmdCompare(os.Args[2:])
	case "mem":
		rc = cmdMem(os.Args[2:])
	case "help", "-h", "--help":
		fmt.Print(usage)
		rc = 0
	default:
		fmt.Fprintf(os.Stderr, "unknown command %q\n\n", os.Args[1])
		fmt.Fprint(os.Stderr, usage)
	}
	os.Exit(rc)
}
