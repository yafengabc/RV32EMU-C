#define BUILDING_EMU 1
#include "console.h"
#include "embed.h"
#include "hostin.h"
#include "rv32.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

bool g_quiet = false;

/* Host wall clock in nanoseconds.  The guest's TIMG is driven by this (see
 * opt_clk_inst), because a real chip's timer runs off a crystal, not off the
 * number of instructions executed. */
static uint64_t host_ns(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static int init = 0;
    if (!init) {
        QueryPerformanceFrequency(&freq);
        init = 1;
    }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)((t.QuadPart * 1000000000ull) / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

/* ---- command line --------------------------------------------------- */

static const char *opt_elf = NULL;
static const char *opt_bin = NULL;
static const char *opt_flashimg = NULL;
static const char *opt_ramdisk = NULL;
static uint32_t opt_ramdisk_off = 0x80000u; /* ramdisk load offset in flash */
static uint32_t opt_entry = RESET_VECTOR;
static uint64_t opt_max = 0;
static uint64_t opt_step = 0;
static int opt_tickdiv = 1;
/* 1 = one timer count per executed instruction (the default, matching the Go
 * build: runs are exactly reproducible and piped stdin lines up with -rxdelay),
 * 0 = drive the guest timer from the host wall clock (-clk wall). */
static int opt_clk_inst = 1;
static int opt_rxrate = 16;
static uint64_t opt_rxperiod = 10000;
static uint64_t opt_rxdelay = 0;
static uint64_t opt_breakpc = 0;
static const char *opt_dumpram = NULL;
static bool opt_nopmp = false;
static bool opt_traplog = false;
static bool opt_trace = false;
static bool opt_regs = false;
static int opt_bench_reps = 0;
static uint64_t opt_bench_max = 0;
static bool opt_bench_mode = false;
static bool opt_console = false; /* -console: interactive UART terminal */
static bool g_console_up = false;

void usage(const char *argv0) {
    printf("usage: %s [options]\n", argv0);
    printf("  -elf FILE        load RV32 ELF\n");
    printf("  -bin FILE        load raw binary\n");
    printf("  -flashimg FILE   raw SPI flash image: boot the app found by\n");
    printf("                   its partition table (MicroPython / ESP-IDF)\n");
    printf("  -entry ADDR      load/entry base for -bin (default 0x40000000)\n");
    printf("  -ramdisk FILE    littlefs image placed at flash+0x%X (override: -ramdiskoff ADDR)\n",
           opt_ramdisk_off);
    printf("  -max N           stop after N instructions (0 = until halt)\n");
    printf("  -step N          execute N instructions then dump registers\n");
    printf("  -tickdiv N       stretch the TIMG0 period by N\n");
    printf("  -clk wall        drive the timer from the host clock (default: instruction count)\n");
    printf("  -nopmp           disable PMP enforcement in U-mode\n");
    printf("  -traplog         log synchronous traps\n");
    printf("  -trace           print every instruction\n");
    printf("  -breakpc ADDR    stop when pc reaches ADDR\n");
    printf("  -dumpram FILE    dump kernel RAM 0x40380000..0x403AC000\n");
    printf("  -regs            print final registers\n");
    printf("  -bench N         timer-only run: execute at most N instructions\n");
    printf("  -reps R          repeat the -bench run R times (default 1)\n");
    printf("  -console         interactive terminal on UART0\n");
    printf("                   (Ctrl-C = SIGINT to the guest, Ctrl-] = quit)\n");
}

static uint64_t parse_u64(const char *s) {
    return (uint64_t)strtoull(s, NULL, 0);
}

/* One option at a time, shared by the CLI loop and the embedded config
 * parser. `value` is NULL for flags. Returns 0 on success, -1 on error. */
static int set_option(const char *a, const char *v) {
    if (!v) {
        if (!strcmp(a, "nopmp")) { opt_nopmp = true; return 0; }
        if (!strcmp(a, "traplog")) { opt_traplog = true; return 0; }
        if (!strcmp(a, "trace")) { opt_trace = true; return 0; }
        if (!strcmp(a, "regs")) { opt_regs = true; return 0; }
        if (!strcmp(a, "quiet")) { g_quiet = true; return 0; }
        if (!strcmp(a, "console")) { opt_console = true; return 0; }
        return -1;
    }
    if (!strcmp(a, "elf")) opt_elf = v;
    else if (!strcmp(a, "bin")) opt_bin = v;
    else if (!strcmp(a, "flashimg")) opt_flashimg = v;
    else if (!strcmp(a, "ramdisk")) opt_ramdisk = v;
    else if (!strcmp(a, "ramdiskoff")) opt_ramdisk_off = (uint32_t)parse_u64(v);
    else if (!strcmp(a, "entry")) opt_entry = (uint32_t)parse_u64(v);
    else if (!strcmp(a, "max")) opt_max = parse_u64(v);
    else if (!strcmp(a, "step")) opt_step = parse_u64(v);
    else if (!strcmp(a, "tickdiv")) opt_tickdiv = (int)parse_u64(v);
    else if (!strcmp(a, "clk")) opt_clk_inst = strcmp(v, "wall") != 0;
    else if (!strcmp(a, "rxrate")) opt_rxrate = (int)parse_u64(v);
    else if (!strcmp(a, "rxperiod")) opt_rxperiod = parse_u64(v);
    else if (!strcmp(a, "rxdelay")) opt_rxdelay = parse_u64(v);
    else if (!strcmp(a, "breakpc")) opt_breakpc = parse_u64(v);
    else if (!strcmp(a, "dumpram")) opt_dumpram = v;
    else if (!strcmp(a, "bench")) {
        opt_bench_mode = true;
        opt_bench_max = parse_u64(v);
    } else if (!strcmp(a, "reps")) opt_bench_reps = (int)parse_u64(v);
    else if (!strcmp(a, "console")) { /* tolerate "console 1" */
        opt_console = true;
        return 0;
    } else return -1;
    return 0;
}

/* Clear every option: an embedded _run() must not inherit the previous
 * session's -elf/-console/... settings. */
static void reset_options(void) {
    opt_elf = NULL;
    opt_bin = NULL;
    opt_flashimg = NULL;
    opt_ramdisk = NULL;
    opt_ramdisk_off = 0x80000u;
    opt_entry = RESET_VECTOR;
    opt_max = 0;
    opt_step = 0;
    opt_tickdiv = 1;
    opt_clk_inst = 1;
    opt_rxrate = 16;
    opt_rxperiod = 10000;
    opt_rxdelay = 0;
    opt_breakpc = 0;
    opt_dumpram = NULL;
    opt_nopmp = false;
    opt_traplog = false;
    opt_trace = false;
    opt_regs = false;
    opt_bench_reps = 0;
    opt_bench_max = 0;
    opt_bench_mode = false;
    opt_console = false;
    g_quiet = false;
}

/* Embedded configuration language, one directive per line:
 *
 *     config {
 *         elf      C:\path\to\firmware.elf
 *         ramdisk  C:\path\to\ramdisk.bin
 *         console
 *         max      5000000
 *         quiet
 *     }
 *
 * Values may be quoted ("..." or '...'); # starts a comment. */
static char *g_cfg_strings[32];
static int g_cfg_nstrings;
static char g_last_error[512];

static const char *cfg_intern(const char *s, size_t n) {
    if (g_cfg_nstrings >= 32) return NULL;
    char *d = (char *)malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, s, n);
    d[n] = 0;
    g_cfg_strings[g_cfg_nstrings++] = d;
    return d;
}

