/*
 * ESP32-C3 extra peripherals: UART1, SPI2 (GP-SPI), I2C0, SYSTEM software
 * interrupts, and register shadow windows.  Mirrors the Go c3more.go model:
 * funcion-less blocks only need to read back what firmware wrote, so bare
 * clock/pin init paths do not land on unmapped addresses.
 */

#include <stdlib.h>
#include <string.h>

#include "rv32.h"

#define SYSTEM_BASE   0x600C0000u
#define SENSITIVE_BASE 0x600C1000u
#define SYSCON_BASE   0x60026000u
#define TIMG1_BASE    0x60020000u
#define LEDC_BASE     0x60019000u
#define RMT_BASE      0x60016000u
#define TWAI_BASE     0x6002B000u
#define I2S0_BASE     0x6002D000u
#define UHCI0_BASE    0x60014000u
#define RTC_I2C_BASE  0x6000E000u
#define DEDICATED_GPIO_BASE 0x600CF000u
#define WORLD_CNTL_BASE 0x600D0000u
#define GDMA_BASE     0x6003F000u
#define AES_BASE      0x6003A000u
#define SHA_BASE      0x6003B000u
#define RSA_BASE      0x6003C000u
#define DS_BASE       0x6003D000u
#define HMAC_BASE     0x6003E000u
#define AES_XTS_BASE  0x600CC000u
#define ASSIST_DEBUG_BASE 0x600CE000u

#define SPI2_BASE 0x60024000u
#define I2C0_BASE 0x60013000u
#define UART1_BASE 0x60010000u

#define GSPI_CMD_REG   0x00u
#define GSPI_USER_REG  0x10u
#define GSPI_USER1_REG 0x14u
#define GSPI_USER2_REG 0x18u
#define GSPI_MS_DLEN   0x1Cu
#define GSPI_INT_ENA   0x34u
#define GSPI_INT_CLR   0x38u
#define GSPI_INT_RAW   0x3Cu
#define GSPI_INT_ST    0x40u
#define GSPI_W0_REG    0x98u

#define GSPI_CMD_USR    (1u << 24)
#define GSPI_CMD_UPDATE (1u << 23)
#define GSPI_USER_MISO  (1u << 28)
#define GSPI_USER_MOSI  (1u << 27)
#define GSPI_TRANS_DONE (1u << 12)
#define SPI2_SRC 19u

#define I2C_CTR_REG    0x04u
#define I2C_SR_REG     0x08u
#define I2C_FIFO_CONF  0x18u
#define I2C_DATA_REG   0x1Cu
#define I2C_INT_RAW    0x20u
#define I2C_INT_CLR    0x24u
#define I2C_INT_ENA    0x28u
#define I2C_INT_ST     0x2Cu
#define I2C_COMD0_REG  0x58u

#define I2C_TRANS_START (1u << 5)
#define I2C_TX_FIFO_RST (1u << 13)
#define I2C_RX_FIFO_RST (1u << 12)
#define I2C_INT_TX_WM   (1u << 1)
#define I2C_INT_END_DET (1u << 3)
#define I2C_INT_TRANS_COMPLETE (1u << 7)
#define I2C_INT_TRANS_START (1u << 9)
#define I2C_CMD_DONE    (1u << 31)
#define I2C_OPCODE_SHIFT 11
#define I2C_FIFO_DEPTH 16
#define I2C_SRC 29u

#define SYS_CPU_INTR0  0x28u
#define SYS_FROM_CPU_SRC0 50u

/* ---- SPI2 ----------------------------------------------------------- */

static void spi2_sync(c3extras_t *e) {
    if (!e->intc) return;
    if (e->spi2_raw & e->spi2_regs[GSPI_INT_ENA / 4]) {
        c3intc_raise(e->intc, SPI2_SRC);
    } else {
        c3intc_clear_source(e->intc, SPI2_SRC);
    }
    c3intc_deliver(e->intc);
}

static void spi2_transfer(c3extras_t *e) {
    uint32_t user = e->spi2_regs[GSPI_USER_REG / 4];
    uint32_t bit_len = (e->spi2_regs[GSPI_MS_DLEN / 4] & 0x3FFFFu) + 1;
    uint32_t n = (bit_len + 7) / 8;
    if (n > 64) n = 64;
    if (user & GSPI_USER_MISO) {
        if ((user & GSPI_USER_MOSI) == 0) {
            for (uint32_t i = 0; i < n; i++) {
                uint32_t reg = GSPI_W0_REG + (i / 4) * 4;
                uint32_t shift = 8 * (i % 4);
                e->spi2_regs[reg / 4] &= ~(0xFFu << shift);
            }
        }
    }
    e->spi2_raw |= GSPI_TRANS_DONE;
    spi2_sync(e);
}

