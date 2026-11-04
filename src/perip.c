#include "rv32.h"

#include <stdlib.h>
#include <string.h>

/* ---- UART0 / UART1 -------------------------------------------------- */

#define UART0_BASE    0x60000000u
#define UART1_BASE    0x60010000u
#define UART_SIZE     0x00001000u
#define UART_FIFO     0x00u
#define UART_INT_RAW  0x04u
#define UART_INT_ST   0x08u
#define UART_INT_ENA  0x0Cu
#define UART_INT_CLR  0x10u
#define UART_CLKDIV   0x14u
#define UART_STATUS   0x1Cu
#define UART_CONF1    0x24u
#define UART_MEM_CONF 0x60u
#define RX_FIFO_MAX   128
#define RX_IN_CAP     (sizeof(((uart_t *)0)->rx_in))

#define UART_INT_RXFIFO_FULL (1u << 0)
#define UART_INT_RXFIFO_TOUT (1u << 8)
#define UART_CONF1_RX_TOUT_EN (1u << 21)

uart_t *uart_new(void) { return (uart_t *)calloc(1, sizeof(uart_t)); }

void uart_free(uart_t *u) { free(u); }

void uart_set_intc(uart_t *u, c3intc_t *i) { u->intc = i; }

/* RAW = level (FIFO >= threshold) + latched RX timeout. */
static uint32_t uart_int_raw(uart_t *u) {
    uint32_t raw = 0;
    uint32_t n = (uint32_t)u->rx_buf_len;
    uint32_t thr = u->regs[UART_CONF1 / 4] & 0x1Fu;
    if (n > 0 && n >= thr) raw |= UART_INT_RXFIFO_FULL;
    if (u->tout) raw |= UART_INT_RXFIFO_TOUT;
    return raw;
}

static uint32_t uart_source(uart_t *u) {
    if (u->src != 0) return u->src;
    return 21u;
}

static void uart_sync_irq(uart_t *u) {
    if (!u->intc) return;
    if (uart_int_raw(u) & u->regs[UART_INT_ENA / 4]) {
        c3intc_raise(u->intc, uart_source(u));
    } else {
        c3intc_clear_source(u->intc, uart_source(u));
    }
    c3intc_deliver(u->intc);
}

/* Chip reset: the RX FIFO is lost, bytes still "on the wire" (rx_in) are
 * kept - on real hardware the host is still sending them. */
void uart_reset(uart_t *u) {
    u->rx_buf_len = 0;
    u->tx_len = 0;
    u->tout = false;
    memset(u->regs, 0, sizeof(u->regs));
    uart_sync_irq(u);
}

void uart_set_tx_hook(uart_t *u, void (*fn)(const char *, size_t), void *ctx) {
    u->user_tx = fn;
    u->user_tx_ctx = ctx;
}

/* Move bytes from rx_in into the 128-byte hardware FIFO. */
void uart_feed(uart_t *u, int limit) {
    bool moved = false;
    while (limit > 0 && u->rx_in_head != u->rx_in_tail &&
           u->rx_buf_len < RX_FIFO_MAX) {
        u->rx_buf[u->rx_buf_len++] = u->rx_in[u->rx_in_head];
        u->rx_in_head = (u->rx_in_head + 1) % RX_IN_CAP;
        limit--;
        moved = true;
    }
    /* New bytes latch the receive-timeout bit when it is enabled. */
    if (moved && (u->regs[UART_CONF1 / 4] & UART_CONF1_RX_TOUT_EN)) {
        u->tout = true;
    }
    if (moved) uart_sync_irq(u);
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
    if (u->discard_tx) return;
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
    switch (off & ~3u) {
    case UART_FIFO: {
        uint32_t v = 0;
        if (u->rx_buf_len > 0) {
            v = u->rx_buf[0];
            memmove(u->rx_buf, u->rx_buf + 1, --u->rx_buf_len);
            /* FIFO read empty: hardware drops the timeout latch. */
            if (u->rx_buf_len == 0) u->tout = false;
            /* keep the FIFO topped up from the host buffer */
            if (u->rx_in_head != u->rx_in_tail) uart_feed(u, 1);
        }
        uart_sync_irq(u);
        return v;
    }
    case UART_INT_RAW:
        return uart_int_raw(u);
    case UART_INT_ST:
        return uart_int_raw(u) & u->regs[UART_INT_ENA / 4];
    case UART_INT_ENA:
        return u->regs[UART_INT_ENA / 4];
    case UART_STATUS:
        return u->rx_buf_len > 0x3FFu ? 0x3FFu : (uint32_t)u->rx_buf_len;
    default:
        return u->regs[(off & ~3u) / 4];
    }
}

static void uart_write(void *ctx, uint32_t off, uint32_t size, uint32_t val) {
    (void)size;
    uart_t *u = (uart_t *)ctx;
    switch (off & ~3u) {
    case UART_FIFO:
        uart_tx_byte(u, (uint8_t)(val & 0xFFu));
        break;
    case UART_INT_ENA:
    case UART_CONF1:
    case UART_MEM_CONF:
    case UART_CLKDIV:
        u->regs[(off & ~3u) / 4] = val;
        uart_sync_irq(u);
        break;
    case UART_INT_CLR:
        /* TOUT is a latch; FULL is a level and clears itself. */
        if (val & UART_INT_RXFIFO_TOUT) u->tout = false;
        uart_sync_irq(u);
        break;
    default:
        /* Other configuration registers are shadow storage. */
        u->regs[(off & ~3u) / 4] = val;
        break;
    }
}