static void cfg_free_strings(void) {
    for (int i = 0; i < g_cfg_nstrings; i++) free(g_cfg_strings[i]);
    g_cfg_nstrings = 0;
}

static int parse_config(const char *cfg) {
    reset_options();
    cfg_free_strings();
    if (!cfg) return -1;
    const char *p = cfg;
    while (*p) {
        /* skip blanks and comments */
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p == '#') {
            while (*p && *p != '\n') p++;
            continue;
        }
        if (!*p) break;
        /* braces are separators only */
        if (*p == '{') { p++; continue; }
        if (*p == '}') { p++; continue; }
        char key[64];
        size_t kl = 0;
        while (*p && !strchr(" \t\r\n{}", *p) && kl + 1 < sizeof(key)) key[kl++] = *p++;
        key[kl] = 0;
        if (!key[0]) continue;
        while (*p == ' ' || *p == '\t') p++;
        if (!strcmp(key, "config")) continue; /* block header */
        if (*p == '{' || *p == '}' || *p == 0 || *p == '\n' || *p == '#') {
            if (set_option(key, NULL) != 0) {
                snprintf(g_last_error, sizeof(g_last_error),
                         "config: unknown option '%s'", key);
                return -1;
            }
            continue;
        }
        char val[1024];
        size_t vl = 0;
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            while (*p && *p != q && vl + 1 < sizeof(val)) val[vl++] = *p++;
            if (*p == q) p++;
        } else {
            while (*p && *p != '\n' && vl + 1 < sizeof(val)) val[vl++] = *p++;
            while (vl > 0 && (val[vl - 1] == ' ' || val[vl - 1] == '\t' ||
                              val[vl - 1] == '\r' || val[vl - 1] == '}'))
                vl--;
        }
        val[vl] = 0;
        const char *stored = cfg_intern(val, vl);
        if (!stored) return -1;
        if (set_option(key, stored) != 0) {
            snprintf(g_last_error, sizeof(g_last_error),
                     "config: unknown option '%s'", key);
            return -1;
        }
    }
    return 0;
}

