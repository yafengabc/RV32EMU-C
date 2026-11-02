GO      ?= go
CC      ?= gcc
CFLAGS  ?= -std=c11 -O0 -g3 -Wall -Wextra

# The Go builder is the single entry point (no PowerShell, no shell-specific
# recipes): it knows where MSYS2 lives on Windows and skips DLL/ASan there.
.PHONY: all emu dll asan tools gen bench debug clean

all:
	$(GO) run ./cmd/rv32emu-build

emu:
	$(GO) run ./cmd/rv32emu-build emu

dll:
	$(GO) run ./cmd/rv32emu-build dll

asan:
	$(GO) run ./cmd/rv32emu-build asan

tools:
	$(GO) run ./cmd/rv32emu-build tools

gen:
	$(GO) run ./cmd/rv32emu-build gen

bench:
	$(GO) run ./cmd/rv32emu-build bench

# Unoptimised binary with symbols for gdb / -trace sessions.
debug:
	@mkdir -p build
	$(CC) $(CFLAGS) -o build/rv32emu-c-debug$(if $(filter Windows_NT,$(OS)),.exe,) src/*.c

clean:
	rm -rf build
