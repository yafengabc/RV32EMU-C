#include "rv32.h"

#include <stdio.h>

static const char *const abi[32] = {
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
    "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
    "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
    "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
};

static const char *rn(int i) { return abi[i & 31]; }

static uint32_t sign_ext(uint32_t x, unsigned n) {
    if (x & (1u << (n - 1))) return x | (~0u << n);
    return x;
}
static uint32_t imm_j_d(uint32_t i) {
    return sign_ext(((i >> 31) & 1u) << 20 | ((i >> 12) & 0xFFu) << 12 |
                        ((i >> 20) & 1u) << 11 | ((i >> 21) & 0x3FFu) << 1,
                    21);
}
static uint32_t imm_s_d(uint32_t i) {
    return sign_ext(((i >> 25) << 5) | ((i >> 7) & 0x1Fu), 12);
}

void disasm_c(uint16_t in, char *buf, size_t cap) {
    snprintf(buf, cap, ".c 0x%04X", in);
}

void disasm_i(uint32_t pc, uint32_t instr, char *buf, size_t cap) {
    uint32_t op = instr & 0x7Fu;
    int rd = (int)((instr >> 7) & 0x1Fu);
    uint32_t f3 = (instr >> 12) & 0x7u;
    int rs1 = (int)((instr >> 15) & 0x1Fu);
    int rs2 = (int)((instr >> 20) & 0x1Fu);
    uint32_t imm_i = sign_ext(instr >> 20, 12);
    uint32_t f7 = instr >> 25;

    switch (op) {
    case 0x37:
        snprintf(buf, cap, "lui   %s, 0x%x", rn(rd), instr & 0xFFFFF000u);
        return;
    case 0x17:
        snprintf(buf, cap, "auipc %s, 0x%x", rn(rd), instr & 0xFFFFF000u);
        return;
    case 0x6F:
        snprintf(buf, cap, "jal   %s, 0x%x", rn(rd), pc + imm_j_d(instr));
        return;
    case 0x67:
        snprintf(buf, cap, "jalr  %s, %s(%d)", rn(rd), rn(rs1),
                 (int)(int32_t)imm_i);
        return;
    case 0x63: {
        static const char *const m[8] = {
            "beq", "bne", "?", "?", "blt", "bge", "bltu", "bgeu"};
        uint32_t base = pc + sign_ext(((instr >> 31) & 1u) << 12 |
                                          ((instr >> 7) & 1u) << 11 |
                                          ((instr >> 25) & 0x3Fu) << 5 |
                                          ((instr >> 8) & 0xFu) << 1,
                                      13);
        snprintf(buf, cap, "%s  %s, %s, 0x%x", m[f3], rn(rs1), rn(rs2), base);
        return;
    }
    case 0x03: {
        static const char *const m[8] = {"lb", "lh", "lw", "?", "lbu", "lhu", "?", "?"};
        snprintf(buf, cap, "%s  %s, %d(%s)", m[f3], rn(rd),
                 (int)(int32_t)imm_i, rn(rs1));
        return;
    }
    case 0x23: {
        static const char *const m[8] = {"sb", "sh", "sw", "?", "?", "?", "?", "?"};
        snprintf(buf, cap, "%s  %s, %d(%s)", m[f3], rn(rs2),
                 (int)(int32_t)imm_s_d(instr), rn(rs1));
        return;
    }
    case 0x13:
        switch (f3) {
        case 0:
            snprintf(buf, cap, "addi  %s, %s, %d", rn(rd), rn(rs1),
                     (int)(int32_t)imm_i);
            return;
        case 2:
            snprintf(buf, cap, "slti  %s, %s, %d", rn(rd), rn(rs1),
                     (int)(int32_t)imm_i);
            return;
        case 3:
            snprintf(buf, cap, "sltiu %s, %s, %d", rn(rd), rn(rs1),
                     (int)(int32_t)imm_i);
            return;
        case 4:
            snprintf(buf, cap, "xori  %s, %s, %d", rn(rd), rn(rs1),
                     (int)(int32_t)imm_i);
            return;
        case 6:
            snprintf(buf, cap, "ori   %s, %s, %d", rn(rd), rn(rs1),
                     (int)(int32_t)imm_i);
            return;
        case 7:
            snprintf(buf, cap, "andi  %s, %s, 0x%x", rn(rd), rn(rs1), imm_i);
            return;
        case 1:
            snprintf(buf, cap, "slli  %s, %s, %d", rn(rd), rn(rs1),
                     (int)(imm_i & 0x1Fu));
            return;
        case 5:
            if (imm_i & 0x400u)
                snprintf(buf, cap, "srai  %s, %s, %d", rn(rd), rn(rs1),
                         (int)(imm_i & 0x1Fu));
            else
                snprintf(buf, cap, "srli  %s, %s, %d", rn(rd), rn(rs1),
                         (int)(imm_i & 0x1Fu));
            return;
        }
        break;
    case 0x33:
        if (f7 == 0x01) {
            static const char *const m[8] = {
                "mul", "mulh", "mulhsu", "mulhu", "div", "divu", "rem", "remu"};
            snprintf(buf, cap, "%s  %s, %s, %s", m[f3], rn(rd), rn(rs1), rn(rs2));
        } else if (f3 == 0 && (f7 & 0x20u)) {
            snprintf(buf, cap, "sub   %s, %s, %s", rn(rd), rn(rs1), rn(rs2));
        } else if (f3 == 5 && (f7 & 0x20u)) {
            snprintf(buf, cap, "sra   %s, %s, %s", rn(rd), rn(rs1), rn(rs2));
        } else {
            static const char *const m[8] = {
                "add", "sll", "slt", "sltu", "xor", "srl", "or", "and"};
            snprintf(buf, cap, "%s  %s, %s, %s", m[f3], rn(rd), rn(rs1), rn(rs2));
        }
        return;
    case 0x73:
        if (f3 == 0) {
            if (instr >> 20 == 0) {
                snprintf(buf, cap, "ecall");
                return;
            }
            if (instr >> 20 == 1) {
                snprintf(buf, cap, "ebreak");
                return;
            }
            if (instr >> 20 == 0x302) {
                snprintf(buf, cap, "mret");
                return;
            }
        }
        snprintf(buf, cap, "csr   %s, %s", rn(rd), rn(rs1));
        return;
    default:
        break;
    }
    snprintf(buf, cap, ".word 0x%08X", instr);
}