static int parse_args(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(argv[0]);
            return 1;
        }
        const char *name = a[0] == '-' ? a + 1 : a;
        const char *v = NULL;
        /* flags take no value */
        if (!strcmp(name, "nopmp") || !strcmp(name, "traplog") ||
            !strcmp(name, "trace") || !strcmp(name, "regs") ||
            !strcmp(name, "quiet") || !strcmp(name, "console")) {
            if (set_option(name, NULL) != 0) {
                fprintf(stderr, "unknown option: %s\n", a);
                return -1;
            }
            continue;
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "missing value for %s\n", a);
            return -1;
        }
        v = argv[++i];
        if (set_option(name, v) != 0) {
            fprintf(stderr, "unknown option: %s\n", a);
            return -1;
        }
    }
    return 0;
}

/* ---- machine -------------------------------------------------------- */

typedef struct {
    bus_t *bus;
    cpu_t *cpu;
    uart_t *uart;
    rtc_t *rtc;
    c3timer_t *timer;
    gpio_t *gpio;
    usj_t *usj;
    c3intc_t *intc;
    c3systimer_t *systimer;
    c3efuse_t *efuse;
    c3adc_t *adc;
    c3extras_t *extras;
    c3spi_t *spi0;
    c3spi_t *spi1;
    flashmmu_t *mmu;
    c3cache_t *cache;
    c3xip_t *drom;
    c3xip_t *irom;
    rom_service_t *rom;
} machine_t;

static void trap_logger(uint32_t cause, uint32_t epc, uint32_t tval) {
    if (cause & CAUSE_INTERRUPT_BIT) return;
    if (cause == CAUSE_ECALL_U || cause == CAUSE_ECALL_M) return;
    printf("[trap] cause=%u mepc=0x%08X mtval=0x%08X\n", cause, epc, tval);
}

static uint8_t *load_file(const char *path, uint32_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)n ? (size_t)n : 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(buf); return NULL; }
    *len = (uint32_t)n;
    return buf;
}

static void machine_free(machine_t *m);

