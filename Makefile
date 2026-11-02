CC      ?= gcc
CFLAGS  ?= -std=c11 -O3 -DNDEBUG -flto -Wall -Wextra
LDFLAGS ?= -flto

SRC := $(wildcard src/*.c)
HDR := $(wildcard src/*.h)
BIN := build/rv32emu-c

ifeq ($(OS),Windows_NT)
BIN := build/rv32emu-c.exe
endif

.PHONY: all clean debug

all: $(BIN)

$(BIN): $(SRC) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

debug: CFLAGS := -std=c11 -O0 -g3 -Wall -Wextra
debug: LDFLAGS :=
debug: $(SRC) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) -o build/rv32emu-c-debug$(if $(filter Windows_NT,$(OS)),.exe,) $(SRC)

clean:
	rm -rf build
