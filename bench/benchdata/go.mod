// Benchmark sources for compare.ps1.
//
// genbench/genfib are ordinary programs and build here directly:
//   go build ./genbench ./genfib
//
// benchrun is NOT built from this module: it imports rv32emu/internal/... and
// is meant to be copied into a scratch module named "rv32emu" together with a
// checkout's internal packages (compare.ps1 does that). Treat it as a
// template - keep it byte-identical to the Go repository's tools/benchrun.
module benchdata

go 1.22