static uint32_t spi2_read(void *ctx, uint32_t off, uint32_t size) {
    c3extras_t *e = ctx;
    (void)size;
    off &= ~3u;
    switch (off) {
    case GSPI_CMD_REG:
        return e->spi2_regs[off / 4] & ~(GSPI_CMD_USR | GSPI_CMD_UPDATE);
    case GSPI_INT_RAW:
        return e->spi2_raw;
    case GSPI_INT_ST:
        return e->spi2_raw & e->spi2_regs[GSPI_INT_ENA / 4];
    }
    return e->spi2_regs[off / 4];
}

static void spi2_write(void *ctx, uint32_t off, uint32_t size, uint32_t val) {
    c3extras_t *e = ctx;
    (void)size;
    off &= ~3u;
    switch (off) {
    case GSPI_CMD_REG:
        e->spi2_regs[off / 4] = val & ~(GSPI_CMD_USR | GSPI_CMD_UPDATE);
        if (val & GSPI_CMD_USR) spi2_transfer(e);
        break;
    case GSPI_INT_CLR:
        e->spi2_raw &= ~val;
        spi2_sync(e);
        break;
    case GSPI_INT_ENA:
        e->spi2_regs[off / 4] = val;
        spi2_sync(e);
        break;
    default:
        e->spi2_regs[off / 4] = val;
        break;
    }
}

/* ---- I2C0 ----------------------------------------------------------- */

static void i2c_sync(c3extras_t *e) {
    if (!e->intc) return;
    if (e->i2c_raw & e->i2c_regs[I2C_INT_ENA / 4]) {
        c3intc_raise(e->intc, I2C_SRC);
    } else {
        c3intc_clear_source(e->intc, I2C_SRC);
    }
    c3intc_deliver(e->intc);
}

static void i2c_run_commands(c3extras_t *e) {
    for (int n = 0; n < 8; n++) {
        uint32_t reg = I2C_COMD0_REG + 4u * (uint32_t)n;
        uint32_t cmd = e->i2c_regs[reg / 4];
        uint32_t op = (cmd >> I2C_OPCODE_SHIFT) & 0x7u;
        int byte_num = (int)(cmd & 0xFFu);
        if (op == 1) {  /* WRITE */
            if (byte_num > e->i2c_tx_len) byte_num = e->i2c_tx_len;
            e->i2c_tx_len -= byte_num;
            if (e->i2c_tx_len > 0) {
                memmove(e->i2c_tx, e->i2c_tx + byte_num, (size_t)e->i2c_tx_len);
            }
        } else if (op == 2) {  /* READ */
            for (int k = 0; k < byte_num && e->i2c_rx_len < I2C_FIFO_DEPTH; k++) {
                e->i2c_rx[e->i2c_rx_len++] = 0;
            }
        }
        e->i2c_regs[reg / 4] |= I2C_CMD_DONE;
    }
    e->i2c_raw |= I2C_INT_TRANS_START | I2C_INT_TRANS_COMPLETE |
                  I2C_INT_END_DET | I2C_INT_TX_WM;
    i2c_sync(e);
}

static uint32_t i2c_read(void *ctx, uint32_t off, uint32_t size) {
    c3extras_t *e = ctx;
    (void)size;
    off &= ~3u;
    switch (off) {
    case I2C_SR_REG:
        return (((uint32_t)e->i2c_rx_len & 0x3Fu) << 8) |
               (((uint32_t)e->i2c_tx_len & 0x3Fu) << 18);
    case I2C_FIFO_CONF:
        return e->i2c_regs[off / 4] & ~(I2C_TX_FIFO_RST | I2C_RX_FIFO_RST);
    case I2C_DATA_REG:
        if (e->i2c_rx_len == 0) return 0;
        {
            uint32_t b = e->i2c_rx[0];
            memmove(e->i2c_rx, e->i2c_rx + 1, (size_t)(e->i2c_rx_len - 1));
            e->i2c_rx_len--;
            return b;
        }
    case I2C_INT_RAW:
        return e->i2c_raw;
    case I2C_INT_ST:
        return e->i2c_raw & e->i2c_regs[I2C_INT_ENA / 4];
    }
    return e->i2c_regs[off / 4];
}

static void i2c_write(void *ctx, uint32_t off, uint32_t size, uint32_t val) {
    c3extras_t *e = ctx;
    (void)size;
    off &= ~3u;
    switch (off) {
    case I2C_CTR_REG:
        e->i2c_regs[off / 4] = val & ~I2C_TRANS_START;
        if (val & I2C_TRANS_START) i2c_run_commands(e);
        break;
    case I2C_FIFO_CONF:
        e->i2c_regs[off / 4] = val & ~(I2C_TX_FIFO_RST | I2C_RX_FIFO_RST);
        if (val & I2C_TX_FIFO_RST) e->i2c_tx_len = 0;
        if (val & I2C_RX_FIFO_RST) e->i2c_rx_len = 0;
        break;
    case I2C_DATA_REG:
        if (e->i2c_tx_len < I2C_FIFO_DEPTH) {
            e->i2c_tx[e->i2c_tx_len++] = (uint8_t)val;
        }
        break;
    case I2C_INT_CLR:
        e->i2c_raw &= ~val;
        i2c_sync(e);
        break;
    case I2C_INT_ENA:
        e->i2c_regs[off / 4] = val;
        i2c_sync(e);
        break;
    default:
        e->i2c_regs[off / 4] = val;
        break;
    }
}

