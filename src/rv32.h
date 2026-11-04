/*
 * rv32.h - RV32IMC emulator core, C port of the Go rv32emu.
 *
 * The layout and behaviour deliberately mirror the Go implementation:
 *   - a single 32 MiB flat window covering flash/SRAM/ROM
 *   - a decoded basic-block cache (4 KiB page generation counters)
 *   - a fast bulk runner (cpu_run_hot) and a one-instruction runner (cpu_step)
 */
#ifndef RV32_H
#define RV32_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

extern bool g_quiet;



/* ---- address map ---------------------------------------------------- */

#define ROM_BASE        0x40000000u
#define ROM_SIZE        0x00080000u
#define SRAM_BASE       0x3FC80000u
#define SRAM_SIZE       0x00080000u

#define FLAT_BASE       0x3F000000u
#define FLAT_SIZE       0x02000000u   /* 32 MiB */

#define FLASH_EMU_BASE  0x3F000000u
#define FLASH_SIZE      0x00400000u

#define RESET_VECTOR    0x40000000u

/* C3 flash cache / XIP windows (see src/cache.c). */
#define DROM_BASE       0x3C000000u
#define IROM_BASE       0x42000000u
#define XIP_WINDOW_SIZE 0x00800000u
#define CACHE_REG_BASE  0x600C4000u
#define CACHE_REG_SIZE  0x00001200u

/* ROM data area: IDF's heap_caps_init reads ets_rom_layout_p here. */
#define ROM_DATA_BASE        0x3FF00000u
#define ROM_DATA_SIZE        0x00020000u
#define ROM_LAYOUT_ADDR      0x3FF1BE3Cu
#define ROM_LAYOUT_PTR_ADDR  0x3FF1FFFCu

/* ROM service addresses used by the flash trampolines. */
#define ROM_ENTRY_WRITE  0x12Cu
#define ROM_ENTRY_READ   0x130u
#define ROM_ENTRY_CONF   0x134u
#define ROM_IMPL_READ    0x7F000u
#define ROM_IMPL_WRITE   0x7F100u

/* ---- CPU causes ----------------------------------------------------- */

#define CAUSE_ILLEGAL_INSTR       2u
#define CAUSE_LOAD_MISALIGN       4u
#define CAUSE_LOAD_FAULT          5u
#define CAUSE_STORE_MISALIGN      6u
#define CAUSE_STORE_FAULT         7u
#define CAUSE_ECALL_U             8u
#define CAUSE_ECALL_M             11u
#define CAUSE_INTERRUPT_BIT       0x80000000u
#define CAUSE_C3_TICK             (CAUSE_INTERRUPT_BIT | 2u)

/* ---- block cache ---------------------------------------------------- */

#define BLOCK_SLOTS     512
#define BLOCK_MAX_OPS   32

typedef enum {
    K_ADDI = 0, K_SLTI, K_SLTIU, K_XORI, K_ORI, K_ANDI,
    K_SLLI, K_SRLI, K_SRAI,
    K_ADD, K_SUB, K_SLL, K_SLT, K_SLTU, K_XOR, K_SRL, K_SRA, K_OR, K_AND,
    K_MULEXT,
    K_LUI, K_AUIPC, K_JAL, K_JALR,
    K_BEQ, K_BNE, K_BLT, K_BGE, K_BLTU, K_BGEU,
    K_LB, K_LH, K_LW, K_LBU, K_LHU,
    K_SB, K_SH, K_SW,
    K_NOP,
    K_CADDI4SPN, K_CLW, K_CSW,
    K_CADDI, K_CJAL, K_CLI, K_CADDI16SP, K_CLUI,
    K_CSRLI, K_CSRAI, K_CANDI, K_CSUB, K_CXOR, K_COR, K_CAND,
    K_CJ, K_CBEQZ, K_CBNEZ, K_CSLLI, K_CLWSP, K_CSWSP,
    K_CJR, K_CMV, K_CJALR, K_CADD,
    K_KIND_COUNT
} op_kind_t;

typedef struct {
    uint8_t kind;
    uint8_t rd;
    uint8_t rs1;
    uint8_t rs2;
    uint8_t ln;
    int32_t imm;
} h_op_t;

