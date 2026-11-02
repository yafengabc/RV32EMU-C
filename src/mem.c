#include "rv32.h"

#include <stdlib.h>
#include <string.h>

static bool in_range(uint32_t addr, uint32_t base, uint32_t size) {
    return addr >= base && addr < base + size;
}

bus_t *bus_new(void) {
    bus_t *b = (bus_t *)calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->flat = (uint8_t *)calloc(FLAT_SIZE, 1);
    b->code_page = (uint8_t *)calloc(FLAT_SIZE >> 12, 1);
    b->code_gen = (uint32_t *)calloc(FLAT_SIZE >> 12, sizeof(uint32_t));
    if (!b->flat || !b->code_page || !b->code_gen) {
        bus_free(b);
        return NULL;
    }
    b->rom = b->flat + (ROM_BASE - FLAT_BASE);
    b->sram = b->flat + (SRAM_BASE - FLAT_BASE);
    return b;
}

void bus_free(bus_t *b) {
    if (!b) return;
    for (int i = 0; i < b->nram; i++) {
        if (b->rams[i].owned) free(b->rams[i].data);
    }
    free(b->flat);
    free(b->code_page);
    free(b->code_gen);
    free(b);
}

void bus_attach(bus_t *b, uint32_t base, uint32_t size, const char *name,
                uint32_t (*rd)(void *, uint32_t, uint32_t),
                void (*wr)(void *, uint32_t, uint32_t, uint32_t), void *ctx) {
    if (b->ndev >= (int)(sizeof(b->devs) / sizeof(b->devs[0]))) return;
    device_t *d = &b->devs[b->ndev++];
    d->base = base;
    d->size = size;
    d->name = name;
    d->read = rd;
    d->write = wr;
    d->ctx = ctx;
}

static device_t *dev_at(bus_t *b, uint32_t addr) {
    for (int i = 0; i < b->ndev; i++) {
        if (in_range(addr, b->devs[i].base, b->devs[i].size)) return &b->devs[i];
    }
    return NULL;
}

void bus_touch_code(bus_t *b, uint32_t addr) {
    uint32_t off = addr - FLAT_BASE;
    if (off < FLAT_SIZE) {
        uint32_t p = off >> 12;
        if (b->code_page[p]) b->code_gen[p]++;
    }
}

static void bus_touch_code_range(bus_t *b, uint32_t base, uint32_t n) {
    if (n == 0) return;
    uint32_t lo = base - FLAT_BASE;
    uint32_t hi = lo + n - 1;
    if (lo >= FLAT_SIZE) return;
    uint32_t last = hi >> 12;
    if (last >= (FLAT_SIZE >> 12)) last = (FLAT_SIZE >> 12) - 1;
    for (uint32_t p = lo >> 12; p <= last; p++) {
        if (b->code_page[p]) b->code_gen[p]++;
    }
}

static uint8_t read_raw(bus_t *b, uint32_t addr) {
    if (in_range(addr, ROM_BASE, ROM_SIZE)) return b->rom[addr - ROM_BASE];
    if (in_range(addr, SRAM_BASE, SRAM_SIZE)) return b->sram[addr - SRAM_BASE];

    for (int i = 0; i < b->nram; i++) {
        ram_region_t *r = &b->rams[i];
        if (in_range(addr, r->base, r->size)) return r->data[addr - r->base];
    }
    for (int i = 0; i < b->ndev; i++) {
        device_t *d = &b->devs[i];
        if (in_range(addr, d->base, d->size)) {
            return (uint8_t)d->read(d->ctx, addr - d->base, 1);
        }
    }

    for (int i = 0; i < b->nunmapped_r; i++) {
        if (b->unmapped_r[i] == addr) return 0;
    }
    if (b->nunmapped_r < 12) {
        b->unmapped_r[b->nunmapped_r++] = addr;
        printf("[bus] unmapped read @0x%08X\n", addr);
    }
    return 0;
}

