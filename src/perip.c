#include "rv32.h"

#include <stdlib.h>
#include <string.h>

/* ---- UART0 ---------------------------------------------------------- */

#define UART0_BASE    0x60000000u
#define UART_SIZE     0x00001000u
#define UART_FIFO     0x00u
#define UART_INT_RAW  0x04u
#define UART_INT_ST   0x08u
#define UART_INT_ENA  0x0Cu
#define UART_INT_CLR  0x10u
#define UART_CLKDIV   0x14u
#define UART_STATUS   0x1Cu
#define RX_FIFO_MAX   128
#define RX_IN_CAP     (sizeof(((uart_t *)0)->rx_in))

uart_t *uart_new(void) { return (uart_t *)calloc(1, sizeof(uart_t)); }

void uart_free(uart_t *u) { free(u); }

/* Chip reset: the RX FIFO is lost, bytes still "on the wire" (rx_in) are
 * kept - on real hardware the host is still sending them. */
void uart_reset(uart_t *u) {
    u->rx_buf_len = 0;
    u->tx_len = 0;
}

void uart_set_tx_hook(uart_t *u, void (*fn)(const char *, size_t), void *ctx) {
    u->user_tx = fn;
    u->user_tx_ctx = ctx;
}

/* Move bytes from rx_in into the 128-byte hardware FIFO. */
void uart_feed(uart_t *u, int limit) {
    while (limit > 0 && u->rx_in_head != u->rx_in_tail &&
           u->rx_buf_len < RX_FIFO_MAX) {
        u->rx_buf[u->rx_buf_len++] = u->rx_in[u->rx_in_head];
        u->rx_in_head = (u->rx_in_head + 1) % RX_IN_CAP;
        limit--;
    }
}

/* Accept host bytes for the guest (bounded by the software buffer). */
int uart_rx_push(uart_t *u, const uint8_t *data, size_t n) {
    size_t done = 0;
    while (done < n) {
        size_t next = (u->rx_in_tail + 1) % RX_IN_CAP;
        if (next == u->rx_in_head) break;
        u->rx_in[u->rx_in_tail] = data[done++];
        u->rx_in_tail = next;
    }
    uart_feed(u, (int)(RX_FIFO_MAX - u->rx_buf_len));
    return (int)done;
}

/* Bytes waiting in the FIFO plus the software buffer. */
uint32_t uart_rx_pending(uart_t *u) { return (uint32_t)u->rx_buf_len; }

/* Flush TX bytes that were queued while no terminal hook was installed. */
void uart_flush(uart_t *u) {
    if (u->user_tx) {
        u->tx_len = 0; /* the hook already got everything */
        return;
    }
    if (u->tx_len == 0) return;
    fwrite(u->tx_buf, 1, u->tx_len, stdout);
    fflush(stdout);
    u->tx_len = 0;
}

static void uart_tx_byte(uart_t *u, uint8_t c) {
    if (u->user_tx) {
        char ch = (char)c;
        u->user_tx(&ch, 1);
        return;
    }
    if (u->tx_len == UART_TX_BUF) {
        fwrite(u->tx_buf, 1, u->tx_len, stdout);
        u->tx_len = 0;
    }
    u->tx_buf[u->tx_len++] = c;
}

static uint32_t uart_read(void *ctx, uint32_t off, uint32_t size) {
    (void)size;
    uart_t *u = (uart_t *)ctx;
    switch (off) {
    case UART_FIFO: {
        uint32_t v = 0;
        if (u->rx_buf_len > 0) {
            v = u->rx_buf[0];
            memmove(u->rx_buf, u->rx_buf + 1, --u->rx_buf_len);
            /* keep the FIFO topped up from the host buffer */
            if (u->rx_in_head != u->rx_in_tail) uart_feed(u, 1);
        }
        return v;
    }
    case UART_INT_ST:
        return (uint32_t)u->rx_buf_len & 0xFFu;
    case UART_STATUS:
        return u->rx_buf_len > 0x3FFu ? 0x3FFFu : (uint32_t)u->rx_buf_len;
    default:
        return 0;
    }
}

static void uart_write(void *ctx, uint32_t off, uint32_t size, uint32_t val) {
    (void)size;
    uart_t *u = (uart_t *)ctx;
    switch (off) {
    case UART_FIFO:
        uart_tx_byte(u, (uint8_t)(val & 0xFFu));
        break;
    case UART_INT_ENA:
    case UART_INT_CLR:
    case UART_CLKDIV:
        break;
    default:
        fprintf(stderr, "[uart] unhandled write off=0x%02X val=0x%08X\n",
                off, val);
        break;
    }
}

/* ---- GPIO ----------------------------------------------------------- */

#define GPIO_BASE 0x60004000u
#define GPIO_SIZE 0x00001000u

gpio_t *gpio_new(void) { return (gpio_t *)calloc(1, sizeof(gpio_t)); }
void gpio_free(gpio_t *g) { free(g); }

