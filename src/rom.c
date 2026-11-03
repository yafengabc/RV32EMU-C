#include "rv32.h"

#include <string.h>

#define FLASH_BASE 0x3F000000u

#define ENTRY_WRITE ROM_ENTRY_WRITE
#define ENTRY_READ  ROM_ENTRY_READ
#define ENTRY_CONF  ROM_ENTRY_CONF
/* Do not use 0x400/0x440: 0x40000400 onwards are real ROM libc entries
 * (strndup/atoi), and this stub would overwrite them. */
#define IMPL_READ   ROM_IMPL_READ
#define IMPL_WRITE  ROM_IMPL_WRITE

static uint32_t enc_lui(uint32_t rd, uint32_t imm20) {
    return (imm20 << 12) | (rd << 7) | 0x37u;
}
static uint32_t enc_addi(uint32_t rd, uint32_t rs1, uint32_t imm) {
    return ((imm & 0xFFFu) << 20) | (rs1 << 15) | (rd << 7) | 0x13u;
}
static uint32_t enc_add(uint32_t rd, uint32_t rs1, uint32_t rs2) {
    return (rs2 << 20) | (rs1 << 15) | (rd << 7) | 0x33u;
}
static uint32_t enc_lb(uint32_t rd, uint32_t rs1, uint32_t imm) {
    return ((imm & 0xFFFu) << 20) | (rs1 << 15) | (rd << 7) | 0x03u;
}
static uint32_t enc_sb(uint32_t rs2, uint32_t rs1, uint32_t imm) {
    imm &= 0xFFFu;
    uint32_t hi = (imm >> 5) & 0x7Fu;
    uint32_t lo = imm & 0x1Fu;
    return (hi << 25) | (rs2 << 20) | (rs1 << 15) | (lo << 7) | 0x23u;
}
static uint32_t enc_beq(uint32_t rs1, uint32_t rs2, int32_t imm) {
    uint32_t u = (uint32_t)imm;
    uint32_t i12 = (u >> 12) & 1u;
    uint32_t i10_5 = (u >> 5) & 0x3Fu;
    uint32_t i4_1 = (u >> 1) & 0xFu;
    uint32_t i11 = (u >> 11) & 1u;
    uint32_t field = (i12 << 31) | (i10_5 << 25) | (i4_1 << 8) | (i11 << 7);
    return field | (rs2 << 20) | (rs1 << 15) | 0x63u;
}
static uint32_t enc_jal(uint32_t rd, int32_t imm) {
    uint32_t u = (uint32_t)imm;
    uint32_t i20 = (u >> 20) & 1u;
    uint32_t i10_1 = (u >> 1) & 0x3FFu;
    uint32_t i11 = (u >> 11) & 1u;
    uint32_t i19_12 = (u >> 12) & 0xFFu;
    uint32_t field = (i20 << 31) | (i10_1 << 21) | (i11 << 20) | (i19_12 << 12);
    return field | (rd << 7) | 0x6Fu;
}
static uint32_t enc_jalr(uint32_t rd, uint32_t rs1, uint32_t imm) {
    return ((imm & 0xFFFu) << 20) | (rs1 << 15) | (rd << 7) | 0x67u;
}

static void put_w(uint8_t *rom, uint32_t off, uint32_t w) {
    rom[off] = (uint8_t)w;
    rom[off + 1] = (uint8_t)(w >> 8);
    rom[off + 2] = (uint8_t)(w >> 16);
    rom[off + 3] = (uint8_t)(w >> 24);
}

#define X_A0 10u
#define X_A1 11u
#define X_A2 12u
#define X_T0 5u
#define X_T1 6u
#define X_T2 7u
#define X_T3 28u
#define X_RA 1u
#define X_ZERO 0u

void rom_build(uint8_t *rom) {
    memset(rom, 0, ROM_SIZE);

    put_w(rom, ENTRY_WRITE, enc_jal(X_ZERO, (int32_t)(IMPL_WRITE - ENTRY_WRITE)));
    put_w(rom, ENTRY_READ, enc_jal(X_ZERO, (int32_t)(IMPL_READ - ENTRY_READ)));
    put_w(rom, ENTRY_CONF, enc_addi(X_A0, X_ZERO, 0));
    put_w(rom, ENTRY_CONF + 4, enc_jalr(X_ZERO, X_RA, 0));

    uint32_t hi = FLASH_BASE >> 12;
    uint32_t off = IMPL_READ;
    put_w(rom, off, enc_lui(X_T0, hi)); off += 4;
    put_w(rom, off, enc_add(X_T0, X_T0, X_A0)); off += 4;
    put_w(rom, off, enc_addi(X_T1, X_A1, 0)); off += 4;
    put_w(rom, off, enc_addi(X_T2, X_A2, 0)); off += 4;
    uint32_t loop_r = off;
    put_w(rom, off, enc_beq(X_T2, X_ZERO,
                            (int32_t)(IMPL_READ + 0x2C - loop_r))); off += 4;
    put_w(rom, off, enc_lb(X_T3, X_T0, 0)); off += 4;
    put_w(rom, off, enc_sb(X_T3, X_T1, 0)); off += 4;
    put_w(rom, off, enc_addi(X_T0, X_T0, 1)); off += 4;
    put_w(rom, off, enc_addi(X_T1, X_T1, 1)); off += 4;
    put_w(rom, off, enc_addi(X_T2, X_T2, 0xFFFFFFFFu)); off += 4;
    put_w(rom, off, enc_jal(X_ZERO, (int32_t)(loop_r - off))); off += 4;
    put_w(rom, off, enc_addi(X_A0, X_ZERO, 0)); off += 4;
    put_w(rom, off, enc_jalr(X_ZERO, X_RA, 0)); off += 4;

    off = IMPL_WRITE;
    put_w(rom, off, enc_lui(X_T0, hi)); off += 4;
    put_w(rom, off, enc_add(X_T0, X_T0, X_A0)); off += 4;
    put_w(rom, off, enc_addi(X_T1, X_A1, 0)); off += 4;
    put_w(rom, off, enc_addi(X_T2, X_A2, 0)); off += 4;
    uint32_t loop_w = off;
    put_w(rom, off, enc_beq(X_T2, X_ZERO,
                            (int32_t)(IMPL_WRITE + 0x2C - loop_w))); off += 4;
    put_w(rom, off, enc_lb(X_T3, X_T1, 0)); off += 4;
    put_w(rom, off, enc_sb(X_T3, X_T0, 0)); off += 4;
    put_w(rom, off, enc_addi(X_T0, X_T0, 1)); off += 4;
    put_w(rom, off, enc_addi(X_T1, X_T1, 1)); off += 4;
    put_w(rom, off, enc_addi(X_T2, X_T2, 0xFFFFFFFFu)); off += 4;
    put_w(rom, off, enc_jal(X_ZERO, (int32_t)(loop_w - off))); off += 4;
    put_w(rom, off, enc_addi(X_A0, X_ZERO, 0)); off += 4;
    put_w(rom, off, enc_jalr(X_ZERO, X_RA, 0)); off += 4;
}