static void write_raw(bus_t *b, uint32_t addr, uint8_t v) {
    bus_touch_code(b, addr);
    if (in_range(addr, ROM_BASE, ROM_SIZE)) {
        b->rom[addr - ROM_BASE] = v;
        return;
    }
    if (in_range(addr, SRAM_BASE, SRAM_SIZE)) {
        b->sram[addr - SRAM_BASE] = v;
        return;
    }
    for (int i = 0; i < b->nram; i++) {
        ram_region_t *r = &b->rams[i];
        if (in_range(addr, r->base, r->size)) {
            r->data[addr - r->base] = v;
            return;
        }
    }
    for (int i = 0; i < b->ndev; i++) {
        device_t *d = &b->devs[i];
        if (in_range(addr, d->base, d->size)) {
            d->write(d->ctx, addr - d->base, 1, (uint32_t)v);
            return;
        }
    }

    for (int i = 0; i < b->nunmapped_w; i++) {
        if (b->unmapped_w[i] == addr) return;
    }
    if (b->nunmapped_w < 12) {
        b->unmapped_w[b->nunmapped_w++] = addr;
        printf("[bus] unmapped write @0x%08X = 0x%02X\n", addr, v);
    }
}

uint32_t bus_read(bus_t *b, uint32_t addr, uint32_t size) {
    device_t *d = dev_at(b, addr);
    if (d) return d->read(d->ctx, addr - d->base, size);

    switch (size) {
    case 1:
        return (uint32_t)read_raw(b, addr);
    case 2:
        return (uint32_t)read_raw(b, addr) |
               ((uint32_t)read_raw(b, addr + 1) << 8);
    case 4:
        return (uint32_t)read_raw(b, addr) |
               ((uint32_t)read_raw(b, addr + 1) << 8) |
               ((uint32_t)read_raw(b, addr + 2) << 16) |
               ((uint32_t)read_raw(b, addr + 3) << 24);
    default:
        return 0;
    }
}

void bus_write(bus_t *b, uint32_t addr, uint32_t size, uint32_t val) {
    device_t *d = dev_at(b, addr);
    if (d) {
        d->write(d->ctx, addr - d->base, size, val);
        return;
    }
    switch (size) {
    case 1:
        write_raw(b, addr, (uint8_t)val);
        break;
    case 2:
        write_raw(b, addr, (uint8_t)val);
        write_raw(b, addr + 1, (uint8_t)(val >> 8));
        break;
    case 4:
        write_raw(b, addr, (uint8_t)val);
        write_raw(b, addr + 1, (uint8_t)(val >> 8));
        write_raw(b, addr + 2, (uint8_t)(val >> 16));
        write_raw(b, addr + 3, (uint8_t)(val >> 24));
        break;
    default:
        break;
    }
}

void bus_add_ram(bus_t *b, uint32_t base, uint32_t size) {
    if (b->nram >= (int)(sizeof(b->rams) / sizeof(b->rams[0]))) return;
    uint32_t off = base - FLAT_BASE;
    ram_region_t *r = &b->rams[b->nram++];
    r->base = base;
    r->size = size;
    if (off < FLAT_SIZE && size <= FLAT_SIZE - off) {
        r->data = b->flat + off;
        r->owned = false;
    } else {
        r->data = (uint8_t *)calloc(size ? size : 1, 1);
        r->owned = true;
    }
}

void bus_extend_ram(bus_t *b, uint32_t base, uint32_t need) {
    for (int i = 0; i < b->nram; i++) {
        ram_region_t *r = &b->rams[i];
        if (r->base != base) continue;
        if (r->size >= need) return;
        uint8_t *nd = (uint8_t *)calloc(need, 1);
        if (!nd) return;
        memcpy(nd, r->data, r->size);
        if (r->owned) free(r->data);
        r->data = nd;
        r->size = need;
        r->owned = true;
        return;
    }
    bus_add_ram(b, base, need);
}

uint32_t bus_ram_size_at(bus_t *b, uint32_t addr) {
    for (int i = 0; i < b->nram; i++) {
        ram_region_t *r = &b->rams[i];
        if (addr >= r->base && addr < r->base + r->size) return r->size;
    }
    return 0;
}

void bus_copy_into(bus_t *b, uint32_t base, uint32_t off,
                   const uint8_t *data, uint32_t len) {
    for (int i = 0; i < b->nram; i++) {
        ram_region_t *r = &b->rams[i];
        if (r->base != base) continue;
        if (off <= r->size && len <= r->size - off) {
            memcpy(r->data + off, data, len);
            bus_touch_code_range(b, base + off, len);
        }
        return;
    }
}