typedef struct {
    uint32_t start;
    uint32_t gen;
    uint8_t n;
    h_op_t ops[BLOCK_MAX_OPS];
} h_block_t;

/* ---- bus ------------------------------------------------------------ */

typedef struct device device_t;
struct device {
    uint32_t base;
    uint32_t size;
    const char *name;
    uint32_t (*read)(void *ctx, uint32_t off, uint32_t size);
    void (*write)(void *ctx, uint32_t off, uint32_t size, uint32_t val);
    void *ctx;
};

typedef struct {
    uint32_t base;
    uint32_t size;
    uint8_t *data;
    bool owned;
} ram_region_t;

typedef struct {
    uint8_t *flat;
    uint8_t *rom;
    uint8_t *sram;
    device_t devs[64];
    int ndev;
    ram_region_t rams[32];
    int nram;
    uint8_t *code_page;
    uint32_t *code_gen;
    uint32_t unmapped_r[12];
    int nunmapped_r;
    uint32_t unmapped_w[12];
    int nunmapped_w;
} bus_t;

bus_t   *bus_new(void);
void     bus_free(bus_t *b);
void     bus_attach(bus_t *b, uint32_t base, uint32_t size, const char *name,
                    uint32_t (*rd)(void *, uint32_t, uint32_t),
                    void (*wr)(void *, uint32_t, uint32_t, uint32_t), void *ctx);
uint32_t bus_read(bus_t *b, uint32_t addr, uint32_t size);
void     bus_write(bus_t *b, uint32_t addr, uint32_t size, uint32_t val);
uint32_t bus_fast_read(bus_t *b, uint32_t addr, uint32_t size);
void     bus_fast_write(bus_t *b, uint32_t addr, uint32_t size, uint32_t val);
void     bus_load_rom(bus_t *b, uint32_t off, const uint8_t *data, uint32_t len);
void     bus_add_ram(bus_t *b, uint32_t base, uint32_t size);
void     bus_extend_ram(bus_t *b, uint32_t base, uint32_t need);
uint32_t bus_ram_size_at(bus_t *b, uint32_t addr);
void     bus_copy_into(bus_t *b, uint32_t base, uint32_t off,
                       const uint8_t *data, uint32_t len);
void     bus_copy_out(bus_t *b, uint32_t base, uint8_t *dst, uint32_t len);
void     bus_touch_code(bus_t *b, uint32_t addr);

/* ---- per peripherals ------------------------------------------------ */

/* UART0 host side. `user_tx` (set by the terminal) receives the guest's TX
 * bytes right away; without it bytes are queued in `tx_buf` and flushed by
 * uart_feed()/uart_flush() from the machine loop (stdout otherwise). */
#define UART_TX_BUF 65536

typedef struct c3intc c3intc_t;

typedef struct {
    uint8_t  tx_buf[UART_TX_BUF];
    size_t   tx_len;

    uint8_t  rx_in[65536];  /* bytes accepted from the host, not yet in the FIFO */
    size_t   rx_in_head;
    size_t   rx_in_tail;
    uint8_t  rx_buf[128];   /* hardware RX FIFO */
    size_t   rx_buf_len;

    void   (*user_tx)(const char *data, size_t n);
    void    *user_tx_ctx;

    /* C3 register side: only INT_ENA/CONF1 and a few config registers are
     * meaningful; the rest is shadow storage so a driver can read back what
     * it wrote. */
    uint32_t regs[0x1000 / 4];
    bool     tout;        /* RX timeout latch */
    bool     discard_tx;  /* UART1: transmit into the void */
    uint32_t src;         /* INTMTX source (21 = UART0, 22 = UART1) */
    c3intc_t *intc;
} uart_t;

uart_t *uart_new(void);
void    uart_free(uart_t *u);
void    uart_reset(uart_t *u);
void    uart_feed(uart_t *u, int limit);
void    uart_flush(uart_t *u);
int     uart_rx_push(uart_t *u, const uint8_t *data, size_t n);
uint32_t uart_rx_pending(uart_t *u);
void    uart_set_tx_hook(uart_t *u, void (*fn)(const char *, size_t), void *ctx);
void    uart_set_intc(uart_t *u, c3intc_t *i);

