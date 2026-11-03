/*
 * Load an ESP-IDF application image from flash, the way a second-stage
 * bootloader would: locate the factory app partition, then either map the
 * XIP (DROM/IROM) segments through the flash MMU or copy RAM segments into
 * place.  Used by -flashimg.
 */

#include <stdio.h>
#include <stdlib.h>

#include "rv32.h"

#define PART_TABLE_BASE 0x8000u
#define PART_TABLE_MAX  0x1000u
#define PART_ENTRY_SIZE 32u
#define PART_TYPE_APP   0u
#define PART_SUB_FACTORY 0u
#define IMAGE_HEADER_SIZE 24u

static bool part_magic(const uint8_t *f, uint32_t off) {
    return f[off] == 0xAA && f[off + 1] == 0x50;
}

static bool find_app_partition(const uint8_t *flash, uint32_t flash_size,
                               uint32_t *out) {
    for (uint32_t off = PART_TABLE_BASE;
         off + PART_ENTRY_SIZE <= PART_TABLE_BASE + PART_TABLE_MAX;
         off += PART_ENTRY_SIZE) {
        if (off + PART_ENTRY_SIZE > flash_size) break;
        if (!part_magic(flash, off)) continue;
        uint8_t typ = flash[off + 2];
        uint8_t sub = flash[off + 3];
        if (typ != PART_TYPE_APP) continue;
        if (sub == PART_SUB_FACTORY) {
            *out = (uint32_t)flash[off + 4] | ((uint32_t)flash[off + 5] << 8) |
                   ((uint32_t)flash[off + 6] << 16) |
                   ((uint32_t)flash[off + 7] << 24);
            return true;
        }
    }
    /* No factory partition: accept any app partition. */
    for (uint32_t off = PART_TABLE_BASE;
         off + PART_ENTRY_SIZE <= PART_TABLE_BASE + PART_TABLE_MAX;
         off += PART_ENTRY_SIZE) {
        if (off + PART_ENTRY_SIZE > flash_size) break;
        if (part_magic(flash, off) && flash[off + 2] == PART_TYPE_APP) {
            *out = (uint32_t)flash[off + 4] | ((uint32_t)flash[off + 5] << 8) |
                   ((uint32_t)flash[off + 6] << 16) |
                   ((uint32_t)flash[off + 7] << 24);
            return true;
        }
    }
    return false;
}

static bool is_xip(uint32_t addr) {
    return (addr >= DROM_BASE && addr < DROM_BASE + XIP_WINDOW_SIZE) ||
           (addr >= IROM_BASE && addr < IROM_BASE + XIP_WINDOW_SIZE);
}

static bool has_device_at(bus_t *bus, uint32_t addr) {
    for (int i = 0; i < bus->ndev; i++) {
        device_t *d = &bus->devs[i];
        if (addr >= d->base && addr < d->base + d->size) return true;
    }
    return false;
}

static void ensure_ram(bus_t *bus, uint32_t addr, uint32_t size) {
    if (addr >= SRAM_BASE && addr < SRAM_BASE + SRAM_SIZE) return;
    if (bus_ram_size_at(bus, addr) > 0) return;
    if (has_device_at(bus, addr)) return;
    if (size < (1u << 20)) size = 1u << 20;
    bus_add_ram(bus, addr, size);
}

bool image_load_app(const uint8_t *flash, uint32_t flash_size, bus_t *bus,
                    flashmmu_t *mmu, uint32_t *entry) {
    uint32_t app_off;
    if (!find_app_partition(flash, flash_size, &app_off)) {
        printf("[image] no app partition found (partition table @0x%X)\n",
               PART_TABLE_BASE);
        return false;
    }
    if (app_off + IMAGE_HEADER_SIZE > flash_size) {
        printf("[image] app partition @0x%X exceeds flash size\n", app_off);
        return false;
    }
    if (flash[app_off] != 0xE9) {
        printf("[image] app @0x%X has no ESP image header (magic=0x%02X)\n",
               app_off, flash[app_off]);
        return false;
    }
    int seg_count = flash[app_off + 1];
    *entry = (uint32_t)flash[app_off + 4] |
             ((uint32_t)flash[app_off + 5] << 8) |
             ((uint32_t)flash[app_off + 6] << 16) |
             ((uint32_t)flash[app_off + 7] << 24);
    printf("[image] app @0x%08X segments=%d entry=0x%08X\n", app_off, seg_count,
           *entry);

    uint32_t p = app_off + IMAGE_HEADER_SIZE;
    for (int i = 0; i < seg_count; i++) {
        if (p + 8 > flash_size) {
            printf("[image] segment %d header truncated\n", i);
            return false;
        }
        uint32_t load_addr = (uint32_t)flash[p] |
                             ((uint32_t)flash[p + 1] << 8) |
                             ((uint32_t)flash[p + 2] << 16) |
                             ((uint32_t)flash[p + 3] << 24);
        uint32_t len = (uint32_t)flash[p + 4] |
                       ((uint32_t)flash[p + 5] << 8) |
                       ((uint32_t)flash[p + 6] << 16) |
                       ((uint32_t)flash[p + 7] << 24);
        uint32_t data_off = p + 8;
        if (data_off + len > flash_size) {
            printf("[image] segment %d data truncated\n", i);
            return false;
        }
        printf("[image] seg%d load=0x%08X len=0x%X flash=0x%08X\n", i, load_addr,
               len, data_off);
        if (is_xip(load_addr)) {
            flashmmu_map(mmu, load_addr, data_off, len);
        } else {
            ensure_ram(bus, load_addr, len);
            for (uint32_t k = 0; k < len; k++) {
                bus_write(bus, load_addr + k, 1, flash[data_off + k]);
            }
        }
        p = data_off + len;
    }
    return true;
}
