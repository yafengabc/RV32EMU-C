/*
 * ESP32-C3 flash cache (XIP) model.
 *
 * The real chip has two levels: the EXTMEM control registers at 0x600C4000
 * and a 128-entry MMU table at 0x600C5000.  Each entry maps one 64 KiB flash
 * page into 0x3C000000 (DROM) or 0x42000000 (IROM).
 *
 * This model follows the Go emulator:
 *   - the MMU table is plain memory; software writes it and the translation
 *     follows;
 *   - reset maps linearly (window offset == flash offset) so bare-metal
 *     firmware can fetch before any bootloader runs;
 *   - bytes written through a window (ELF loader) are kept in a per-page
 *     overlay which reads prefer, so IROM/DROM segments need no flash write;
 *   - the EXTMEM control registers are shadow storage only.
 */

#include <stdlib.h>
#include <string.h>

#include "rv32.h"

#define CACHE_PAGE_SHIFT 16
#define CACHE_PAGE_SIZE  (1u << CACHE_PAGE_SHIFT)
#define MMU_ENTRY_NUM    128
#define MMU_INVALID_BIT  (1u << 8)
#define MMU_TABLE_OFF    (0x600C5000u - CACHE_REG_BASE)

struct flashmmu {
    bus_t    *bus;
    uint32_t  flash_base;
    uint32_t  flash_size;
    uint32_t  entries[MMU_ENTRY_NUM];
    uint8_t  *overlay[MMU_ENTRY_NUM * 2];  /* DROM pages then IROM pages */
};

static int overlay_slot(uint32_t vaddr) {
    uint32_t page = vaddr >> CACHE_PAGE_SHIFT;
    uint32_t drom = DROM_BASE >> CACHE_PAGE_SHIFT;
    uint32_t irom = IROM_BASE >> CACHE_PAGE_SHIFT;
    if (page >= drom && page < drom + MMU_ENTRY_NUM) {
        return (int)(page - drom);
    }
    if (page >= irom && page < irom + MMU_ENTRY_NUM) {
        return MMU_ENTRY_NUM + (int)(page - irom);
    }
    return -1;
}

flashmmu_t *flashmmu_new(bus_t *bus, uint32_t flash_base, uint32_t flash_size) {
    flashmmu_t *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->bus = bus;
    m->flash_base = flash_base;
    m->flash_size = flash_size;
    /* Reset default: linear mapping; pages beyond flash are invalid. */
    for (int i = 0; i < MMU_ENTRY_NUM; i++) {
        m->entries[i] = ((uint32_t)i << CACHE_PAGE_SHIFT) < flash_size
                            ? (uint32_t)i
                            : MMU_INVALID_BIT;
    }
    return m;
}

void flashmmu_free(flashmmu_t *m) {
    if (!m) return;
    for (int i = 0; i < MMU_ENTRY_NUM * 2; i++) free(m->overlay[i]);
    free(m);
}

static bool mmu_phys(flashmmu_t *m, uint32_t vaddr, uint32_t *phys) {
    uint32_t e = m->entries[(vaddr >> CACHE_PAGE_SHIFT) & (MMU_ENTRY_NUM - 1)];
    if (e & MMU_INVALID_BIT) return false;
    uint32_t page = e & 0xFFu;
    if ((page << CACHE_PAGE_SHIFT) >= m->flash_size) return false;
    *phys = (page << CACHE_PAGE_SHIFT) | (vaddr & (CACHE_PAGE_SIZE - 1));
    return true;
}

static uint8_t mmu_read_byte(flashmmu_t *m, uint32_t addr) {
    int slot = overlay_slot(addr);
    if (slot >= 0 && m->overlay[slot]) {
        return m->overlay[slot][addr & (CACHE_PAGE_SIZE - 1)];
    }
    uint32_t phys;
    if (!mmu_phys(m, addr, &phys)) return 0xFF;
    return (uint8_t)bus_read(m->bus, m->flash_base + phys, 1);
}

uint32_t flashmmu_read(flashmmu_t *m, uint32_t addr, uint32_t size) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < size; i++) {
        v |= (uint32_t)mmu_read_byte(m, addr + i) << (8 * i);
    }
    return v;
}