typedef struct {
    uint32_t enable;
    uint32_t out;
    /* IO_MUX lives at 0x60009000, a separate block with one register per
     * pad.  Modelled as plain storage: nothing reads it back in a way that
     * matters, but a driver must be able to write it without the bus
     * complaining about an unmapped address. */
    uint32_t mux[0x1000 / 4];
} gpio_t;
gpio_t *gpio_new(void);
void    gpio_free(gpio_t *g);

typedef struct {
    uint32_t conf0;
    uint32_t ep1;
} usj_t;
usj_t *usj_new(void);
void   usj_free(usj_t *u);

typedef struct {
    uint32_t regs[0x800 / 4];  /* RTC_CNTL is 0x800 bytes; eFuse follows */
    uint64_t rtc_time;         /* RTC slow-clock counter, 1 tick per inst */
    bool     reset;

    uint32_t wdt_cfg0;
    uint32_t wdt_hold[4];
    bool     wdt_unlock;
    uint64_t wdt_count;
    uint64_t wdt_fires;
    bool     wdt_reset;

    uint32_t swd_conf;
    bool     swd_unlock;
    bool     swd_reset_flag;
    uint64_t swd_feeds;
} rtc_t;
rtc_t *rtc_new(void);
void   rtc_free(rtc_t *r);
bool   rtc_reset_pending(rtc_t *r);
bool   rtc_wdt_reset_pending(rtc_t *r);
void   rtc_tick(rtc_t *r, uint64_t n);

typedef struct cpu_t cpu_t;

typedef struct {
    cpu_t   *cpu;
    c3intc_t *intc;
    uint64_t counter;
    uint32_t reload;
    uint64_t fires;
    uint32_t div;
    uint32_t sub;
    uint32_t regs[0x1000 / 4];

    /* TG0WDT (MWDT): CONFIG0.EN, CONFIG1 prescale[31:16], CONFIG2 stage0. */
    uint32_t wdt_cfg0;
    uint32_t wdt_hold;
    uint32_t wdt_prescale;
    uint64_t wdt_count;
    uint64_t wdt_fires;
    bool     wdt_unlock;
    bool     wdt_reset;

    /* T0 alarm level: raised on fire, cleared by INT_CLR bit0. */
    bool     tick_pending;

    /* RTC calibration (0x68/0x6C/0x80). */
    uint32_t rtc_cali_cfg;
    uint32_t rtc_cali_val;
    uint32_t rtc_cali_cfg2;
} c3timer_t;

c3timer_t *c3timer_new(cpu_t *cpu);
void       c3timer_free(c3timer_t *t);
void       c3timer_set_cpu(c3timer_t *t, cpu_t *cpu);
void       c3timer_set_intc(c3timer_t *t, c3intc_t *i);
void       c3timer_set_div(c3timer_t *t, uint32_t d);
void       c3timer_tick_n(c3timer_t *t, uint64_t n);
bool       c3timer_wdt_reset_pending(c3timer_t *t);

struct c3intc {
    uint32_t regs[0x1000 / 4];
    uint8_t  pending[128];  /* level state per INTMTX source */
    cpu_t   *cpu;
};
c3intc_t *c3intc_new(void);
void      c3intc_free(c3intc_t *i);
void      c3intc_set_cpu(c3intc_t *i, cpu_t *cpu);
void      c3intc_raise(c3intc_t *i, uint32_t src);
void      c3intc_clear_source(c3intc_t *i, uint32_t src);
int       c3intc_mapped_line(c3intc_t *i, uint32_t src);
void      c3intc_deliver(c3intc_t *i);

/* SPI0 (cache/flash controller) and SPI1 (flash programming) share one
 * SPI_MEM register layout. */
typedef struct {
    bus_t   *bus;
    uint32_t flash_base;
    const char *name;
    uint32_t addr;
    uint32_t cmd;
    uint32_t status;      /* flash status: bit0 WIP, bit1 WEL */
    uint32_t regs[0x1000 / 4];
} c3spi_t;
c3spi_t *c3spi_new(bus_t *bus, uint32_t flash_base, const char *name);
void     c3spi_free(c3spi_t *s);

