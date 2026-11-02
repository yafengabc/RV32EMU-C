#include "rv32.h"

#include <stdlib.h>
#include <string.h>

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint8_t *read_file(const char *path, uint32_t *out_len) {
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
    *out_len = (uint32_t)n;
    return buf;
}

static bool in_builtin(uint32_t addr) {
    return (addr >= 0x40000000u && addr < 0x40080000u) ||
           (addr >= 0x3FC80000u && addr < 0x3FC80000u + 0x80000u);
}

typedef struct {
    uint32_t base;
    uint32_t end;
    uint32_t offset;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t vaddr;
} seg_t;

uint32_t elf_load(const char *path, bus_t *bus) {
    uint32_t len = 0;
    uint8_t *data = read_file(path, &len);
    if (!data) {
        fprintf(stderr, "[elf] cannot read %s\n", path);
        return 0;
    }
    if (len < 52) {
        fprintf(stderr, "[elf] file too short\n");
        free(data);
        return 0;
    }
    if (data[0] != 0x7F || data[1] != 'E' || data[2] != 'L' || data[3] != 'F') {
        fprintf(stderr, "[elf] bad magic\n");
        free(data);
        return 0;
    }
    if (data[4] != 1) {
        fprintf(stderr, "[elf] not ELF32\n");
        free(data);
        return 0;
    }
    uint16_t machine = rd16(data + 18);
    if (machine != 243) {
        fprintf(stderr, "[elf] not RISC-V ELF (machine=%u)\n", machine);
        free(data);
        return 0;
    }

    uint32_t entry = rd32(data + 24);
    uint32_t phoff = rd32(data + 28);
    uint16_t phentsize = rd16(data + 42);
    uint16_t phnum = rd16(data + 44);
    if (phentsize < 32) {
        fprintf(stderr, "[elf] bad phentsize\n");
        free(data);
        return 0;
    }

    seg_t segs[128];
    int nseg = 0;
    for (uint16_t i = 0; i < phnum && nseg < 128; i++) {
        uint64_t off64 = (uint64_t)phoff + (uint64_t)i * phentsize;
        if (off64 + 32 > len) break;
        const uint8_t *p = data + off64;
        if (rd32(p) != 1) continue; /* PT_LOAD */
        seg_t s;
        s.offset = rd32(p + 4);
        s.vaddr = rd32(p + 8);
        s.base = rd32(p + 12);
        s.filesz = rd32(p + 16);
        s.memsz = rd32(p + 20);
        s.end = s.base + s.memsz;
        segs[nseg++] = s;
    }

    for (int i = 0; i < nseg; i++) {
        seg_t *s = &segs[i];
        if (in_builtin(s->base)) continue;
        if (bus_ram_size_at(bus, s->base) > 0) continue;
        uint32_t lo = s->base, hi = s->end;
        for (int j = 0; j < nseg; j++) {
            seg_t *s2 = &segs[j];
            if (s2->base >= lo && s2->base <= hi && !in_builtin(s2->base)) {
                if (s2->end > hi) hi = s2->end;
            }
        }
        uint32_t size = (hi - lo + 0xFFFu) & ~0xFFFu;
        if (size < 1024u * 1024u) size = 1024u * 1024u;
        bus_add_ram(bus, lo, size);
    }

    for (int i = 0; i < nseg; i++) {
        seg_t *s = &segs[i];
        if (s->offset > len) continue;
        uint32_t n = s->filesz;
        if (n > len - s->offset) n = len - s->offset;
        for (uint32_t j = 0; j < n; j++) {
            bus_write(bus, s->base + j, 1, data[s->offset + j]);
        }
        if (!g_quiet)
            printf("[elf] load vaddr=0x%08X paddr=0x%08X filesz=0x%x "
                   "memsz=0x%x\n",
                   s->vaddr, s->base, s->filesz, s->memsz);
    }

    if (!g_quiet) printf("[elf] entry=0x%08X\n", entry);
    free(data);
    return entry;
}

int elf_load_bin(const char *path, uint32_t base, bus_t *bus) {
    uint32_t len = 0;
    uint8_t *data = read_file(path, &len);
    if (!data) return -1;
    for (uint32_t i = 0; i < len; i++) {
        bus_write(bus, base + i, 1, data[i]);
    }
    if (!g_quiet) printf("[bin] load base=0x%08X size=0x%x\n", base, len);
    free(data);
    return 0;
}
