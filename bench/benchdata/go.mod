// Benchmark sources and the test driver for rv32emu-c.
//
// genbench/genfib/genwork are ordinary programs and build here directly:
//   go build ./genbench ./genfib ./genwork
//
// cmd/bench is the driver behind `go run ./cmd/rv32emu-build bench`; it is a
// separate module so the emulator repository itself stays C-only.
//
// benchrun is NOT built from this module: it imports rv32emu/internal/... and
// is meant to be copied into a scratch module named "rv32emu" together with a
// checkout's internal packages (cmd/bench compare does that). Treat it as a
// template - keep it byte-identical to the Go repository's tools/benchrun.
module benchdata

go 1.22
