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
    device_t devs[16];
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
} uart_t;

uart_t *uart_new(void);
void    uart_free(uart_t *u);
void    uart_reset(uart_t *u);
void    uart_feed(uart_t *u, int limit);
void    uart_flush(uart_t *u);
int     uart_rx_push(uart_t *u, const uint8_t *data, size_t n);
uint32_t uart_rx_pending(uart_t *u);
void    uart_set_tx_hook(uart_t *u, void (*fn)(const char *, size_t), void *ctx);

typedef struct {
    uint32_t enable;
    uint32_t out;
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
    uint32_t regs[0x1000 / 4];
    bool     reset;
} rtc_t;
rtc_t *rtc_new(void);
void   rtc_free(rtc_t *r);
bool   rtc_reset_pending(rtc_t *r);

typedef struct cpu_t cpu_t;

typedef struct {
    cpu_t   *cpu;
    uint64_t counter;
    uint32_t reload;
    uint64_t fires;
    uint32_t div;
    uint32_t sub;
    uint32_t regs[0x1000 / 4];
} c3timer_t;

c3timer_t *c3timer_new(cpu_t *cpu);
void       c3timer_free(c3timer_t *t);
void       c3timer_set_cpu(c3timer_t *t, cpu_t *cpu);
void       c3timer_set_div(c3timer_t *t, uint32_t d);
void       c3timer_tick_n(c3timer_t *t, uint64_t n);

typedef struct {
    uint32_t regs[0x1000 / 4];
} c3intc_t;
c3intc_t *c3intc_new(void);
void      c3intc_free(c3intc_t *i);

typedef struct {
    bus_t   *bus;
    uint32_t flash_base;
    uint32_t addr;
    uint32_t cmd;
} c3spi1_t;
c3spi1_t *c3spi1_new(bus_t *bus, uint32_t flash_base);
void      c3spi1_free(c3spi1_t *s);

void perip_attach_uart(bus_t *b, uart_t *u);
void perip_attach_gpio(bus_t *b, gpio_t *g);
void perip_attach_usj(bus_t *b, usj_t *u);
void perip_attach_rtc(bus_t *b, rtc_t *r);
void perip_attach_timer(bus_t *b, c3timer_t *t);
void perip_attach_intc(bus_t *b, c3intc_t *i);
void perip_attach_spi1(bus_t *b, c3spi1_t *s);

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

    pmp_entry_t pmp_ent[16];
    int         pmp_n;

    uint32_t priv;
    uint32_t pending_int;
    uint32_t cur_pc;
    bool     pmp_enforce;

    void (*on_trap)(uint32_t cause, uint32_t epc, uint32_t tval);

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