/* ---- SYSTEM (CPU software interrupts) ------------------------------- */

static uint32_t sys_read(void *ctx, uint32_t off, uint32_t size) {
    c3extras_t *e = ctx;
    (void)size;
    return e->sys_regs[(off & ~3u) / 4];
}

static void sys_write(void *ctx, uint32_t off, uint32_t size, uint32_t val) {
    c3extras_t *e = ctx;
    (void)size;
    uint32_t o = off & ~3u;
    e->sys_regs[o / 4] = val;
    if (o != SYS_CPU_INTR0 || !e->intc) return;
    if (val & 1u) c3intc_raise(e->intc, SYS_FROM_CPU_SRC0);
    else c3intc_clear_source(e->intc, SYS_FROM_CPU_SRC0);
    c3intc_deliver(e->intc);
}

/* ---- shadows -------------------------------------------------------- */

c3shadow_t *c3shadow_new(const char *name) {
    c3shadow_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->name = name;
    return s;
}

void c3shadow_free(c3shadow_t *s) { free(s); }

static uint32_t shadow_read(void *ctx, uint32_t off, uint32_t size) {
    c3shadow_t *s = ctx;
    (void)size;
    return s->regs[(off & ~3u) / 4];
}

static void shadow_write(void *ctx, uint32_t off, uint32_t size,
                         uint32_t val) {
    c3shadow_t *s = ctx;
    (void)size;
    s->regs[(off & ~3u) / 4] = val;
}

static void attach_shadow(bus_t *b, c3extras_t *e, uint32_t base,
                          const char *name) {
    if (e->nshadow >= (int)(sizeof(e->shadows) / sizeof(e->shadows[0]))) return;
    c3shadow_t *s = c3shadow_new(name);
    if (!s) return;
    e->shadows[e->nshadow++] = s;
    bus_attach(b, base, 0x1000u, name, shadow_read, shadow_write, s);
}

c3extras_t *c3extras_new(bus_t *b, c3intc_t *intc) {
    c3extras_t *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->intc = intc;

    e->uart1 = uart_new();
    if (!e->uart1) {
        free(e);
        return NULL;
    }
    e->uart1->discard_tx = true;
    e->uart1->src = 22;
    uart_set_intc(e->uart1, intc);
    perip_attach_uart_at(b, e->uart1, UART1_BASE, "UART1");

    bus_attach(b, SPI2_BASE, 0x1000u, "SPI2", spi2_read, spi2_write, e);
    bus_attach(b, I2C0_BASE, 0x1000u, "I2C0", i2c_read, i2c_write, e);
    bus_attach(b, SYSTEM_BASE, 0x1000u, "SYSTEM", sys_read, sys_write, e);

    attach_shadow(b, e, SENSITIVE_BASE, "SENSITIVE");
    /* IO_MUX is already functional in perip.c; no shadow duplicate. */
    attach_shadow(b, e, SYSCON_BASE, "SYSCON");
    attach_shadow(b, e, TIMG1_BASE, "TIMG1");
    attach_shadow(b, e, LEDC_BASE, "LEDC");
    attach_shadow(b, e, RMT_BASE, "RMT");
    attach_shadow(b, e, TWAI_BASE, "TWAI");
    attach_shadow(b, e, I2S0_BASE, "I2S0");
    attach_shadow(b, e, UHCI0_BASE, "UHCI0");
    attach_shadow(b, e, RTC_I2C_BASE, "RTC_I2C");
    attach_shadow(b, e, DEDICATED_GPIO_BASE, "DEDICATED_GPIO");
    attach_shadow(b, e, WORLD_CNTL_BASE, "WORLD_CNTL");
    attach_shadow(b, e, GDMA_BASE, "GDMA");
    attach_shadow(b, e, AES_BASE, "AES");
    attach_shadow(b, e, SHA_BASE, "SHA");
    attach_shadow(b, e, RSA_BASE, "RSA");
    attach_shadow(b, e, DS_BASE, "DS");
    attach_shadow(b, e, HMAC_BASE, "HMAC");
    attach_shadow(b, e, AES_XTS_BASE, "AES_XTS");
    attach_shadow(b, e, ASSIST_DEBUG_BASE, "ASSIST_DEBUG");
    return e;
}

void c3extras_free(c3extras_t *e) {
    if (!e) return;
    for (int i = 0; i < e->nshadow; i++) c3shadow_free(e->shadows[i]);
    uart_free(e->uart1);
    free(e);
}