void flashmmu_write(flashmmu_t *m, uint32_t addr, uint32_t size, uint32_t val) {
    int slot = overlay_slot(addr);
    if (slot < 0) return;  /* outside both XIP windows */
    if (!m->overlay[slot]) {
        m->overlay[slot] = calloc(1, CACHE_PAGE_SIZE);
        if (!m->overlay[slot]) return;
    }
    for (uint32_t i = 0; i < size; i++) {
        m->overlay[slot][(addr + i) & (CACHE_PAGE_SIZE - 1)] =
            (uint8_t)(val >> (8 * i));
    }
}

void flashmmu_map(flashmmu_t *m, uint32_t vaddr, uint32_t flash_off,
                  uint32_t size) {
    uint32_t first = (vaddr >> CACHE_PAGE_SHIFT) & (MMU_ENTRY_NUM - 1);
    uint32_t fpage = (flash_off >> CACHE_PAGE_SHIFT) & 0xFFu;
    uint32_t pages = (size + CACHE_PAGE_SIZE - 1) >> CACHE_PAGE_SHIFT;
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t idx = (first + i) & (MMU_ENTRY_NUM - 1);
        m->entries[idx] = (fpage + i) & 0xFFu;
        int slot = overlay_slot((vaddr + i * CACHE_PAGE_SIZE) &
                                ~(CACHE_PAGE_SIZE - 1));
        if (slot >= 0) {
            free(m->overlay[slot]);
            m->overlay[slot] = NULL;
        }
    }
}

/* ---- XIP window device ---------------------------------------------- */

c3xip_t *c3xip_new(flashmmu_t *m, uint32_t base, const char *name) {
    c3xip_t *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->mmu = m;
    w->base = base;
    w->name = name;
    return w;
}

void c3xip_free(c3xip_t *w) { free(w); }

static uint32_t xip_read(void *ctx, uint32_t off, uint32_t size) {
    c3xip_t *w = ctx;
    return flashmmu_read(w->mmu, w->base + off, size);
}

static void xip_write(void *ctx, uint32_t off, uint32_t size, uint32_t val) {
    c3xip_t *w = ctx;
    flashmmu_write(w->mmu, w->base + off, size, val);
}

void perip_attach_xip(bus_t *b, c3xip_t *w) {
    bus_attach(b, w->base, XIP_WINDOW_SIZE, w->name, xip_read, xip_write, w);
}

/* ---- EXTMEM control registers --------------------------------------- */

static bool cache_trigger(uint32_t off) {
    switch (off) {
    case 0x01C: case 0x028: case 0x034:
    case 0x064: case 0x07C: case 0x088:
        return true;
    }
    return false;
}

c3cache_t *c3cache_new(flashmmu_t *m) {
    c3cache_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->mmu = m;
    c->regs[0x004 / 4] = 0x3;  /* ICACHE_CTRL1 */
    c->regs[0x054 / 4] = IROM_BASE;
    c->regs[0x058 / 4] = IROM_BASE + XIP_WINDOW_SIZE - 1;
    c->regs[0x05C / 4] = DROM_BASE;
    c->regs[0x060 / 4] = DROM_BASE + XIP_WINDOW_SIZE - 1;
    return c;
}

void c3cache_free(c3cache_t *c) { free(c); }

static uint32_t cache_read(void *ctx, uint32_t off, uint32_t size) {
    c3cache_t *c = ctx;
    (void)size;
    if (off >= MMU_TABLE_OFF) {
        uint32_t idx = (off - MMU_TABLE_OFF) / 4;
        if (idx < MMU_ENTRY_NUM) return c->mmu->entries[idx];
        return 0;
    }
    if (cache_trigger(off)) return 0;
    return c->regs[off / 4];
}

static void cache_write(void *ctx, uint32_t off, uint32_t size, uint32_t val) {
    c3cache_t *c = ctx;
    (void)size;
    if (off >= MMU_TABLE_OFF) {
        uint32_t idx = (off - MMU_TABLE_OFF) / 4;
        if (idx < MMU_ENTRY_NUM) c->mmu->entries[idx] = val;
        return;
    }
    if (cache_trigger(off)) return;
    c->regs[off / 4] = val;
}

void perip_attach_cache(bus_t *b, c3cache_t *c) {
    bus_attach(b, CACHE_REG_BASE, CACHE_REG_SIZE, "C3CACHE", cache_read,
               cache_write, c);
}