/* SYSTIMER: 2x52-bit counters + 3 comparators (esp_timer / FreeRTOS tick). */
typedef struct {
    uint32_t conf;
    uint64_t units[2];
    uint32_t loads[2][2];
    uint64_t targets[3];
    uint32_t target_cfg[3];
    uint64_t comps[3];
    bool     fired[3];
    uint32_t int_ena;
    uint32_t int_raw;
    uint64_t fires[3];
    uint32_t regs[0x1000 / 4];
    c3intc_t *intc;
} c3systimer_t;
c3systimer_t *c3systimer_new(void);
void          c3systimer_free(c3systimer_t *s);
void          c3systimer_set_intc(c3systimer_t *s, c3intc_t *i);
void          c3systimer_tick_n(c3systimer_t *s, uint64_t n);

typedef struct { uint32_t regs[0x200 / 4]; } c3efuse_t;
c3efuse_t *c3efuse_new(void);
void       c3efuse_free(c3efuse_t *e);

typedef struct {
    uint32_t regs[0x1000 / 4];
    uint32_t channels[5];
} c3adc_t;
c3adc_t *c3adc_new(void);
void     c3adc_free(c3adc_t *a);
bool     c3adc_set_channel(c3adc_t *a, int ch, uint32_t raw);

typedef struct {
    const char *name;
    uint32_t regs[0x1000 / 4];
} c3shadow_t;
c3shadow_t *c3shadow_new(const char *name);
void        c3shadow_free(c3shadow_t *s);

/* UART1 + SPI2 + I2C0 + register shadow windows. */
typedef struct {
    uart_t *uart1;
    uint32_t spi2_regs[0x1000 / 4];
    uint32_t spi2_raw;
    uint32_t i2c_regs[0x1000 / 4];
    uint8_t  i2c_tx[16];
    int      i2c_tx_len;
    uint8_t  i2c_rx[16];
    int      i2c_rx_len;
    uint32_t i2c_raw;
    uint32_t sys_regs[0x1000 / 4];
    c3intc_t *intc;
    c3shadow_t *shadows[24];
    int      nshadow;
} c3extras_t;
c3extras_t *c3extras_new(bus_t *b, c3intc_t *intc);
void        c3extras_free(c3extras_t *e);

/* Clock tree, derived from SYSTEM_SYSCLK_CONF / SYSTEM_CPU_PER_CONF.  Every
 * consumer asks these instead of assuming a rate: the guest programs its
 * dividers from the same two registers. */
uint32_t c3clk_cpu_hz(bus_t *b);
uint32_t c3clk_apb_hz(bus_t *b);

/* Flash cache / XIP MMU (EXTMEM control regs + 128-entry page table). */
typedef struct flashmmu flashmmu_t;
typedef struct {
    flashmmu_t *mmu;
    uint32_t regs[0x1000 / 4];
} c3cache_t;

flashmmu_t *flashmmu_new(bus_t *bus, uint32_t flash_base, uint32_t flash_size);
void        flashmmu_free(flashmmu_t *m);
void        flashmmu_map(flashmmu_t *m, uint32_t vaddr, uint32_t flash_off,
                         uint32_t size);
uint32_t    flashmmu_read(flashmmu_t *m, uint32_t addr, uint32_t size);
void        flashmmu_write(flashmmu_t *m, uint32_t addr, uint32_t size,
                           uint32_t val);
c3cache_t  *c3cache_new(flashmmu_t *m);
void        c3cache_free(c3cache_t *c);

typedef struct {
    flashmmu_t *mmu;
    uint32_t base;
    const char *name;
} c3xip_t;
c3xip_t *c3xip_new(flashmmu_t *m, uint32_t base, const char *name);
void     c3xip_free(c3xip_t *w);

bool image_load_app(const uint8_t *flash, uint32_t flash_size, bus_t *bus,
                    flashmmu_t *mmu, uint32_t *entry);

void perip_attach_uart(bus_t *b, uart_t *u);
void perip_attach_uart_at(bus_t *b, uart_t *u, uint32_t base,
                          const char *name);
void perip_attach_gpio(bus_t *b, gpio_t *g);
void perip_attach_iomux(bus_t *b, gpio_t *g);
void perip_attach_usj(bus_t *b, usj_t *u);
void perip_attach_rtc(bus_t *b, rtc_t *r);
void perip_attach_timer(bus_t *b, c3timer_t *t);
void perip_attach_intc(bus_t *b, c3intc_t *i);
void perip_attach_spi(bus_t *b, c3spi_t *s, uint32_t base);
void perip_attach_systimer(bus_t *b, c3systimer_t *s);
void perip_attach_efuse(bus_t *b, c3efuse_t *e);
void perip_attach_adc(bus_t *b, c3adc_t *a);
void perip_attach_cache(bus_t *b, c3cache_t *c);
void perip_attach_xip(bus_t *b, c3xip_t *w);