static machine_t *machine_new(const uint8_t *flash, uart_t *uart) {
    machine_t *m = (machine_t *)calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->bus = bus_new();
    m->gpio = gpio_new();
    m->usj = usj_new();
    m->rtc = rtc_new();
    m->intc = c3intc_new();
    m->systimer = c3systimer_new();
    m->efuse = c3efuse_new();
    m->adc = c3adc_new();
    m->uart = uart;

    perip_attach_uart(m->bus, uart);
    perip_attach_gpio(m->bus, m->gpio);
    perip_attach_iomux(m->bus, m->gpio);
    perip_attach_usj(m->bus, m->usj);
    perip_attach_rtc(m->bus, m->rtc);
    perip_attach_intc(m->bus, m->intc);
    uart_set_intc(uart, m->intc);

    bus_add_ram(m->bus, FLASH_EMU_BASE, FLASH_SIZE);
    bus_copy_into(m->bus, FLASH_EMU_BASE, 0, flash, FLASH_SIZE);

    uint8_t *rom = (uint8_t *)malloc(ROM_SIZE);
    rom_build(rom);
    bus_load_rom(m->bus, 0, rom, ROM_SIZE);
    free(rom);

    rom_service_load_rom_data(m->bus);

    m->timer = c3timer_new(NULL);
    m->spi1 = c3spi_new(m->bus, FLASH_EMU_BASE, "C3SPI1");
    m->spi0 = c3spi_new(m->bus, FLASH_EMU_BASE, "C3SPI0");
    perip_attach_timer(m->bus, m->timer);
    perip_attach_systimer(m->bus, m->systimer);
    perip_attach_efuse(m->bus, m->efuse);
    perip_attach_adc(m->bus, m->adc);
    perip_attach_spi(m->bus, m->spi1, 0x60002000u);
    perip_attach_spi(m->bus, m->spi0, 0x60003000u);
    m->extras = c3extras_new(m->bus, m->intc);

    /* Flash cache / XIP must be attached before any ELF or image load: the
     * DROM/IROM windows hold the overlay the loader writes. */
    m->mmu = flashmmu_new(m->bus, FLASH_EMU_BASE, FLASH_SIZE);
    m->cache = c3cache_new(m->mmu);
    m->drom = c3xip_new(m->mmu, DROM_BASE, "DROM");
    m->irom = c3xip_new(m->mmu, IROM_BASE, "IROM");
    perip_attach_cache(m->bus, m->cache);
    perip_attach_xip(m->bus, m->drom);
    perip_attach_xip(m->bus, m->irom);

    m->cpu = cpu_new(m->bus);
    cpu_set_pmp_enforce(m->cpu, !opt_nopmp);
    if (opt_traplog) m->cpu->on_trap = trap_logger;
    c3timer_set_cpu(m->timer, m->cpu);
    c3timer_set_div(m->timer, (uint32_t)opt_tickdiv);
    c3timer_set_intc(m->timer, m->intc);
    c3systimer_set_intc(m->systimer, m->intc);
    c3intc_set_cpu(m->intc, m->cpu);

    if (opt_elf && opt_elf[0]) {
        uint32_t entry = elf_load(opt_elf, m->bus);
        if (!entry) {
            fprintf(stderr, "load elf failed\n");
            machine_free(m);
            return NULL;
        }
        cpu_set_pc(m->cpu, entry);
    } else if (opt_bin && opt_bin[0]) {
        if (elf_load_bin(opt_bin, opt_entry, m->bus) != 0) {
            fprintf(stderr, "load bin failed\n");
            machine_free(m);
            return NULL;
        }
        cpu_set_pc(m->cpu, opt_entry);
    } else if (opt_flashimg && opt_flashimg[0]) {
        uint32_t entry = 0;
        if (!image_load_app(flash, FLASH_SIZE, m->bus, m->mmu, &entry)) {
            fprintf(stderr, "load flash image failed\n");
            machine_free(m);
            return NULL;
        }
        cpu_set_pc(m->cpu, entry);
    } else {
        fprintf(stderr, "need -elf, -bin or -flashimg\n");
        machine_free(m);
        return NULL;
    }

    /* The ESP-IDF app entry assumes a bootloader already set up a stack.
     * 0x3FCDE000 sits at the top of DRAM, below nothing the app reserves. */
    m->cpu->x[2] = 0x3FCDE000u;

    m->rom = rom_service_new(m->bus, uart);
    if (m->rom) {
        m->cpu->rom_call = (void (*)(void *, cpu_t *, uint32_t))rom_service_call;
        m->cpu->rom_ctx = m->rom;
    }
    return m;
}

static void machine_free(machine_t *m) {
    if (!m) return;
    cpu_free(m->cpu);
    bus_free(m->bus);
    rom_service_free(m->rom);
    c3extras_free(m->extras);
    c3xip_free(m->drom);
    c3xip_free(m->irom);
    c3cache_free(m->cache);
    flashmmu_free(m->mmu);
    c3spi_free(m->spi0);
    c3spi_free(m->spi1);
    c3adc_free(m->adc);
    c3efuse_free(m->efuse);
    c3systimer_free(m->systimer);
    c3timer_free(m->timer);
    c3intc_free(m->intc);
    gpio_free(m->gpio);
    usj_free(m->usj);
    rtc_free(m->rtc);
    free(m);
}

static void machine_snapshot_flash(machine_t *m, uint8_t *dst) {
    bus_copy_out(m->bus, FLASH_EMU_BASE, dst, FLASH_SIZE);
}