static uint32_t gpio_read(void *ctx, uint32_t off, uint32_t size) {
    (void)size;
    gpio_t *g = (gpio_t *)ctx;
    if (off == 0x04) return g->enable;
    if (off == 0x08) return g->out;
    return 0;
}

static void gpio_write(void *ctx, uint32_t off, uint32_t size, uint32_t val) {
    (void)size;
    gpio_t *g = (gpio_t *)ctx;
    if (off == 0x04) g->enable = val;
    else if (off == 0x08) g->out = val;
}

/* ---- USJ (USB-Serial/JTAG, black hole) ------------------------------ */

#define USJ_BASE 0x60043000u
#define USJ_SIZE 0x00001000u

usj_t *usj_new(void) { return (usj_t *)calloc(1, sizeof(usj_t)); }
void usj_free(usj_t *u) { free(u); }

static uint32_t usj_read(void *ctx, uint32_t off, uint32_t size) {
    (void)size;
    usj_t *u = (usj_t *)ctx;
    switch (off & ~0x3u) {
    case 0x00:
        return u->ep1 & 0xFFu;
    case 0x04:
        return 1u << 1; /* IN_DATA_FREE */
    case 0x18:
        return u->conf0;
    default:
        return 0;
    }
}

static void usj_write(void *ctx, uint32_t off, uint32_t size, uint32_t val) {
    (void)size;
    usj_t *u = (usj_t *)ctx;
    switch (off & ~0x3u) {
    case 0x00:
        u->ep1 = val & 0xFFu;
        break;
    case 0x04:
        break;
    case 0x18:
        u->conf0 = val;
        break;
    default:
        break;
    }
}

/* ---- RTC_CNTL ------------------------------------------------------- */

#define RTC_BASE 0x60008000u
#define RTC_SIZE 0x00001000u

rtc_t *rtc_new(void) { return (rtc_t *)calloc(1, sizeof(rtc_t)); }
void rtc_free(rtc_t *r) { free(r); }
bool rtc_reset_pending(rtc_t *r) { return r->reset; }

static uint32_t rtc_read(void *ctx, uint32_t off, uint32_t size) {
    (void)size;
    rtc_t *r = (rtc_t *)ctx;
    return r->regs[(off & ~0x3u) >> 2];
}

static void rtc_write(void *ctx, uint32_t off, uint32_t size, uint32_t val) {
    (void)size;
    rtc_t *r = (rtc_t *)ctx;
    uint32_t o = off & ~0x3u;
    r->regs[o >> 2] = val;
    if (o == 0x00 && (val & 0x80000000u)) r->reset = true;
}

/* ---- TIMG0 ---------------------------------------------------------- */

#define TIMG0_BASE 0x6001F000u
#define TIMG0_SIZE 0x00001000u

#define T0_CONFIG    0x00u
#define T0_ALARM_LO  0x10u
#define T0_ALARM_HI  0x14u
#define T0_LOAD_LO   0x18u
#define T0_LOAD_HI   0x1Cu
#define T0_LOAD      0x20u
#define T0_WDT_CFG0  0x48u
#define T0_WDT_FEED  0x60u
#define T0_WDT_PROT  0x64u
#define T0_INT_ENA   0x70u
#define T0_INT_CLR   0x7Cu
#define T0_REG_CLK   0xFCu

c3timer_t *c3timer_new(cpu_t *cpu) {
    c3timer_t *t = (c3timer_t *)calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->cpu = cpu;
    t->reload = 10000;
    t->div = 1;
    return t;
}

void c3timer_free(c3timer_t *t) { free(t); }
void c3timer_set_cpu(c3timer_t *t, cpu_t *cpu) { t->cpu = cpu; }

void c3timer_set_div(c3timer_t *t, uint32_t d) {
    t->div = d ? d : 1;
}

void c3timer_tick_n(c3timer_t *t, uint64_t n) {
    if (n == 0) return;
    if (t->div > 1) {
        uint64_t total = (uint64_t)t->sub + n;
        n = total / t->div;
        t->sub = (uint32_t)(total % t->div);
        if (n == 0) return;
    }
    if (t->reload == 0) {
        t->fires += n;
        cpu_set_pending_int(t->cpu, CAUSE_C3_TICK);
        return;
    }
    uint64_t fires = 0;
    while (n > 0) {
        uint64_t remaining = (uint64_t)t->reload - t->counter;
        if (n < remaining) {
            t->counter += n;
            break;
        }
        n -= remaining;
        t->counter = 0;
        fires++;
    }
    if (fires > 0) {
        t->fires += fires;
        cpu_set_pending_int(t->cpu, CAUSE_C3_TICK);
    }
}

static uint32_t timg_read(void *ctx, uint32_t off, uint32_t size) {
    (void)size;
    c3timer_t *t = (c3timer_t *)ctx;
    return t->regs[(off & ~0x3u) >> 2];
}