/* ---- ROM service ---------------------------------------------------- */

typedef struct rom_service rom_service_t;

rom_service_t *rom_service_new(bus_t *bus, uart_t *uart);
void           rom_service_free(rom_service_t *s);
void           rom_service_call(rom_service_t *s, cpu_t *c, uint32_t addr);
void           rom_service_load_rom_data(bus_t *bus);

/* ---- CPU ------------------------------------------------------------ */

typedef struct {
    uint32_t perm;
    uint32_t base;
    uint32_t top;
    bool     all;
} pmp_entry_t;

struct cpu_t {
    uint32_t x[32];
    uint32_t pc;
    bus_t   *bus;

    uint8_t *flat;
    uint32_t flat_base;
    uint32_t flat_size;

    uint8_t *code_page;
    uint32_t *code_gen;

    h_block_t blocks[BLOCK_SLOTS];

    /* CSR file */
    uint32_t csr_mstatus;
    uint32_t csr_misa;
    uint32_t csr_mie;
    uint32_t csr_mtvec;
    uint32_t csr_mscratch;
    uint32_t csr_mepc;
    uint32_t csr_mcause;
    uint32_t csr_mtval;
    uint32_t csr_mip;
    uint64_t csr_mcycle;
    uint64_t csr_minstret;
    uint32_t pmpcfg[4];
    uint32_t pmpaddr[16];
    bool     pmp_dirty;

    /* ESP32-C3 custom CSRs (0x7C0..0x7FF, e.g. mhint/mintthresh and the
     * clock-related CSR 0x7E2 that esp_clk_init reads and writes). The
     * firmware accesses them during startup; they carry no semantics we
     * need, but must not be treated as illegal instructions. */
    uint32_t csr_custom[64];

    pmp_entry_t pmp_ent[16];
    int         pmp_n;

    uint32_t priv;
    uint32_t pending_int;
    uint32_t cur_pc;
    bool     pmp_enforce;

    void (*on_trap)(uint32_t cause, uint32_t epc, uint32_t tval);

    /* ROM call hook: an illegal instruction whose PC lies in the ROM window
     * is dispatched to the service instead of raising a trap. */
    void (*rom_call)(void *ctx, cpu_t *c, uint32_t addr);
    void *rom_ctx;

    bool    halted;
    char    halt_msg[32];
    uint64_t exc_count;
    uint64_t int_count;
    uint64_t inst_count;
};

cpu_t   *cpu_new(bus_t *bus);
void     cpu_free(cpu_t *c);
bool     cpu_step(cpu_t *c);
uint64_t cpu_run_hot(cpu_t *c, uint64_t max);
void     cpu_exec_m(cpu_t *c, uint32_t f3, int rd, uint32_t a, uint32_t b);
void     cpu_set_pending_int(cpu_t *c, uint32_t cause);
void     cpu_sample_interrupt(cpu_t *c);
void     cpu_set_pmp_enforce(cpu_t *c, bool v);
uint32_t cpu_reg(cpu_t *c, int i);
void     cpu_set_reg(cpu_t *c, int i, uint32_t v);
void     cpu_set_pc(cpu_t *c, uint32_t pc);
void     cpu_dump(cpu_t *c, char *buf, size_t cap);
bool     cpu_csr_read(cpu_t *c, uint32_t addr, uint32_t *val);
bool     cpu_csr_write(cpu_t *c, uint32_t addr, uint32_t val);

/* ---- ROM / ELF ------------------------------------------------------ */

void     rom_build(uint8_t *rom);
uint32_t elf_load(const char *path, bus_t *bus);
int      elf_load_bin(const char *path, uint32_t base, bus_t *bus);

/* ---- disassembler --------------------------------------------------- */

void disasm_i(uint32_t pc, uint32_t instr, char *buf, size_t cap);
void disasm_c(uint16_t in, char *buf, size_t cap);

#endif /* RV32_H */