static void write_ram_dump(machine_t *m) {
    if (!opt_dumpram || !opt_dumpram[0]) return;
    const uint32_t base = 0x40380000u, len = 0x2C000u;
    uint8_t *buf = (uint8_t *)malloc(len);
    bus_copy_out(m->bus, base, buf, len);
    FILE *f = fopen(opt_dumpram, "wb");
    if (!f) {
        fprintf(stderr, "dumpram: cannot write %s\n", opt_dumpram);
        free(buf);
        return;
    }
    fwrite(buf, 1, len, f);
    fclose(f);
    free(buf);
    printf("=== ram dump %u bytes @0x%08X -> %s ===\n", len, base, opt_dumpram);
}

static void print_disasm(cpu_t *c, uint32_t pc) {
    uint32_t low = bus_read(c->bus, pc, 2);
    char buf[128];
    if ((low & 0x3u) != 0x3u) {
        disasm_c((uint16_t)low, buf, sizeof(buf));
    } else {
        uint32_t hi = bus_read(c->bus, pc + 2, 2);
        disasm_i(pc, low | (hi << 16), buf, sizeof(buf));
    }
    printf("  %s\n", buf);
}

/* Returns true when the firmware requested a software reset. */
static bool machine_run(machine_t *m) {
    cpu_t *c = m->cpu;
    uint64_t limit = opt_max;
    if (opt_step > 0) limit = opt_step;
    bool bulk = !opt_trace && opt_step == 0 && opt_breakpc == 0 &&
                (!opt_console || g_console_up);
    uint64_t sim_ms = 0;

    if (g_quiet && !opt_console) {
        /* batch traces stay silent too, the header only matters for humans */
    } else if (!opt_console) {
        printf("=== RV32EMU start pc=0x%08X ===\n", c->pc);
    }
    uint64_t n = 0;
    /* Wall-clock mode (-clk wall): TIMG0 counts microseconds of host time.
     * The remainder is carried between batches so a 1000-instruction batch
     * that takes less than a microsecond still adds up instead of truncating
     * to zero. */
    uint64_t clk_prev = host_ns();
    uint64_t clk_rem_ns = 0;
    /* Carries sub-8 remainders when SYSTIMER is folded in single-step mode. */
    uint64_t sys_acc = 0;
    while (!c->halted && (limit == 0 || n < limit)) {
        if (g_console_up) {
            (void)console_input();
            if (console_quit_requested()) break;
        }
        if (opt_trace) {
            printf("[%6" PRIu64 "] pc=0x%08X", n, c->pc);
            print_disasm(c, c->pc);
        }
        if (opt_breakpc != 0 && (uint64_t)c->pc == opt_breakpc) {
            printf("\n=== break at pc=0x%08X (inst %" PRIu64 ") ===\n",
                   c->pc, n);
            char buf[2048];
            cpu_dump(c, buf, sizeof(buf));
            printf("%s", buf);
            write_ram_dump(m);
            return false;
        }

        uint64_t ran;
        if (bulk) {
            uint64_t b = 1000;
            if (limit != 0 && limit - n < b) b = limit - n;
            ran = cpu_run_hot(c, b);
        } else {
            cpu_step(c);
            ran = 1;
        }
        if (ran == 0) break;
        if (opt_clk_inst) {
            /* One timer count per instruction: the old behaviour, and the
             * one to use when a run has to be exactly reproducible. */
            c3timer_tick_n(m->timer, ran);
            /* Single-stepping would advance SYSTIMER once per instruction, so
             * the firmware's "read VAL_LO twice and wait for the same value"
             * loop would never settle (two reads are only four instructions
             * apart).  Fold eight instructions into one count, as the Go build
             * does; a batch already ticks once per batch. */
            if (bulk) {
                c3systimer_tick_n(m->systimer, ran);
            } else {
                sys_acc += ran;
                if (sys_acc >= 8) {
                    c3systimer_tick_n(m->systimer, sys_acc / 8);
                    sys_acc %= 8;
                }
            }
            rtc_tick(m->rtc, ran);
        } else {
            /* Real clock: a crystal does not care how many instructions
             * retired, so neither does the emulated TIMG/SYSTIMER. */
            uint64_t t = host_ns();
            uint64_t d = (t - clk_prev) + clk_rem_ns;
            clk_prev = t;
            clk_rem_ns = d % 1000u;
            c3timer_tick_n(m->timer, d / 1000u);
            c3systimer_tick_n(m->systimer, d / 1000u);
            rtc_tick(m->rtc, d / 1000u);
        }

        if (g_console_up) {
            /* The terminal owns the UART: bytes arrive through console_input()
             * and output is flushed about once per simulated millisecond. */
            uint64_t before = m->timer->fires;
            (void)before;
            console_tick(sim_ms);
        } else if (opt_rxrate > 0 && n >= opt_rxdelay) {
            uint64_t p = opt_rxperiod ? opt_rxperiod : 1;
            uint64_t k0 = (n + p - 1) / p;
            uint64_t k1 = (n + ran - 1) / p;
            if (k1 >= k0) {
                /* Without -console the guest still has to see piped stdin,
                 * like the Go build's reader goroutine. Poll on feed
                 * boundaries (one timer tick by default) so the syscall cost
                 * stays around one per 10k instructions instead of one per
                 * batch; a thread would be cheaper but the DLL sessions would
                 * inherit it. */
                uint8_t inbuf[256];
                size_t got = host_stdin_poll(inbuf, sizeof(inbuf));
                if (got > 0) (void)uart_rx_push(m->uart, inbuf, got);
            }
            for (uint64_t k = k0; k <= k1; k++) uart_feed(m->uart, opt_rxrate);
        }
        n += ran;
        sim_ms = m->timer->fires; /* one TIMG0 tick is one simulated ms */
        if (g_console_up) console_tick(sim_ms);
        if (!g_console_up) uart_flush(m->uart);

        if (rtc_reset_pending(m->rtc) ||
            rtc_wdt_reset_pending(m->rtc) ||
            c3timer_wdt_reset_pending(m->timer)) {
            bool wdt = rtc_wdt_reset_pending(m->rtc) ||
                       c3timer_wdt_reset_pending(m->timer);
            if (g_console_up) {
                console_write("\r\n[reset]\r\n", strlen("\r\n[reset]\r\n"));
            } else {
                printf("\n=== %s after %" PRIu64 " inst ===\n",
                       wdt ? "watchdog reset"
                           : "software reset (RTC_CNTL SW_SYS_RST)",
                       n);
            }
            uart_reset(m->uart);
            m->rtc->reset = false;
            return true;
        }
    }

    if (g_console_up) {
        console_tick(sim_ms);
        return false;
    }
    printf("\n=== stopped after %" PRIu64 " inst, reason=\"%s\" ===\n",
           n, c->halt_msg);
    printf("=== irq: timg0 fires=%" PRIu64 ", cpu interrupts=%" PRIu64
           ", exceptions=%" PRIu64 " ===\n",
           m->timer->fires, c->int_count, c->exc_count);
    write_ram_dump(m);
    if (opt_step > 0 || c->halted || opt_regs) {
        char buf[2048];
        cpu_dump(c, buf, sizeof(buf));
        printf("%s", buf);
    }
    return false;
}

