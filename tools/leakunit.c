#include "../src/rv32.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

bool g_quiet = true;

static size_t private_kib(void) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc;
    ZeroMemory(&pmc, sizeof(pmc));
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc,
                         sizeof(pmc));
    return pmc.PrivateUsage / 1024;
#else
    return 0;
#endif
}

static void stats(const char *when) {
    printf("%-12s private=%7zu KiB\n", when, private_kib());
}

/*
 * Leak check for the core (no peripherals):
 *
 * 1. boot a fresh TinyOS machine 12 times and release it each time: the
 *    7.5 MiB bus window, the CPU, the timer and the flash image must all go
 *    away, so private bytes must return to the baseline;
 * 2. translate the same ELF 12 times on ONE bus: bus_add_ram currently appends
 *    a new RAM region for every load instead of merging into the existing one,
 *    so private bytes grow once (that is the "repeated load on one bus" case,
 *    not a per-session leak - the CLI/GUI always build a new bus).
 */
int main(void) {
    const char *elf =
        "C:\\Users\\SONG\\Documents\\Projects\\TinyOS\\build\\kernel-esp32c3.elf";
    stats("start");
    for (int r = 0; r < 12; r++) {
        bus_t *bus = bus_new();
        cpu_t *c = cpu_new(bus);
        uint32_t entry = elf_load(elf, bus);
        cpu_set_pc(c, entry);
        for (int i = 0; i < 1000; i++) cpu_run_hot(c, 1000);
        char tag[24];
        snprintf(tag, sizeof(tag), "round %d", r + 1);
        stats(tag);
        cpu_free(c);
        bus_free(bus);
    }
    stats("end");

    bus_t *shared = bus_new();
    stats("shared bus");
    for (int r = 0; r < 12; r++) {
        uint32_t entry = elf_load(elf, shared);
        char tag[24];
        snprintf(tag, sizeof(tag), "reload %d", r + 1);
        (void)entry;
        stats(tag);
    }
    bus_free(shared);
    stats("after free");
    return 0;
}