static void timg_write(void *ctx, uint32_t off, uint32_t size, uint32_t val) {
    (void)size;
    c3timer_t *t = (c3timer_t *)ctx;
    switch (off) {
    case T0_CONFIG:
        t->regs[off >> 2] = val;
        if (val & 0x80000000u) t->counter = 0;
        break;
    case T0_ALARM_LO:
        t->regs[off >> 2] = val;
        t->reload = val | (t->regs[T0_ALARM_HI >> 2] << 16);
        break;
    case T0_ALARM_HI:
        t->regs[off >> 2] = val;
        t->reload = t->regs[T0_ALARM_LO >> 2] | (val << 16);
        break;
    case T0_LOAD_LO:
    case T0_LOAD_HI:
        t->regs[off >> 2] = val;
        break;
    case T0_LOAD:
        t->counter = 0;
        break;
    case T0_WDT_CFG0:
    case T0_WDT_FEED:
    case T0_WDT_PROT:
    case T0_INT_ENA:
    case T0_INT_CLR:
    case T0_REG_CLK:
        t->regs[off >> 2] = val;
        break;
    default:
        fprintf(stderr, "[c3timg0] unhandled write off=0x%02X val=0x%08X\n",
                off, val);
        break;
    }
}

/* ---- INTMTX (passive) ----------------------------------------------- */

#define INTMTX_BASE 0x600C2000u
#define INTMTX_SIZE 0x00001000u

c3intc_t *c3intc_new(void) { return (c3intc_t *)calloc(1, sizeof(c3intc_t)); }
void c3intc_free(c3intc_t *i) { free(i); }

static uint32_t intc_read(void *ctx, uint32_t off, uint32_t size) {
    (void)size;
    c3intc_t *i = (c3intc_t *)ctx;
    return i->regs[(off & ~0x3u) >> 2];
}

static void intc_write(void *ctx, uint32_t off, uint32_t size, uint32_t val) {
    (void)size;
    c3intc_t *i = (c3intc_t *)ctx;
    i->regs[(off & ~0x3u) >> 2] = val;
}

/* ---- SPI1 ----------------------------------------------------------- */

#define SPI1_BASE 0x60002000u
#define SPI1_SIZE 0x00001000u
#define SPI1_CMD_REG  0x00u
#define SPI1_ADDR_REG 0x04u
#define SPI1_CMD_WREN 0x40000000u
#define SPI1_CMD_SE   0x01000000u

c3spi1_t *c3spi1_new(bus_t *bus, uint32_t flash_base) {
    c3spi1_t *s = (c3spi1_t *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->bus = bus;
    s->flash_base = flash_base;
    return s;
}

void c3spi1_free(c3spi1_t *s) { free(s); }

static uint32_t spi1_read(void *ctx, uint32_t off, uint32_t size) {
    (void)size;
    c3spi1_t *s = (c3spi1_t *)ctx;
    if (off == SPI1_CMD_REG) return s->cmd;
    if (off == SPI1_ADDR_REG) return s->addr;
    return 0;
}

static void spi1_write(void *ctx, uint32_t off, uint32_t size, uint32_t val) {
    (void)size;
    c3spi1_t *s = (c3spi1_t *)ctx;
    if (off == SPI1_ADDR_REG) {
        s->addr = val;
        return;
    }
    if (off != SPI1_CMD_REG) {
        fprintf(stderr, "[c3spi1] unhandled write off=0x%02X val=0x%08X\n",
                off, val);
        return;
    }
    if (val & SPI1_CMD_WREN) val &= ~SPI1_CMD_WREN;
    if (val & SPI1_CMD_SE) {
        uint32_t base = s->flash_base + (s->addr & ~0xFFFu);
        for (uint32_t i = 0; i < 4096; i++) {
            bus_write(s->bus, base + i, 1, 0xFF);
        }
        val &= ~SPI1_CMD_SE;
    }
    s->cmd = val;
}

/* ---- registration helpers (used by main) ---------------------------- */

void perip_attach_uart(bus_t *b, uart_t *u) {
    bus_attach(b, UART0_BASE, UART_SIZE, "UART0",
               uart_read, uart_write, u);
}
void perip_attach_gpio(bus_t *b, gpio_t *g) {
    bus_attach(b, GPIO_BASE, GPIO_SIZE, "GPIO", gpio_read, gpio_write, g);
}
void perip_attach_usj(bus_t *b, usj_t *u) {
    bus_attach(b, USJ_BASE, USJ_SIZE, "USJ", usj_read, usj_write, u);
}
void perip_attach_rtc(bus_t *b, rtc_t *r) {
    bus_attach(b, RTC_BASE, RTC_SIZE, "RTC_CNTL", rtc_read, rtc_write, r);
}
void perip_attach_timer(bus_t *b, c3timer_t *t) {
    bus_attach(b, TIMG0_BASE, TIMG0_SIZE, "C3TIMG0", timg_read, timg_write, t);
}
void perip_attach_intc(bus_t *b, c3intc_t *i) {
    bus_attach(b, INTMTX_BASE, INTMTX_SIZE, "C3INTMTX", intc_read, intc_write, i);
}
void perip_attach_spi1(bus_t *b, c3spi1_t *s) {
    bus_attach(b, SPI1_BASE, SPI1_SIZE, "C3SPI1", spi1_read, spi1_write, s);
}