/* ---- GPIO ----------------------------------------------------------- */

/* The ESP32-C3 layout, because the board driver talks to these registers:
 * OUT at 0x04 with write-1-to-set / write-1-to-clear mirrors at 0x08/0x0C,
 * ENABLE at 0x20 with the same pair at 0x24/0x28, and IN at 0x3C.  A pin
 * that is driven reads back what it drives, which is how a real pad with
 * its input enabled behaves - and it is what makes `gpio 4 1; gpio 4`
 * testable without wiring anything up. */
#define GPIO_BASE 0x60004000u
#define GPIO_SIZE 0x00001000u

gpio_t *gpio_new(void) { return (gpio_t *)calloc(1, sizeof(gpio_t)); }
void gpio_free(gpio_t *g) { free(g); }

static uint32_t gpio_read(void *ctx, uint32_t off, uint32_t size) {
    (void)size;
    gpio_t *g = (gpio_t *)ctx;
    switch (off & ~0x3u) {
    case 0x04: return g->out;
    case 0x20: return g->enable;
    case 0x3C: return g->out & g->enable;
    default:   return 0;
    }
}

static void gpio_write(void *ctx, uint32_t off, uint32_t size, uint32_t val) {
    (void)size;
    gpio_t *g = (gpio_t *)ctx;
    switch (off & ~0x3u) {
    case 0x04: g->out = val; break;
    case 0x08: g->out |= val; break;
    case 0x0C: g->out &= ~val; break;
    case 0x20: g->enable = val; break;
    case 0x24: g->enable |= val; break;
    case 0x28: g->enable &= ~val; break;
    default:
        /* 0x554.. is the GPIO matrix (FUNCn_OUT_SEL / OEN_SEL) and the
         * IO_MUX block lives elsewhere; neither has behaviour we model, so
         * writes are accepted and forgotten rather than reported as
         * unhandled - a driver configuring a pad is not an error. */
        break;
    }
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
#define RTC_SIZE 0x00000800u  /* eFuse starts at 0x60008800 */

#define RTC_OPTIONS0     0x00u
#define RTC_TIME_UPDATE  0x0Cu
#define RTC_TIME_LO0     0x10u
#define RTC_TIME_HI0     0x14u
#define RTC_TIME_LO1     0xDCu
#define RTC_TIME_HI1     0xE0u
#define RTC_WDT_CFG0     0x90u
#define RTC_WDT_CFG1     0x94u
#define RTC_WDT_CFG2     0x98u
#define RTC_WDT_CFG3     0x9Cu
#define RTC_WDT_CFG4     0xA0u
#define RTC_WDT_FEED     0xA4u
#define RTC_WDT_PROTECT  0xA8u
#define RTC_SWD_CONF     0xACu
#define RTC_SWD_PROTECT  0xB0u
#define RTC_SW_CPU_STALL 0xB4u

#define RTC_SW_SYS_RST    (1u << 31)
#define RTC_WDT_EN        (1u << 31)
#define RTC_WDT_FEED_B    (1u << 31)
#define RTC_WDT_KEY       0x50D83AA1u
#define RTC_SWD_KEY       0x8F1D312Au
#define RTC_SWD_DISABLE   (1u << 30)
#define RTC_SWD_FEED      (1u << 29)
#define RTC_SWD_RST_FLAG_CLR (1u << 28)
#define RTC_SWD_RESET_FLAG   (1u << 0)

rtc_t *rtc_new(void) {
    rtc_t *r = (rtc_t *)calloc(1, sizeof(*r));
    if (!r) return NULL;
    /* WKEY reset value is the unlock word: unlocked after reset. */
    r->regs[RTC_WDT_PROTECT / 4] = RTC_WDT_KEY;
    r->regs[RTC_SWD_PROTECT / 4] = RTC_SWD_KEY;
    r->wdt_unlock = true;
    r->swd_unlock = true;
    r->wdt_hold[0] = 200000;
    r->wdt_hold[1] = 80000;
    r->wdt_hold[2] = 0xFFF;
    r->wdt_hold[3] = 0xFFF;
    return r;
}

void rtc_free(rtc_t *r) { free(r); }
bool rtc_reset_pending(rtc_t *r) { return r->reset; }
bool rtc_wdt_reset_pending(rtc_t *r) { return r->wdt_reset; }

void rtc_tick(rtc_t *r, uint64_t n) {
    r->rtc_time += n;
    if ((r->wdt_cfg0 & RTC_WDT_EN) == 0 || r->wdt_hold[0] == 0 ||
        r->wdt_reset) {
        return;
    }
    uint32_t action = (r->wdt_cfg0 >> 28) & 0x7u;
    if (action < 2) return;  /* off / interrupt only */
    r->wdt_count += n;
    if (r->wdt_count >= r->wdt_hold[0]) {
        r->wdt_count = r->wdt_hold[0];
        r->wdt_fires++;
        r->wdt_reset = true;
    }
}

static uint32_t rtc_read(void *ctx, uint32_t off, uint32_t size) {
    (void)size;
    rtc_t *r = (rtc_t *)ctx;
    uint32_t o = off & ~0x3u;
    if (o == RTC_SWD_CONF) {
        uint32_t v = r->swd_conf & ~RTC_SWD_RESET_FLAG;
        if (r->swd_reset_flag) v |= RTC_SWD_RESET_FLAG;
        return v;
    }
    if (o == RTC_WDT_FEED || o == RTC_TIME_UPDATE) return 0;
    switch (o) {
    case RTC_TIME_LO0:
    case RTC_TIME_LO1:
        return (uint32_t)r->rtc_time;
    case RTC_TIME_HI0:
    case RTC_TIME_HI1:
        return (uint32_t)(r->rtc_time >> 32);
    default:
        return r->regs[o / 4];
    }
}

static void rtc_write(void *ctx, uint32_t off, uint32_t size, uint32_t val) {
    (void)size;
    rtc_t *r = (rtc_t *)ctx;
    uint32_t o = off & ~0x3u;
    if (o == RTC_WDT_FEED) {
        if (val & RTC_WDT_FEED_B) r->wdt_count = 0;
        return;
    }
    if (o == RTC_SWD_CONF) {
        if (r->swd_unlock) {
            if (val & RTC_SWD_FEED) r->swd_feeds++;
            if (val & RTC_SWD_RST_FLAG_CLR) r->swd_reset_flag = false;
            r->swd_conf = val & ~(RTC_SWD_FEED | RTC_SWD_RST_FLAG_CLR |
                                  RTC_SWD_RESET_FLAG);
            r->regs[o / 4] = r->swd_conf;
        }
        return;
    }
    switch (o) {
    case RTC_OPTIONS0:
        r->regs[o / 4] = val;
        if (val & RTC_SW_SYS_RST) r->reset = true;
        break;
    case RTC_WDT_CFG0:
        if (!r->wdt_unlock) return;
        r->regs[o / 4] = val;
        if ((val & RTC_WDT_EN) && (r->wdt_cfg0 & RTC_WDT_EN) == 0) {
            r->wdt_count = 0;
        }
        r->wdt_cfg0 = val;
        if ((val & RTC_WDT_EN) == 0) r->wdt_count = 0;
        break;
    case RTC_WDT_CFG1:
    case RTC_WDT_CFG2:
    case RTC_WDT_CFG3:
    case RTC_WDT_CFG4:
        if (!r->wdt_unlock) return;
        r->regs[o / 4] = val;
        r->wdt_hold[(o - RTC_WDT_CFG1) / 4] = val;
        break;
    case RTC_WDT_PROTECT:
        r->regs[o / 4] = val;
        r->wdt_unlock = val == RTC_WDT_KEY;
        break;
    case RTC_SWD_PROTECT:
        r->regs[o / 4] = val;
        r->swd_unlock = val == RTC_SWD_KEY;
        break;
    default:
        r->regs[o / 4] = val;
        break;
    }
}

/* ---- TIMG0 ---------------------------------------------------------- */

#define TIMG0_BASE 0x6001F000u
#define TIMG0_SIZE 0x00001000u

#define T0_CONFIG    0x00u
#define T0_LO        0x04u
#define T0_HI        0x08u
#define T0_UPDATE    0x0Cu
#define T0_ALARM_LO  0x10u
#define T0_ALARM_HI  0x14u
#define T0_LOAD_LO   0x18u
#define T0_LOAD_HI   0x1Cu
#define T0_LOAD      0x20u
#define T0_WDT_CFG0  0x48u
#define T0_WDT_CFG1  0x4Cu
#define T0_WDT_CFG2  0x50u
#define T0_WDT_CFG3  0x54u
#define T0_WDT_CFG4  0x58u
#define T0_WDT_CFG5  0x5Cu
#define T0_WDT_FEED  0x60u
#define T0_WDT_PROT  0x64u
#define T0_RTC_CALI_CFG 0x68u
#define T0_RTC_CALI_1   0x6Cu
#define T0_INT_ENA   0x70u
#define T0_INT_CLR   0x7Cu
#define T0_RTC_CALI_2   0x80u
#define T0_REG_CLK   0xFCu

#define T0_EN        (1u << 31)
#define T0_ALARM_EN  (1u << 10)
#define T0_DIVIDER_S 13
#define T0_DIVIDER_M 0xFFFFu
#define T0_WDT_EN    (1u << 31)
#define T0_WDT_PROTECT_KEY 0x50D83AA1u
#define T0_RTC_CALI_START (1u << 31)
#define T0_RTC_CALI_RDY   (1u << 15)
#define T0_RTC_CALI_CFG_DEFAULT 0x0001B000u

#define TIMER_SRC_G0 27u  /* ETS_TG0_T0_LEVEL in the Go model */
#define TIMER_SRC_TG0 32u /* source number used by TinyOS on real silicon */

c3timer_t *c3timer_new(cpu_t *cpu) {
    c3timer_t *t = (c3timer_t *)calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->cpu = cpu;
    t->reload = 10000;
    t->div = 1;
    t->wdt_prescale = 1;
    t->rtc_cali_cfg = T0_RTC_CALI_CFG_DEFAULT;
    return t;
}

void c3timer_free(c3timer_t *t) { free(t); }
void c3timer_set_cpu(c3timer_t *t, cpu_t *cpu) { t->cpu = cpu; }
void c3timer_set_intc(c3timer_t *t, c3intc_t *i) { t->intc = i; }

void c3timer_set_div(c3timer_t *t, uint32_t d) {
    t->div = d ? d : 1;
}

bool c3timer_wdt_reset_pending(c3timer_t *t) { return t->wdt_reset; }

static void timer_sync_intc(c3timer_t *t) {
    if (!t->cpu) return;
    bool mapped = t->intc &&
                  (c3intc_mapped_line(t->intc, TIMER_SRC_G0) >= 0 ||
                   c3intc_mapped_line(t->intc, TIMER_SRC_TG0) >= 0);
    if (mapped) {
        if (t->tick_pending) {
            c3intc_raise(t->intc, TIMER_SRC_G0);
            c3intc_raise(t->intc, TIMER_SRC_TG0);
        } else {
            c3intc_clear_source(t->intc, TIMER_SRC_G0);
            c3intc_clear_source(t->intc, TIMER_SRC_TG0);
        }
        c3intc_deliver(t->intc);
        return;
    }
    if (t->tick_pending) cpu_set_pending_int(t->cpu, CAUSE_C3_TICK);
}

static void timer_raise_tick(c3timer_t *t) {
    if (!t->cpu) return;
    t->tick_pending = true;
    timer_sync_intc(t);
}

static void timer_tick_wdt(c3timer_t *t, uint64_t n) {
    if ((t->wdt_cfg0 & T0_WDT_EN) == 0 || t->wdt_hold == 0 || t->wdt_reset) {
        return;
    }
    uint64_t limit = (uint64_t)t->wdt_hold * (uint64_t)t->wdt_prescale;
    if (limit == 0) limit = t->wdt_hold;
    t->wdt_count += n;
    if (t->wdt_count >= limit) {
        t->wdt_count = limit;
        t->wdt_fires++;
        t->wdt_reset = true;
    }
}

void c3timer_tick_n(c3timer_t *t, uint64_t n) {
    if (n == 0) return;
    if (t->div > 1) {
        uint64_t total = (uint64_t)t->sub + n;
        n = total / t->div;
        t->sub = (uint32_t)(total % t->div);
        if (n == 0) return;
    }
    timer_tick_wdt(t, n);
    /* T0 only fires when firmware armed both EN and ALARM_EN - reset state
     * of a real chip is off, and IDF uses SYSTIMER on the C3. */
    if ((t->regs[T0_CONFIG / 4] & T0_EN) == 0 ||
        (t->regs[T0_CONFIG / 4] & T0_ALARM_EN) == 0) {
        return;
    }
    if (t->reload == 0) {
        t->fires += n;
        timer_raise_tick(t);
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
        timer_raise_tick(t);
    }
}

static void timer_start_rtc_cali(c3timer_t *t) {
    uint32_t max = (t->rtc_cali_cfg >> 16) & 0x7FFFu;
    if (max == 0) max = 1;
    uint32_t sel = (t->rtc_cali_cfg >> 13) & 3u;
    uint32_t slow_hz;
    switch (sel) {
    case 0: slow_hz = 150000u; break;
    case 1: slow_hz = 8000000u / 256u; break;
    default: slow_hz = 32768u; break;
    }
    uint64_t cycles = (uint64_t)max * 40000000ull / slow_hz;
    t->rtc_cali_val = (uint32_t)(cycles & 0x1FFFFFFu) << 7;
}

static uint32_t timg_read(void *ctx, uint32_t off, uint32_t size) {
    (void)size;
    c3timer_t *t = (c3timer_t *)ctx;
    switch (off & ~0x3u) {
    /* The time base.  The guest reads it for sub-tick time (plat_now_ns):
     * real hardware latches it when software writes T0UPDATE, and the two
     * halves are consistent here by construction. */
    case T0_LO: return (uint32_t)t->counter;
    case T0_HI: return (uint32_t)(t->counter >> 32);
    case T0_UPDATE: return 0;
    case T0_RTC_CALI_CFG: return t->rtc_cali_cfg;
    case T0_RTC_CALI_1: return t->rtc_cali_val;
    case T0_RTC_CALI_2: return t->rtc_cali_cfg2;
    default: return t->regs[(off & ~0x3u) >> 2];
    }
}

static void timg_write(void *ctx, uint32_t off, uint32_t size, uint32_t val) {
    (void)size;
    c3timer_t *t = (c3timer_t *)ctx;
    uint32_t o = off & ~0x3u;
    switch (o) {
    case T0_CONFIG:
        t->regs[o >> 2] = val;
        /* Bits 13:28 divide the APB clock feeding the counter.  The field
         * holds the divisor itself, not divisor-1, and 0 (the reset value)
         * means 65536 - both pinned on a real C3 by board/esp32c3/platform.c
         * of TinyOS, which therefore writes the divisor it wants. */
        c3timer_set_div(t, ((val >> T0_DIVIDER_S) & T0_DIVIDER_M)
                               ? ((val >> T0_DIVIDER_S) & T0_DIVIDER_M)
                               : 65536u);
        if (val & T0_EN) t->counter = 0;
        break;
    case T0_ALARM_LO:
        t->regs[o >> 2] = val;
        t->reload = val | (t->regs[T0_ALARM_HI >> 2] << 16);
        break;
    case T0_ALARM_HI:
        t->regs[o >> 2] = val;
        t->reload = t->regs[T0_ALARM_LO >> 2] | (val << 16);
        break;
    case T0_LOAD_LO:
    case T0_LOAD_HI:
        t->regs[o >> 2] = val;
        break;
    case T0_UPDATE:
        /* Latch request: the value registers are already live here. */
        break;
    case T0_LOAD:
        t->counter = 0;
        break;
    case T0_RTC_CALI_CFG:
        t->rtc_cali_cfg = (val & ~T0_RTC_CALI_RDY) |
                          (t->rtc_cali_cfg & T0_RTC_CALI_RDY);
        if (val & T0_RTC_CALI_START) {
            timer_start_rtc_cali(t);
            t->rtc_cali_cfg &= ~T0_RTC_CALI_START;
            t->rtc_cali_cfg |= T0_RTC_CALI_RDY;
        }
        break;
    case T0_RTC_CALI_2:
        t->rtc_cali_cfg2 = val & ~1u;
        break;
    case T0_WDT_CFG0:
        if (t->wdt_unlock) {
            if ((val & T0_WDT_EN) && (t->wdt_cfg0 & T0_WDT_EN) == 0) {
                t->wdt_count = 0;
            }
            t->regs[o >> 2] = val;
            t->wdt_cfg0 = val;
            if ((val & T0_WDT_EN) == 0) t->wdt_count = 0;
        }
        break;
    case T0_WDT_CFG1:
        if (t->wdt_unlock) {
            t->regs[o >> 2] = val;
            t->wdt_prescale = (val >> 16) ? (val >> 16) : 1;
            if (val & 1u) t->wdt_count = 0;
        }
        break;
    case T0_WDT_CFG2:
        if (t->wdt_unlock) {
            t->regs[o >> 2] = val;
            t->wdt_hold = val;
        }
        break;
    case T0_WDT_CFG3:
    case T0_WDT_CFG4:
    case T0_WDT_CFG5:
        if (t->wdt_unlock) t->regs[o >> 2] = val;
        break;
    case T0_WDT_FEED:
        t->regs[o >> 2] = val;
        t->wdt_count = 0;
        break;
    case T0_WDT_PROT:
        t->regs[o >> 2] = val;
        t->wdt_unlock = val == T0_WDT_PROTECT_KEY;
        break;
    case T0_INT_ENA:
        t->regs[o >> 2] = val;
        timer_sync_intc(t);
        break;
    case T0_INT_CLR:
        t->regs[o >> 2] = val;
        if (val & 1u) t->tick_pending = false;
        timer_sync_intc(t);
        break;
    case T0_REG_CLK:
        t->regs[o >> 2] = val;
        break;
    default:
        fprintf(stderr, "[c3timg0] unhandled write off=0x%02X val=0x%08X\n",
                o, val);
        break;
    }
}

/* ---- INTMTX (active routing) --------------------------------------- */

#define INTMTX_BASE 0x600C2000u
#define INTMTX_SIZE 0x00001000u

#define INTC_ENA    0x104u
#define INTC_TYPE   0x108u
#define INTC_CLEAR  0x10Cu
#define INTC_EIP    0x110u
#define INTC_PRI_BASE 0x114u
#define INTC_THRESH 0x194u

c3intc_t *c3intc_new(void) { return (c3intc_t *)calloc(1, sizeof(c3intc_t)); }
void c3intc_free(c3intc_t *i) { free(i); }
void c3intc_set_cpu(c3intc_t *i, cpu_t *c) { i->cpu = c; }

void c3intc_raise(c3intc_t *i, uint32_t src) {
    if (src < 128) i->pending[src] = 1;
}

void c3intc_clear_source(c3intc_t *i, uint32_t src) {
    if (src < 128) i->pending[src] = 0;
}

int c3intc_mapped_line(c3intc_t *i, uint32_t src) {
    if (src >= 128) return -1;
    uint32_t v = i->regs[src];
    if (v == 0) return -1;
    return (int)(v & 0x1Fu);
}

static bool source_maps_to(c3intc_t *i, uint32_t src, int line) {
    return src < 128 && (int)(i->regs[src] & 0x1Fu) == line;
}

void c3intc_deliver(c3intc_t *i) {
    if (!i->cpu) return;
    uint32_t ena = i->regs[INTC_ENA / 4];
    uint32_t thresh = i->regs[INTC_THRESH / 4] & 0xFu;
    int best = -1;
    int best_pri = -1;
    for (int line = 0; line < 32; line++) {
        if ((ena & (1u << line)) == 0 || line == 1 || line == 6) continue;
        int pri = (int)(i->regs[(INTC_PRI_BASE / 4) + line] & 0xFu);
        if (pri < (int)thresh) continue;
        bool hit = false;
        for (uint32_t src = 0; src < 128; src++) {
            if (i->pending[src] && source_maps_to(i, src, line)) {
                hit = true;
                break;
            }
        }
        if (hit && pri > best_pri) {
            best = line;
            best_pri = pri;
        }
    }
    if (best >= 0) cpu_set_pending_int(i->cpu, 0x80000000u | (uint32_t)best);
}

static uint32_t intc_eip(c3intc_t *i) {
    uint32_t ena = i->regs[INTC_ENA / 4];
    uint32_t thresh = i->regs[INTC_THRESH / 4] & 0xFu;
    uint32_t st = 0;
    for (uint32_t src = 0; src < 128; src++) {
        if (!i->pending[src]) continue;
        uint32_t line = i->regs[src] & 0x1Fu;
        if (line >= 32 || (ena & (1u << line)) == 0 || line == 1 || line == 6) {
            continue;
        }
        if ((i->regs[(INTC_PRI_BASE / 4) + line] & 0xFu) < thresh) continue;
        st |= 1u << line;
    }
    return st;
}

static uint32_t intc_read(void *ctx, uint32_t off, uint32_t size) {
    (void)size;
    c3intc_t *i = (c3intc_t *)ctx;
    uint32_t o = off & ~0x3u;
    if (o == INTC_EIP) return intc_eip(i);
    return i->regs[o / 4];
}

static void intc_write(void *ctx, uint32_t off, uint32_t size, uint32_t val) {
    (void)size;
    c3intc_t *i = (c3intc_t *)ctx;
    uint32_t o = off & ~0x3u;
    i->regs[o / 4] = val;
    if (o < 0x100 || o == INTC_ENA || o == INTC_THRESH || o == INTC_TYPE ||
        o == INTC_CLEAR || o == INTC_EIP) {
        c3intc_deliver(i);
    }
}

/* ---- SPI_MEM (SPI0/SPI1 flash controller) --------------------------- */

#define SPI0_BASE 0x60003000u
#define SPI1_BASE 0x60002000u
#define SPI_SIZE 0x00001000u

#define SPI_CMD_REG  0x00u
#define SPI_ADDR_REG 0x04u
#define SPI_USER_REG 0x18u
#define SPI_USER1_REG 0x1Cu
#define SPI_USER2_REG 0x20u
#define SPI_MOSI_DLEN 0x24u
#define SPI_MISO_DLEN 0x28u
#define SPI_RD_STATUS 0x2Cu
#define SPI_W0_REG   0x58u
#define SPI_W15_REG  0x94u

#define SPI_CMD_FLASH_PE   (1u << 17)
#define SPI_CMD_USR        (1u << 18)
#define SPI_CMD_FLASH_CE   (1u << 22)
#define SPI_CMD_FLASH_BE   (1u << 23)
#define SPI_CMD_FLASH_SE   (1u << 24)
#define SPI_CMD_FLASH_PP   (1u << 25)
#define SPI_CMD_FLASH_RSR  (1u << 27)
#define SPI_CMD_FLASH_ID   (1u << 28)
#define SPI_CMD_FLASH_WRDI (1u << 29)
#define SPI_CMD_FLASH_WR   (1u << 30)
#define SPI_CMD_FLASH_RD   (1u << 31)

#define ST_WIP (1u << 0)
#define ST_WEL (1u << 1)
#define FLASH_JEDEC_ID 0x1640C8u
#define FLASH_MASK 0x3FFFFFu

c3spi_t *c3spi_new(bus_t *bus, uint32_t flash_base, const char *name) {
    c3spi_t *s = (c3spi_t *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->bus = bus;
    s->flash_base = flash_base;
    s->name = name;
    return s;
}

void c3spi_free(c3spi_t *s) { free(s); }

static int spi_len_bits(uint32_t v) {
    int n = (int)((v & 0x3FFu) + 1) / 8;
    if (n > 64) n = 64;
    return n;
}

static void spi_set_w_bytes(c3spi_t *s, const uint8_t *data, int n) {
    for (int i = 0; i < n; i++) {
        uint32_t reg = SPI_W0_REG + (uint32_t)(i / 4) * 4;
        uint32_t shift = (uint32_t)(8 * (i % 4));
        s->regs[reg / 4] &= ~(0xFFu << shift);
        s->regs[reg / 4] |= (uint32_t)data[i] << shift;
    }
}

static void spi_w_bytes(c3spi_t *s, uint8_t *out, int n) {
    for (int i = 0; i < n; i++) {
        uint32_t reg = SPI_W0_REG + (uint32_t)(i / 4) * 4;
        uint32_t shift = (uint32_t)(8 * (i % 4));
        out[i] = (uint8_t)(s->regs[reg / 4] >> shift);
    }
}

static void spi_read_flash(c3spi_t *s, uint32_t addr, int n) {
    if (n <= 0) return;
    uint8_t buf[64];
    for (int i = 0; i < 16; i++) s->regs[(SPI_W0_REG / 4) + (uint32_t)i] = 0;
    for (int i = 0; i < n; i++) {
        buf[i] = (uint8_t)bus_read(s->bus,
                                   s->flash_base + ((addr + (uint32_t)i) & FLASH_MASK),
                                   1);
    }
    spi_set_w_bytes(s, buf, n);
}

static void spi_program_page(c3spi_t *s, uint32_t addr) {
    int n = spi_len_bits(s->regs[SPI_MOSI_DLEN / 4]);
    if (n <= 0) return;
    uint8_t data[64];
    spi_w_bytes(s, data, n);
    for (int i = 0; i < n; i++) {
        bus_write(s->bus, s->flash_base + ((addr + (uint32_t)i) & FLASH_MASK), 1,
                  data[i]);
    }
}

static void spi_erase_block(c3spi_t *s, uint32_t addr, uint32_t size) {
    uint32_t base = addr & ~(size - 1);
    for (uint32_t i = 0; i < size; i++) {
        bus_write(s->bus, s->flash_base + ((base + i) & FLASH_MASK), 1, 0xFF);
    }
}

static void spi_user_command(c3spi_t *s) {
    uint32_t cmd = s->regs[SPI_USER2_REG / 4] & 0xFFFFu;
    int miso_len = spi_len_bits(s->regs[SPI_MISO_DLEN / 4]);
    uint32_t addr = s->addr;

    switch (cmd) {
    case 0x9F: { /* RDID */
        uint8_t id[3] = {0xC8, 0x40, 0x16};
        s->regs[SPI_RD_STATUS / 4] = FLASH_JEDEC_ID;
        spi_set_w_bytes(s, id, 3);
        break;
    }
    case 0x05: { /* RDSR */
        uint8_t sr = (uint8_t)s->status;
        s->regs[SPI_RD_STATUS / 4] = s->status;
        spi_set_w_bytes(s, &sr, 1);
        break;
    }
    case 0x35: { /* RDSR2 */
        uint8_t z = 0;
        s->regs[SPI_RD_STATUS / 4] = 0;
        spi_set_w_bytes(s, &z, 1);
        break;
    }
    case 0x03: case 0x0B: case 0x3B: case 0xBB: /* READ / FAST READ */
        spi_read_flash(s, addr, miso_len);
        break;
    case 0x02: case 0x32: case 0x38: /* PP / QPP */
        spi_program_page(s, addr);
        s->status &= ~ST_WEL;
        break;
    case 0x06: /* WREN */
        s->status |= ST_WEL;
        break;
    case 0x04: /* WRDI */
        s->status &= ~ST_WEL;
        break;
    case 0x20: /* SE */
        spi_erase_block(s, addr, 0x1000);
        s->status &= ~ST_WEL;
        break;
    case 0xD8: /* BE (64 KiB) */
        spi_erase_block(s, addr, 0x10000);
        s->status &= ~ST_WEL;
        break;
    case 0xC7: case 0x60: /* CE */
        spi_erase_block(s, 0, 0x400000);
        s->status &= ~ST_WEL;
        break;
    case 0x01: case 0x31: { /* WRSR */
        uint8_t sr;
        spi_w_bytes(s, &sr, 1);
        s->status = (s->status & (ST_WIP | ST_WEL)) | (sr & ~(ST_WIP | ST_WEL));
        break;
    }
    default:
        break;
    }
}

static void spi_fire_commands(c3spi_t *s, uint32_t val) {
    uint32_t done = 0;
    if (val & SPI_CMD_FLASH_WR) {
        s->status |= ST_WEL;
        done |= SPI_CMD_FLASH_WR;
    }
    if (val & SPI_CMD_FLASH_WRDI) {
        s->status &= ~ST_WEL;
        done |= SPI_CMD_FLASH_WRDI;
    }
    if (val & SPI_CMD_FLASH_ID) {
        s->regs[SPI_RD_STATUS / 4] = FLASH_JEDEC_ID;
        done |= SPI_CMD_FLASH_ID;
    }
    if (val & SPI_CMD_FLASH_RSR) {
        s->regs[SPI_RD_STATUS / 4] = s->status;
        done |= SPI_CMD_FLASH_RSR;
    }
    if (val & SPI_CMD_FLASH_SE) {
        spi_erase_block(s, s->addr, 0x1000);
        s->status &= ~ST_WEL;
        done |= SPI_CMD_FLASH_SE;
    }
    if (val & SPI_CMD_FLASH_BE) {
        spi_erase_block(s, s->addr, 0x8000);
        s->status &= ~ST_WEL;
        done |= SPI_CMD_FLASH_BE;
    }
    if (val & SPI_CMD_FLASH_CE) {
        spi_erase_block(s, 0, 0x400000);
        s->status &= ~ST_WEL;
        done |= SPI_CMD_FLASH_CE;
    }
    if (val & SPI_CMD_FLASH_PP) {
        spi_program_page(s, s->addr);
        s->status &= ~ST_WEL;
        done |= SPI_CMD_FLASH_PP;
    }
    if (val & SPI_CMD_FLASH_RD) {
        spi_read_flash(s, s->addr, spi_len_bits(s->regs[SPI_MISO_DLEN / 4]));
        done |= SPI_CMD_FLASH_RD;
    }
    if (val & SPI_CMD_USR) {
        spi_user_command(s);
        done |= SPI_CMD_USR | SPI_CMD_FLASH_PE;
    }
    s->cmd = val & ~done;
}

static uint32_t spi_read(void *ctx, uint32_t off, uint32_t size) {
    c3spi_t *s = (c3spi_t *)ctx;
    switch (off) {
    case SPI_CMD_REG:
        return s->cmd;
    case SPI_ADDR_REG:
        return s->addr;
    }
    /* W0..W15 allow byte addressing: firmware memcpy()s from them. */
    if (off >= SPI_W0_REG && off <= SPI_W15_REG + 3) {
        uint32_t base = off & ~3u;
        uint32_t v = s->regs[base / 4] >> ((off & 3u) * 8);
        if (size < 4) v &= (1u << (8 * size)) - 1u;
        return v;
    }
    return s->regs[(off & ~3u) / 4];
}

static void spi_write(void *ctx, uint32_t off, uint32_t size, uint32_t val) {
    c3spi_t *s = (c3spi_t *)ctx;
    switch (off & ~3u) {
    case SPI_ADDR_REG:
        s->addr = val;
        break;
    case SPI_CMD_REG:
        spi_fire_commands(s, val);
        break;
    default:
        if ((off & ~3u) >= SPI_W0_REG && (off & ~3u) <= SPI_W15_REG + 3) {
            uint32_t base = off & ~3u;
            uint32_t shift = (off & 3u) * 8;
            uint32_t mask = 0xFFFFFFFFu;
            if (size < 4) mask = (1u << (8 * size)) - 1u;
            s->regs[base / 4] = (s->regs[base / 4] & ~(mask << shift)) |
                                ((val & mask) << shift);
            return;
        }
        s->regs[(off & ~3u) / 4] = val;
        break;
    }
}

/* ---- eFuse ---------------------------------------------------------- */

#define EFUSE_BASE 0x60008800u
#define EFUSE_SIZE 0x00000200u
#define EFUSE_MAC_SPI_SYS_0 0x44u
#define EFUSE_MAC_SPI_SYS_1 0x48u
#define EFUSE_MAC_SPI_SYS_3 0x50u

c3efuse_t *c3efuse_new(void) {
    c3efuse_t *e = (c3efuse_t *)calloc(1, sizeof(*e));
    if (!e) return NULL;
    /* Fixed MAC AC:67:B2:12:34:56 and wafer version v0.4. */
    e->regs[EFUSE_MAC_SPI_SYS_0 / 4] = 0xAC67B212u;
    e->regs[EFUSE_MAC_SPI_SYS_1 / 4] = 0x00003456u;
    e->regs[EFUSE_MAC_SPI_SYS_3 / 4] = 4u << 18;
    return e;
}

void c3efuse_free(c3efuse_t *e) { free(e); }

static uint32_t efuse_read(void *ctx, uint32_t off, uint32_t size) {
    (void)size;
    c3efuse_t *e = (c3efuse_t *)ctx;
    return e->regs[(off & ~3u) / 4];
}

static void efuse_write(void *ctx, uint32_t off, uint32_t size, uint32_t val) {
    /* eFuse is read-only in the model: programming is not emulated. */
    (void)ctx; (void)off; (void)size; (void)val;
}

/* ---- ADC (APB_SARADC, minimal) -------------------------------------- */

#define ADC_BASE 0x60040000u
#define ADC_SIZE 0x00001000u
#define ADC_DATA1_STATUS 0x2Cu
#define ADC_INT_RAW 0x44u

c3adc_t *c3adc_new(void) { return (c3adc_t *)calloc(1, sizeof(c3adc_t)); }
void c3adc_free(c3adc_t *a) { free(a); }

bool c3adc_set_channel(c3adc_t *a, int ch, uint32_t raw) {
    if (ch < 0 || ch >= 5) return false;
    a->channels[ch] = raw & 0xFFFu;
    return true;
}

static uint32_t adc_read(void *ctx, uint32_t off, uint32_t size) {
    (void)size;
    c3adc_t *a = (c3adc_t *)ctx;
    switch (off & ~3u) {
    case ADC_DATA1_STATUS:
        return a->channels[0] & 0xFFFu;
    case ADC_INT_RAW:
        /* Conversion always completes immediately in the model. */
        return 0xC0000000u;
    default:
        return a->regs[(off & ~3u) / 4];
    }
}

static void adc_write(void *ctx, uint32_t off, uint32_t size, uint32_t val) {
    (void)size;
    c3adc_t *a = (c3adc_t *)ctx;
    a->regs[(off & ~3u) / 4] = val;
}

/* ---- registration helpers (used by main) ---------------------------- */

void perip_attach_uart_at(bus_t *b, uart_t *u, uint32_t base, const char *name) {
    bus_attach(b, base, UART_SIZE, name, uart_read, uart_write, u);
}

void perip_attach_uart(bus_t *b, uart_t *u) {
    perip_attach_uart_at(b, u, UART0_BASE, "UART0");
}
void perip_attach_gpio(bus_t *b, gpio_t *g) {
    bus_attach(b, GPIO_BASE, GPIO_SIZE, "GPIO", gpio_read, gpio_write, g);
}

/* ---- IO_MUX (one register per pad) ----------------------------------- */

#define IOMUX_BASE 0x60009000u
#define IOMUX_SIZE 0x00001000u

static uint32_t iomux_read(void *ctx, uint32_t off, uint32_t size) {
    (void)size;
    gpio_t *g = (gpio_t *)ctx;
    return g->mux[(off & ~0x3u) >> 2];
}

static void iomux_write(void *ctx, uint32_t off, uint32_t size, uint32_t val) {
    (void)size;
    gpio_t *g = (gpio_t *)ctx;
    g->mux[(off & ~0x3u) >> 2] = val;
}

void perip_attach_iomux(bus_t *b, gpio_t *g) {
    bus_attach(b, IOMUX_BASE, IOMUX_SIZE, "IO_MUX", iomux_read, iomux_write, g);
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
void perip_attach_spi(bus_t *b, c3spi_t *s, uint32_t base) {
    bus_attach(b, base, SPI_SIZE, s->name, spi_read, spi_write, s);
}
void perip_attach_efuse(bus_t *b, c3efuse_t *e) {
    bus_attach(b, EFUSE_BASE, EFUSE_SIZE, "EFUSE", efuse_read, efuse_write, e);
}
void perip_attach_adc(bus_t *b, c3adc_t *a) {
    bus_attach(b, ADC_BASE, ADC_SIZE, "APB_SARADC", adc_read, adc_write, a);
}
