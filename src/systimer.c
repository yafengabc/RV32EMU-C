/*
 * ESP32-C3 SYSTIMER: two 52-bit up counters and three comparators.
 *
 * ESP-IDF's esp_timer and the FreeRTOS tick are built on TARGET0, and
 * MicroPython uses the same path.  TARGET0..2 raise INTMTX sources 37..39.
 */

#include <stdlib.h>

#include "rv32.h"

#define ST_CONF        0x00u
#define ST_UNIT0_OP    0x04u
#define ST_UNIT1_OP    0x08u
#define ST_UNIT0_LOAD_HI 0x0Cu
#define ST_UNIT0_LOAD_LO 0x10u
#define ST_UNIT1_LOAD_HI 0x14u
#define ST_UNIT1_LOAD_LO 0x18u
#define ST_TARGET0_HI  0x1Cu
#define ST_TARGET0_LO  0x20u
#define ST_TARGET1_HI  0x24u
#define ST_TARGET1_LO  0x28u
#define ST_TARGET2_HI  0x2Cu
#define ST_TARGET2_LO  0x30u
#define ST_TARGET0_CONF 0x34u
#define ST_TARGET1_CONF 0x38u
#define ST_TARGET2_CONF 0x3Cu
#define ST_UNIT0_VAL_HI 0x40u
#define ST_UNIT0_VAL_LO 0x44u
#define ST_UNIT1_VAL_HI 0x48u
#define ST_UNIT1_VAL_LO 0x4Cu
#define ST_COMP0_LOAD  0x50u
#define ST_COMP1_LOAD  0x54u
#define ST_COMP2_LOAD  0x58u
#define ST_UNIT0_LOAD  0x5Cu
#define ST_UNIT1_LOAD  0x60u
#define ST_INT_ENA     0x64u
#define ST_INT_RAW     0x68u
#define ST_INT_CLR     0x6Cu
#define ST_INT_ST      0x70u
#define ST_DATE        0xFCu
#define ST_DATE_RESET  33579377u

#define ST_CLK_EN       (1u << 31)
#define ST_UNIT0_WORK   (1u << 30)
#define ST_UNIT1_WORK   (1u << 29)
#define ST_TARGET0_EN   (1u << 24)
#define ST_TARGET1_EN   (1u << 25)
#define ST_TARGET2_EN   (1u << 26)
#define ST_OP_UPDATE    (1u << 30)
#define ST_OP_VALID     (1u << 29)
#define ST_TGT_PERIOD_M (1u << 30)
#define ST_TGT_UNIT_SEL (1u << 31)
#define ST_UNIT_MASK    0x000FFFFFFFFFFFFFull
#define ST_TGT_HI_MASK  0x000FFFFFu
#define ST_TGT_PERIOD_MASK 0x03FFFFFFu

#define ST_SRC0 37u

static uint32_t target_en(int t) {
    return (uint32_t[]){ST_TARGET0_EN, ST_TARGET1_EN, ST_TARGET2_EN}[t];
}

static uint32_t unit_en(int u) {
    return u == 0 ? ST_UNIT0_WORK : ST_UNIT1_WORK;
}

static void st_sync_irq(c3systimer_t *s) {
    if (!s->intc) return;
    for (int t = 0; t < 3; t++) {
        uint32_t src = ST_SRC0 + (uint32_t)t;
        if ((s->int_raw & (1u << t)) && (s->int_ena & (1u << t))) {
            c3intc_raise(s->intc, src);
        } else {
            c3intc_clear_source(s->intc, src);
        }
    }
    c3intc_deliver(s->intc);
}

c3systimer_t *c3systimer_new(void) {
    c3systimer_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->conf = ST_UNIT0_WORK;
    s->regs[ST_DATE / 4] = ST_DATE_RESET;
    return s;
}

void c3systimer_free(c3systimer_t *s) { free(s); }

void c3systimer_set_intc(c3systimer_t *s, c3intc_t *i) { s->intc = i; }

void c3systimer_tick_n(c3systimer_t *s, uint64_t n) {
    if (n == 0) return;
    if (s->conf & ST_UNIT0_WORK) s->units[0] = (s->units[0] + n) & ST_UNIT_MASK;
    if (s->conf & ST_UNIT1_WORK) s->units[1] = (s->units[1] + n) & ST_UNIT_MASK;

    for (int t = 0; t < 3; t++) {
        if ((s->conf & target_en(t)) == 0) continue;
        int unit = (s->target_cfg[t] & ST_TGT_UNIT_SEL) ? 1 : 0;
        if ((s->conf & unit_en(unit)) == 0) continue;
        uint64_t cur = s->units[unit];
        if (cur < s->comps[t]) continue;
        if ((s->target_cfg[t] & ST_TGT_PERIOD_M) == 0) {
            if (s->fired[t]) continue;
            s->fired[t] = true;
        }
        s->fires[t]++;
        s->int_raw |= 1u << t;
        if (s->target_cfg[t] & ST_TGT_PERIOD_M) {
            uint64_t period = s->target_cfg[t] & ST_TGT_PERIOD_MASK;
            if (period == 0) {
                s->comps[t] = cur + 1;
            } else {
                while (s->comps[t] <= cur) s->comps[t] += period;
            }
        }
    }
    st_sync_irq(s);
}