static double now_sec(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static int init = 0;
    if (!init) {
        QueryPerformanceFrequency(&freq);
        init = 1;
    }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

static uint64_t bench_one(const uint8_t *flash, uart_t *uart, double *sec) {
    machine_t *m = machine_new(flash, uart);
    if (!m) {
        *sec = 0;
        return 0;
    }
    cpu_t *c = m->cpu;
    uint64_t n = 0;
    double t0 = now_sec();
    while (!c->halted && (opt_bench_max == 0 || n < opt_bench_max)) {
        uint64_t b = 1000;
        if (opt_bench_max != 0 && opt_bench_max - n < b)
            b = opt_bench_max - n;
        uint64_t ran = cpu_run_hot(c, b);
        if (ran == 0) break;
        c3timer_tick_n(m->timer, ran);
        n += ran;
        if (rtc_reset_pending(m->rtc)) break;
    }
    double t1 = now_sec();
    *sec = t1 - t0;

    uint64_t inst = c->inst_count;
    uint32_t a0 = cpu_reg(c, 10);
    if (!g_quiet)
        printf("[bench] inst=%" PRIu64 " final_pc=0x%08X a0=0x%08X (%u)\n",
               inst, c->pc, a0, (unsigned)a0);
    else
        printf("[bench] inst=%" PRIu64 " a0=%u\n", inst, (unsigned)a0);
    machine_free(m);
    return inst;
}

/* ---- CLI ------------------------------------------------------------ */

int main(int argc, char **argv) {
    int pr = parse_args(argc, argv);
    if (pr != 0) return pr > 0 ? 0 : 2;

    if (!opt_elf && !opt_bin && !opt_flashimg) {
        usage(argv[0]);
        return 2;
    }

    uint8_t *flash = (uint8_t *)malloc(FLASH_SIZE);
    if (!flash) return 1;
    /* Erased SPI flash reads 0xFF; the app and LittleFS both rely on that. */
    memset(flash, 0xFF, FLASH_SIZE);
    if (opt_flashimg && opt_flashimg[0]) {
        uint32_t img_len = 0;
        uint8_t *img = load_file(opt_flashimg, &img_len);
        if (!img) {
            fprintf(stderr, "load flash image failed: %s\n", opt_flashimg);
            free(flash);
            return 1;
        }
        if (img_len > FLASH_SIZE) img_len = FLASH_SIZE;
        memcpy(flash, img, img_len);
        if (!g_quiet) printf("[flash] image %u bytes\n", img_len);
        free(img);
    }
    if (opt_ramdisk && opt_ramdisk[0]) {
        uint32_t rd_len = 0;
        uint8_t *rd = load_file(opt_ramdisk, &rd_len);
        if (!rd) {
            fprintf(stderr, "load ramdisk failed: %s\n", opt_ramdisk);
            free(flash);
            return 1;
        }
        const uint32_t flash_off = opt_ramdisk_off;
        if (rd_len > FLASH_SIZE - flash_off) rd_len = FLASH_SIZE - flash_off;
        memcpy(flash + flash_off, rd, rd_len);
        if (!g_quiet)
            printf("[flash] ramdisk %u bytes @0x%08X\n", rd_len,
                   FLASH_EMU_BASE + flash_off);
        free(rd);
    }

    uart_t *uart = uart_new();
    if (!uart) {
        free(flash);
        return 1;
    }

    if (opt_console && !opt_bench_mode) {
        g_console_up = console_start();
        if (g_console_up) {
            console_set_uart(uart);
            uart_set_tx_hook(uart, console_write, NULL);
        } else {
            fprintf(stderr, "console: cannot start the terminal\n");
            uart_free(uart);
            free(flash);
            return 1;
        }
    }

    if (opt_bench_mode) {
        int reps = opt_bench_reps > 0 ? opt_bench_reps : 1;
        if (opt_bench_max == 0) opt_bench_max = opt_max;
        double *times = (double *)calloc((size_t)reps, sizeof(double));
        uint64_t inst = 0;
        for (int r = 0; r < reps; r++) {
            inst = bench_one(flash, uart, &times[r]);
            printf("[bench] rep=%d sec=%.6f mips=%.2f\n", r + 1, times[r],
                   times[r] > 0 ? (double)inst / times[r] / 1e6 : 0.0);
        }
        /* median */
        for (int i = 0; i < reps; i++)
            for (int j = i + 1; j < reps; j++)
                if (times[j] < times[i]) {
                    double t = times[i];
                    times[i] = times[j];
                    times[j] = t;
                }
        double med = times[reps / 2];
        printf("[bench] median sec=%.6f inst=%" PRIu64 " mips=%.2f\n", med,
               inst, med > 0 ? (double)inst / med / 1e6 : 0.0);
        free(times);
        uart_free(uart);
        free(flash);
        return 0;
    }

    for (int boot = 1;; boot++) {
        machine_t *m = machine_new(flash, uart);
        if (!m) {
            uart_free(uart);
            free(flash);
            return 1;
        }
        bool reset = machine_run(m);
        if (!reset) {
            machine_free(m);
            break;
        }
        machine_snapshot_flash(m, flash);
        machine_free(m);
        if (g_console_up) {
            console_write("\r\n[rebooting]\r\n", strlen("\r\n[rebooting]\r\n"));
        } else {
            printf("=== rebooting (boot #%d) ===\n", boot + 1);
        }
    }

    if (g_console_up) {
        console_write("\r\n[guest stopped]\r\n", strlen("\r\n[guest stopped]\r\n"));
        console_stop();
        g_console_up = false;
    }
    fflush(stdout);
    uart_free(uart);
    free(flash);
    return 0;
}

/* ---- embedded session API (also used by the GUI) -------------------- */
/*
 * The GUI loads this executable as a DLL and calls _init/_run/_shutdown.
 * Everything a session allocates is released again by _shutdown(), because a
 * GUI keeps the process alive across "power on"/"power off" cycles.
 */

static uint8_t *g_flash;    /* survives software resets, like SPI flash */
static uart_t *g_uart;

static void set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_last_error, sizeof(g_last_error), fmt, ap);
    va_end(ap);
    if (!g_quiet) fprintf(stderr, "%s\n", g_last_error);
}

