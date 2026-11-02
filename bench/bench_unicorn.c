/*
 * bench_unicorn.c - run benchmark ELFs on Unicorn (QEMU TCG 2.1.4) so the
 * numbers can be compared with rv32emu-go and rv32emu-c.
 *
 * Build (MSYS2 UCRT64, after `pacman -S mingw-w64-ucrt-x86_64-unicorn`):
 *   gcc -O2 -o bench_unicorn.exe bench_unicorn.c -lunicorn
 *
 * Usage:
 *   bench_unicorn.exe -elf FILE [-reps R] [-max N]
 *
 * How it works
 *   Unicorn's RISC-V port stops at the end of every basic block whose tail is
 *   a jump or branch that does not fit the last instruction of the TB (about
 *   15 bytes or more); resuming at that PC would re-execute the previous
 *   instruction, so a chunked loop stalls. For -max N this is solved by the
 *   workload itself: the generator puts the ECALL that ends the program N
 *   words *after* the entry point and fills the gap with "addi x5, x5, 1".
 *   With such an image, `uc_emu_start(entry, 0, 0, N)` executes exactly N
 *   instructions (the last one is the ECALL) and stops, which is the same
 *   semantics as Go/C `-max N`.
 *
 *   Without -max, the image must contain the real end of the program (an
 *   ECALL: Unicorn raises UC_ERR_INSN_INVALID for EBREAK); the harness
 *   counts it with UC_HOOK_CODE and then times the same path.
 *
 * Counter
 *   The harness keeps its own instruction counter, derived from the text
 *   layout, instead of UC_HOOK_CODE for the timed run: hooks add a C callback
 *   per instruction and would dominate the JIT's execution time.
 *
 * MMIO: the peripheral window is mapped and reads return 0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unicorn/unicorn.h>
#include <unicorn/riscv.h>

#ifdef _WIN32
#include <windows.h>
static double now_sec(void) {
    static LARGE_INTEGER freq;
    static int init = 0;
    if (!init) {
        QueryPerformanceFrequency(&freq);
        init = 1;
    }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}
#else
#include <time.h>
static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
#endif

#define MMIO_BASE  0x60000000u
#define MMIO_SIZE  0x00100000u
#define SRAM_BASE  0x3FC80000u
#define SRAM_SIZE  0x00080000u
#define TEXT_BASE  0x40000000u
#define TEXT_SIZE  0x00100000u

static uint64_t g_inst;
static int g_diag;

typedef struct {
    uint32_t base, off, filesz, memsz;
} seg_t;

static seg_t g_segs[64];
static int g_nseg;
static uint8_t *g_data;

static uint32_t rd16(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static int is_ecall(uint32_t w) { return w == 0x00000073u; }

static uint8_t *read_file(const char *path, uint32_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)n ? (size_t)n : 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) {
        free(buf);
        return NULL;
    }
    *len = (uint32_t)n;
    return buf;
}

static void code_hook(uc_engine *uc, uint64_t address, uint32_t size,
                      void *ud) {
    (void)uc;
    (void)size;
    (void)ud;
    if (g_diag && g_inst < 40)
        fprintf(stderr, "[dbg] %llu 0x%llx\n", (unsigned long long)g_inst,
                (unsigned long long)address);
    g_inst++;
}

static void intr_hook(uc_engine *uc, uint32_t intno, void *ud) {
    (void)intno;
    (void)ud;
    uc_emu_stop(uc);
}

static void mmio_hook(uc_engine *uc, uc_mem_type type, uint64_t address,
                      int size, int64_t value, void *ud) {
    (void)type;
    (void)value;
    (void)ud;
    uint32_t v = 0;
    if (size <= 0 || size > 4 || (size & (size - 1)) != 0) size = 4;
    uc_mem_write(uc, address, &v, (size_t)size);
}

static void map_range(uc_engine *uc, uint32_t addr, uint32_t size) {
    uint64_t base = addr & ~0xFFFFull;
    uint64_t end = ((uint64_t)addr + (size ? size : 1) + 0xFFFF) & ~0xFFFFull;
    if (end <= base) end = base + 0x10000ull;
    uc_mem_map(uc, base, end - base, UC_PROT_ALL);
}

static uc_engine *make_engine(int counted) {
    uc_engine *uc;
    if (uc_open(UC_ARCH_RISCV, UC_MODE_RISCV32, &uc) != UC_ERR_OK) return NULL;
    map_range(uc, TEXT_BASE, TEXT_SIZE);
    for (int i = 0; i < g_nseg; i++) {
        if (g_segs[i].base < TEXT_BASE ||
            g_segs[i].base >= TEXT_BASE + TEXT_SIZE)
            map_range(uc, g_segs[i].base, g_segs[i].memsz);
        uc_mem_write(uc, g_segs[i].base, g_data + g_segs[i].off,
                     g_segs[i].filesz);
    }
    map_range(uc, SRAM_BASE, SRAM_SIZE);
    map_range(uc, MMIO_BASE, MMIO_SIZE);
    uint32_t sp = SRAM_BASE + SRAM_SIZE - 0x1000;
    uc_reg_write(uc, UC_RISCV_REG_SP, &sp);
    uc_hook h;
    uc_hook_add(uc, &h, UC_HOOK_MEM_READ, (void *)mmio_hook, NULL,
                MMIO_BASE, MMIO_BASE + MMIO_SIZE - 1);
    uc_hook_add(uc, &h, UC_HOOK_INTR, (void *)intr_hook, NULL, 1, 0);
    if (counted) uc_hook_add(uc, &h, UC_HOOK_CODE, (void *)code_hook, NULL, 1, 0);
    return uc;
}

int main(int argc, char **argv) {
    const char *elf = NULL;
    int reps = 7;
    uint64_t max_inst = 0;
    g_diag = getenv("UC_DIAG") != NULL;
    uint32_t start_override = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-elf") && i + 1 < argc) elf = argv[++i];
        else if (!strcmp(argv[i], "-reps") && i + 1 < argc)
            reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-max") && i + 1 < argc)
            max_inst = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-start") && i + 1 < argc)
            start_override = (uint32_t)strtoul(argv[++i], NULL, 0);
    }
    if (!elf) {
        fprintf(stderr, "usage: %s -elf FILE [-reps R] [-max N]\n", argv[0]);
        return 2;
    }

    uint32_t len = 0;
    g_data = read_file(elf, &len);
    if (!g_data || len < 52 || memcmp(g_data, "\x7f" "ELF", 4) != 0 ||
        g_data[4] != 1) {
        fprintf(stderr, "bad elf: %s\n", elf);
        return 1;
    }
    uint32_t entry = rd32(g_data + 24);
    if (start_override) entry = start_override;
    uint32_t phoff = rd32(g_data + 28);
    uint32_t phentsize = rd16(g_data + 42);
    uint32_t phnum = rd16(g_data + 44);
    g_nseg = 0;
    for (uint32_t i = 0; i < phnum && g_nseg < 64; i++) {
        const uint8_t *p = g_data + phoff + i * phentsize;
        if (rd32(p) != 1) continue;
        seg_t *s = &g_segs[g_nseg++];
        s->off = rd32(p + 4);
        s->base = rd32(p + 12);
        s->filesz = rd32(p + 16);
        s->memsz = rd32(p + 20);
    }

    if (reps < 1) reps = 1;
    if (reps > 64) reps = 64;
    double times[64];

    /* Instruction count: -max, or the ECALL present in the image. */
    uint64_t inst = max_inst;
    if (inst == 0) {
        for (int i = 0; i < g_nseg && inst == 0; i++) {
            seg_t *s = &g_segs[i];
            for (uint32_t b = 0; b + 4 <= s->filesz; b += 4) {
                if (!is_ecall(rd32(g_data + s->off + b))) continue;
                if (s->base + b < entry) continue;
                inst = (s->base + b - entry) / 4 + 1;
                break;
            }
        }
        if (inst == 0) {
            fprintf(stderr, "[unicorn] no ECALL in image and no -max given\n");
            return 1;
        }
    }

    /* warm-up + timed runs */
    {
        uc_engine *uc = make_engine(0);
        if (!uc) return 1;
        uc_emu_start(uc, entry, 0, 0, inst);
        uc_close(uc);
    }
    uint32_t a0 = 0;
    for (int r = 0; r < reps; r++) {
        uc_engine *uc = make_engine(0);
        if (!uc) return 1;
        double t0 = now_sec();
        uc_err err = uc_emu_start(uc, entry, 0, 0, inst);
        double t1 = now_sec();
        times[r] = t1 - t0;
        uc_reg_read(uc, UC_RISCV_REG_X10, &a0);
        uc_close(uc);
        if (g_diag) fprintf(stderr, "[dbg] run %d err=%d a0=%u\n", r, err, a0);
    }
    /* sanity: a counted run must retire the same number of instructions
     * before stopping at the same point */
    {
        uc_engine *uc = make_engine(1);
        if (!uc) return 1;
        g_inst = 0;
        uc_emu_start(uc, entry, 0, 0, inst);
        uc_close(uc);
    }
    double best = times[0];
    for (int i = 1; i < reps; i++)
        if (times[i] < best) best = times[i];
    printf("[bench] unicorn inst=%llu counted=%llu a0=%u\n",
           (unsigned long long)inst, (unsigned long long)g_inst, a0);
    printf("[bench] unicorn median sec=%.6f mips=%.2f\n", best,
           (double)inst / best / 1e6);
    free(g_data);
    return 0;
}