void bus_copy_out(bus_t *b, uint32_t base, uint8_t *dst, uint32_t len) {
    for (int i = 0; i < b->nram; i++) {
        ram_region_t *r = &b->rams[i];
        if (base >= r->base && base + len <= r->base + r->size) {
            memcpy(dst, r->data + (base - r->base), len);
            return;
        }
    }
    for (uint32_t i = 0; i < len; i++) dst[i] = read_raw(b, base + i);
}

static bool ram_at(bus_t *b, uint32_t addr, uint32_t size,
                   ram_region_t **out, uint32_t *off) {
    for (int i = 0; i < b->nram; i++) {
        ram_region_t *r = &b->rams[i];
        uint32_t d = addr - r->base;
        if (d < r->size && size <= r->size - d) {
            *out = r;
            *off = d;
            return true;
        }
    }
    return false;
}

uint32_t bus_fast_read(bus_t *b, uint32_t addr, uint32_t size) {
    uint32_t off = addr - FLAT_BASE;
    if (off < FLAT_SIZE - size + 1) {
        switch (size) {
        case 1:
            return (uint32_t)b->flat[off];
        case 2:
            return (uint32_t)b->flat[off] | ((uint32_t)b->flat[off + 1] << 8);
        case 4:
            return (uint32_t)b->flat[off] |
                   ((uint32_t)b->flat[off + 1] << 8) |
                   ((uint32_t)b->flat[off + 2] << 16) |
                   ((uint32_t)b->flat[off + 3] << 24);
        default:
            return 0;
        }
    }
    ram_region_t *r;
    uint32_t ro;
    if (ram_at(b, addr, size, &r, &ro)) {
        switch (size) {
        case 1:
            return (uint32_t)r->data[ro];
        case 2:
            return (uint32_t)r->data[ro] |
                   ((uint32_t)r->data[ro + 1] << 8);
        case 4:
            return (uint32_t)r->data[ro] |
                   ((uint32_t)r->data[ro + 1] << 8) |
                   ((uint32_t)r->data[ro + 2] << 16) |
                   ((uint32_t)r->data[ro + 3] << 24);
        default:
            return 0;
        }
    }
    return bus_read(b, addr, size);
}

void bus_fast_write(bus_t *b, uint32_t addr, uint32_t size, uint32_t val) {
    uint32_t off = addr - FLAT_BASE;
    if (off < FLAT_SIZE - size + 1) {
        bus_touch_code(b, addr);
        switch (size) {
        case 1:
            b->flat[off] = (uint8_t)val;
            break;
        case 2:
            b->flat[off] = (uint8_t)val;
            b->flat[off + 1] = (uint8_t)(val >> 8);
            break;
        case 4:
            b->flat[off] = (uint8_t)val;
            b->flat[off + 1] = (uint8_t)(val >> 8);
            b->flat[off + 2] = (uint8_t)(val >> 16);
            b->flat[off + 3] = (uint8_t)(val >> 24);
            break;
        default:
            break;
        }
        return;
    }
    ram_region_t *r;
    uint32_t ro;
    if (ram_at(b, addr, size, &r, &ro)) {
        switch (size) {
        case 1:
            r->data[ro] = (uint8_t)val;
            break;
        case 2:
            r->data[ro] = (uint8_t)val;
            r->data[ro + 1] = (uint8_t)(val >> 8);
            break;
        case 4:
            r->data[ro] = (uint8_t)val;
            r->data[ro + 1] = (uint8_t)(val >> 8);
            r->data[ro + 2] = (uint8_t)(val >> 16);
            r->data[ro + 3] = (uint8_t)(val >> 24);
            break;
        default:
            break;
        }
        return;
    }
    bus_write(b, addr, size, val);
}

void bus_load_rom(bus_t *b, uint32_t off, const uint8_t *data, uint32_t len) {
    if (off >= ROM_SIZE) return;
    if (len > ROM_SIZE - off) len = ROM_SIZE - off;
    memcpy(b->rom + off, data, len);
    bus_touch_code_range(b, ROM_BASE + off, len);
}