void _init(void) { g_last_error[0] = 0; }

const char *_last_error(void) { return g_last_error; }

static void collect(char *out, size_t cap, const char *fmt, ...) {
    size_t n = strlen(out);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out + n, cap - n, fmt, ap);
    va_end(ap);
}

/* Release every VM resource; safe to call more than once. */
static void release_vm(void) {
    if (g_console_up) {
        console_write("\r\n[guest stopped]\r\n", strlen("\r\n[guest stopped]\r\n"));
        console_stop();
        g_console_up = false;
    }
    fflush(stdout);
    if (g_uart) {
        uart_free(g_uart);
        g_uart = NULL;
    }
    free(g_flash);
    g_flash = NULL;
}

/* Run one guest session; `brief` keeps stdout to the CLI banner rules. */
static int vm_session(bool brief, char *out, size_t cap) {
    (void)brief;
    if (!opt_elf && !opt_bin && !opt_flashimg) {
        set_error("need -elf, -bin or -flashimg");
        return 2;
    }

    g_flash = (uint8_t *)malloc(FLASH_SIZE);
    if (!g_flash) {
        set_error("out of memory for the flash image");
        return 1;
    }
    memset(g_flash, 0xFF, FLASH_SIZE);
    if (opt_flashimg && opt_flashimg[0]) {
        uint32_t img_len = 0;
        uint8_t *img = load_file(opt_flashimg, &img_len);
        if (!img) {
            set_error("load flash image failed: %s", opt_flashimg);
            release_vm();
            return 1;
        }
        if (img_len > FLASH_SIZE) img_len = FLASH_SIZE;
        memcpy(g_flash, img, img_len);
        free(img);
    }
    if (opt_ramdisk && opt_ramdisk[0]) {
        uint32_t rd_len = 0;
        uint8_t *rd = load_file(opt_ramdisk, &rd_len);
        if (!rd) {
            set_error("load ramdisk failed: %s", opt_ramdisk);
            release_vm();
            return 1;
        }
        const uint32_t flash_off = opt_ramdisk_off;
        if (rd_len > FLASH_SIZE - flash_off) rd_len = FLASH_SIZE - flash_off;
        memcpy(g_flash + flash_off, rd, rd_len);
        if (!g_quiet)
            printf("[flash] ramdisk %u bytes @0x%08X\n", rd_len,
                   FLASH_EMU_BASE + flash_off);
        free(rd);
    }

    g_uart = uart_new();
    if (!g_uart) {
        set_error("cannot create UART");
        release_vm();
        return 1;
    }
    if (opt_console && !opt_bench_mode) {
        g_console_up = console_start();
        if (!g_console_up) {
            set_error("console: cannot start the terminal");
            release_vm();
            return 1;
        }
        console_set_uart(g_uart);
        uart_set_tx_hook(g_uart, console_write, NULL);
    }

    for (int boot = 1;; boot++) {
        machine_t *m = machine_new(g_flash, g_uart);
        if (!m) {
            set_error("machine setup failed (load error?)");
            release_vm();
            return 1;
        }
        bool reset = machine_run(m);
        if (!reset) {
            uint64_t inst = m->cpu->inst_count;
            uint64_t fires = m->timer->fires;
            uint32_t pc = m->cpu->pc;
            machine_free(m);
            if (out && cap)
                collect(out, cap,
                        "halted pc=0x%08X instructions=%" PRIu64
                        " ticks=%" PRIu64 "\n",
                        pc, inst, fires);
            break;
        }
        machine_snapshot_flash(m, g_flash);
        machine_free(m);
        if (g_console_up) {
            console_write("\r\n[rebooting]\r\n", strlen("\r\n[rebooting]\r\n"));
        } else {
            printf("=== rebooting (boot #%d) ===\n", boot + 1);
        }
    }

    release_vm();
    return 0;
}

int _run(const char *config) {
    release_vm();
    if (parse_config(config) != 0) {
        set_error("configuration error");
        return 2;
    }
    char brief_output[1024] = {0};
    return vm_session(true, brief_output, sizeof(brief_output));
}

void _shutdown(void) {
    release_vm();
    g_last_error[0] = 0;
}