static uint32_t st_read(void *ctx, uint32_t off, uint32_t size) {
    c3systimer_t *s = ctx;
    (void)size;
    switch (off) {
    case ST_CONF:
        return s->conf;
    case ST_UNIT0_OP:
    case ST_UNIT1_OP:
        return s->regs[off / 4];
    case ST_UNIT0_VAL_HI:
    case ST_UNIT0_VAL_LO:
    case ST_UNIT1_VAL_HI:
    case ST_UNIT1_VAL_LO: {
        int i = (off == ST_UNIT1_VAL_HI || off == ST_UNIT1_VAL_LO) ? 1 : 0;
        if (off == ST_UNIT0_VAL_HI || off == ST_UNIT1_VAL_HI) {
            return (uint32_t)(s->units[i] >> 32) & ST_TGT_HI_MASK;
        }
        return (uint32_t)s->units[i];
    }
    case ST_INT_ENA:
        return s->int_ena;
    case ST_INT_RAW:
        return s->int_raw;
    case ST_INT_ST:
        return s->int_raw & s->int_ena;
    case ST_INT_CLR:
        return 0;
    }
    return s->regs[off / 4];
}

static void st_write(void *ctx, uint32_t off, uint32_t size, uint32_t val) {
    c3systimer_t *s = ctx;
    (void)size;
    switch (off) {
    case ST_CONF:
        s->conf = val & (ST_CLK_EN | ST_UNIT0_WORK | ST_UNIT1_WORK |
                         ST_TARGET0_EN | ST_TARGET1_EN | ST_TARGET2_EN);
        break;
    case ST_UNIT0_OP:
    case ST_UNIT1_OP:
        s->regs[off / 4] = val & ~ST_OP_UPDATE;
        if (val & ST_OP_UPDATE) s->regs[off / 4] |= ST_OP_VALID;
        break;
    case ST_UNIT0_LOAD_HI:
    case ST_UNIT0_LOAD_LO:
    case ST_UNIT1_LOAD_HI:
    case ST_UNIT1_LOAD_LO: {
        s->regs[off / 4] = val;
        int i = (off == ST_UNIT1_LOAD_HI || off == ST_UNIT1_LOAD_LO) ? 1 : 0;
        if (off == ST_UNIT0_LOAD_HI || off == ST_UNIT1_LOAD_HI) {
            s->loads[i][0] = val & ST_TGT_HI_MASK;
        } else {
            s->loads[i][1] = val;
        }
        break;
    }
    case ST_TARGET0_HI:
    case ST_TARGET0_LO:
    case ST_TARGET1_HI:
    case ST_TARGET1_LO:
    case ST_TARGET2_HI:
    case ST_TARGET2_LO: {
        s->regs[off / 4] = val;
        int t = (int)((off - ST_TARGET0_HI) / 8);
        if (((off - ST_TARGET0_HI) % 8) == 0) {
            s->targets[t] = ((uint64_t)(val & ST_TGT_HI_MASK) << 32) |
                            (s->targets[t] & 0xFFFFFFFFull);
        } else {
            s->targets[t] = (s->targets[t] & ~0xFFFFFFFFull) | val;
        }
        break;
    }
    case ST_TARGET0_CONF:
    case ST_TARGET1_CONF:
    case ST_TARGET2_CONF:
        s->regs[off / 4] = val;
        s->target_cfg[(off - ST_TARGET0_CONF) / 4] = val;
        break;
    case ST_COMP0_LOAD:
    case ST_COMP1_LOAD:
    case ST_COMP2_LOAD: {
        int t = (int)((off - ST_COMP0_LOAD) / 4);
        s->comps[t] = s->targets[t];
        s->fired[t] = false;
        break;
    }
    case ST_UNIT0_LOAD:
    case ST_UNIT1_LOAD: {
        int i = off == ST_UNIT1_LOAD ? 1 : 0;
        if (val & 1u) {
            s->units[i] = (((uint64_t)s->loads[i][1]) |
                           ((uint64_t)s->loads[i][0] << 32)) & ST_UNIT_MASK;
        }
        break;
    }
    case ST_INT_ENA:
        s->int_ena = val & 0x7u;
        s->regs[off / 4] = s->int_ena;
        st_sync_irq(s);
        break;
    case ST_INT_CLR:
        s->int_raw &= ~(val & 0x7u);
        st_sync_irq(s);
        break;
    case ST_INT_RAW:
        s->int_raw &= ~(val & 0x7u);
        st_sync_irq(s);
        break;
    case ST_DATE:
        s->regs[off / 4] = val;
        break;
    }
}

void perip_attach_systimer(bus_t *b, c3systimer_t *s) {
    bus_attach(b, 0x60023000u, 0x1000u, "C3SYSTIMER", st_read, st_write, s);
}
