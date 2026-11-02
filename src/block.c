#include "rv32.h"

#include <string.h>

/* The block cache is a direct port of the Go version. A block never crosses a
 * 4 KiB page; page-generation counters invalidate it after code writes. */

static inline int c_reg(uint32_t b) { return (int)(b & 0x7u) + 8; }

static uint32_t c_addi4spn_imm(uint32_t inst) {
    uint32_t imm = ((inst >> 7) & 0xFu) << 6;
    imm |= ((inst >> 11) & 0x3u) << 4;
    imm |= ((inst >> 6) & 1u) << 2;
    imm |= ((inst >> 5) & 1u) << 3;
    return imm;
}
static uint32_t c_lw_off(uint32_t inst) {
    uint32_t off = ((inst >> 5) & 1u) << 6;
    off |= ((inst >> 12) & 1u) << 5;
    off |= ((inst >> 11) & 1u) << 4;
    off |= ((inst >> 10) & 1u) << 3;
    off |= ((inst >> 6) & 1u) << 2;
    return off;
}
static uint32_t c_addi16sp_imm(uint32_t inst) {
    uint32_t imm = ((inst >> 12) & 1u) << 9;
    imm |= ((inst >> 4) & 1u) << 8;
    imm |= ((inst >> 3) & 1u) << 7;
    imm |= ((inst >> 5) & 1u) << 6;
    imm |= ((inst >> 2) & 1u) << 5;
    imm |= ((inst >> 6) & 1u) << 4;
    if (imm & (1u << 9)) imm |= ~0u << 10;
    return imm;
}
static uint32_t c_lwsp_off(uint32_t inst) {
    uint32_t off = ((inst >> 12) & 1u) << 5;
    off |= ((inst >> 6) & 1u) << 4;
    off |= ((inst >> 5) & 1u) << 3;
    off |= ((inst >> 4) & 1u) << 2;
    off |= ((inst >> 3) & 1u) << 7;
    off |= ((inst >> 2) & 1u) << 6;
    return off;
}
static uint32_t c_swsp_off(uint32_t inst) {
    uint32_t off = ((inst >> 9) & 0xFu) << 2;
    off |= ((inst >> 7) & 0x3u) << 6;
    return off;
}
static uint32_t c_j_offset(uint32_t inst) {
    uint32_t off = 0;
    off |= ((inst >> 12) & 1u) << 11;
    off |= ((inst >> 11) & 1u) << 4;
    off |= ((inst >> 10) & 1u) << 9;
    off |= ((inst >> 9) & 1u) << 8;
    off |= ((inst >> 8) & 1u) << 10;
    off |= ((inst >> 7) & 1u) << 6;
    off |= ((inst >> 6) & 1u) << 7;
    off |= ((inst >> 5) & 1u) << 3;
    off |= ((inst >> 4) & 1u) << 2;
    off |= ((inst >> 3) & 1u) << 1;
    off |= ((inst >> 2) & 1u) << 5;
    off &= 0xFFFu;
    if (off & (1u << 11)) off |= ~0u << 12;
    return off;
}
static uint32_t c_b_offset(uint32_t inst) {
    uint32_t off = 0;
    off |= ((inst >> 12) & 1u) << 8;
    off |= ((inst >> 6) & 1u) << 7;
    off |= ((inst >> 5) & 1u) << 6;
    off |= ((inst >> 2) & 1u) << 5;
    off |= ((inst >> 11) & 1u) << 4;
    off |= ((inst >> 10) & 1u) << 3;
    off |= ((inst >> 4) & 1u) << 2;
    off |= ((inst >> 3) & 1u) << 1;
    off &= 0x1FFu;
    if (off & (1u << 8)) off |= ~0u << 9;
    return off;
}

static uint32_t imm_i(uint32_t i) {
    uint32_t x = i >> 20;
    if (x & 0x800u) x |= ~0u << 12;
    return x;
}
static uint32_t imm_s(uint32_t i) {
    uint32_t x = ((i >> 25) << 5) | ((i >> 7) & 0x1Fu);
    if (x & 0x800u) x |= ~0u << 12;
    return x;
}
static uint32_t imm_b(uint32_t i) {
    uint32_t x = ((i >> 31) & 1u) << 12 | ((i >> 7) & 1u) << 11 |
                 ((i >> 25) & 0x3Fu) << 5 | ((i >> 8) & 0xFu) << 1;
    if (x & (1u << 12)) x |= ~0u << 13;
    return x;
}
static uint32_t imm_j(uint32_t i) {
    uint32_t x = ((i >> 31) & 1u) << 20 | ((i >> 12) & 0xFFu) << 12 |
                 ((i >> 20) & 1u) << 11 | ((i >> 21) & 0x3FFu) << 1;
    if (x & (1u << 20)) x |= ~0u << 21;
    return x;
}

static bool decode_i32(uint32_t instr, h_op_t *o, bool *term) {
    *term = false;
    o->rd = (uint8_t)((instr >> 7) & 0x1Fu);
    o->rs1 = (uint8_t)((instr >> 15) & 0x1Fu);
    o->rs2 = (uint8_t)((instr >> 20) & 0x1Fu);
    uint32_t f3 = (instr >> 12) & 0x7u;

    switch (instr & 0x7Fu) {
    case 0x13:
        o->imm = (int32_t)imm_i(instr);
        switch (f3) {
        case 0: o->kind = K_ADDI; break;
        case 2: o->kind = K_SLTI; break;
        case 3: o->kind = K_SLTIU; break;
        case 4: o->kind = K_XORI; break;
        case 6: o->kind = K_ORI; break;
        case 7: o->kind = K_ANDI; break;
        case 1: o->kind = K_SLLI; break;
        case 5:
            o->kind = (o->imm & 0x400) ? K_SRAI : K_SRLI;
            break;
        }
        return true;
    case 0x33: {
        uint32_t f7 = instr >> 25;
        if (f7 == 0x01) {
            o->kind = K_MULEXT;
            o->imm = (int32_t)f3;
            return true;
        }
        switch (f3) {
        case 0: o->kind = (f7 & 0x20u) ? K_SUB : K_ADD; break;
        case 1: o->kind = K_SLL; break;
        case 2: o->kind = K_SLT; break;
        case 3: o->kind = K_SLTU; break;
        case 4: o->kind = K_XOR; break;
        case 5: o->kind = (f7 & 0x20u) ? K_SRA : K_SRL; break;
        case 6: o->kind = K_OR; break;
        case 7: o->kind = K_AND; break;
        }
        return true;
    }
    case 0x63:
        o->imm = (int32_t)imm_b(instr);
        switch (f3) {
        case 0: o->kind = K_BEQ; break;
        case 1: o->kind = K_BNE; break;
        case 4: o->kind = K_BLT; break;
        case 5: o->kind = K_BGE; break;
        case 6: o->kind = K_BLTU; break;
        case 7: o->kind = K_BGEU; break;
        default: *term = true; return false;
        }
        *term = true;
        return true;
    case 0x03:
        o->imm = (int32_t)imm_i(instr);
        if (o->rd == 0) {
            *term = true;
            return false;
        }
        switch (f3) {
        case 0: o->kind = K_LB; break;
        case 1: o->kind = K_LH; break;
        case 2: o->kind = K_LW; break;
        case 4: o->kind = K_LBU; break;
        case 5: o->kind = K_LHU; break;
        default: *term = true; return false;
        }
        return true;
    case 0x23:
        o->imm = (int32_t)imm_s(instr);
        switch (f3) {
        case 0: o->kind = K_SB; break;
        case 1: o->kind = K_SH; break;
        case 2: o->kind = K_SW; break;
        default: *term = true; return false;
        }
        return true;
    case 0x37:
        o->kind = K_LUI;
        o->imm = (int32_t)(instr & 0xFFFFF000u);
        return true;
    case 0x17:
        o->kind = K_AUIPC;
        o->imm = (int32_t)(instr & 0xFFFFF000u);
        return true;
    case 0x6F:
        o->kind = K_JAL;
        o->imm = (int32_t)imm_j(instr);
        *term = true;
        return true;
    case 0x67:
        o->kind = K_JALR;
        o->imm = (int32_t)imm_i(instr);
        *term = true;
        return true;
    case 0x0F:
        o->kind = K_NOP;
        return true;
    default:
        *term = true;
        return false;
    }
}

static bool decode_i16(uint32_t inst, h_op_t *o, bool *term) {
    *term = false;
    switch (inst & 0x3u) {
    case 0x0:
        switch ((inst >> 13) & 0x7u) {
        case 0x0: {
            uint32_t imm = c_addi4spn_imm(inst);
            if (imm == 0) {
                *term = true;
                return false;
            }
            o->kind = K_CADDI4SPN;
            o->rd = (uint8_t)c_reg((inst >> 2) & 0x7u);
            o->imm = (int32_t)imm;
            return true;
        }
        case 0x2:
            o->kind = K_CLW;
            o->rd = (uint8_t)c_reg((inst >> 2) & 0x7u);
            o->rs1 = (uint8_t)c_reg((inst >> 7) & 0x7u);
            o->imm = (int32_t)c_lw_off(inst);
            return true;
        case 0x6:
            o->kind = K_CSW;
            o->rs2 = (uint8_t)c_reg((inst >> 2) & 0x7u);
            o->rs1 = (uint8_t)c_reg((inst >> 7) & 0x7u);
            o->imm = (int32_t)c_lw_off(inst);
            return true;
        default:
            *term = true;
            return false;
        }
    case 0x1: {
        uint32_t rd5 = (inst >> 7) & 0x1Fu;
        int cd = c_reg((inst >> 7) & 0x7u);
        uint32_t simm = (((inst >> 12) & 1u) << 5) | ((inst >> 2) & 0x1Fu);
        if (simm & 0x20u) simm |= ~0u << 6;
        switch ((inst >> 13) & 0x7u) {
        case 0x0:
            if (rd5 == 0) {
                *term = true;
                return false;
            }
            o->kind = K_CADDI;
            o->rd = (uint8_t)rd5;
            o->rs1 = (uint8_t)rd5;
            o->imm = (int32_t)simm;
            return true;
        case 0x1:
            o->kind = K_CJAL;
            o->imm = (int32_t)c_j_offset(inst);
            *term = true;
            return true;
        case 0x2:
            if (rd5 == 0) {
                *term = true;
                return false;
            }
            o->kind = K_CLI;
            o->rd = (uint8_t)rd5;
            o->imm = (int32_t)simm;
            return true;
        case 0x3:
            if (rd5 == 2) {
                o->kind = K_CADDI16SP;
                o->imm = (int32_t)c_addi16sp_imm(inst);
                return true;
            }
            if (rd5 == 0) {
                *term = true;
                return false;
            }
            o->kind = K_CLUI;
            o->rd = (uint8_t)rd5;
            o->imm = (int32_t)simm;
            return true;
        case 0x4:
            switch ((inst >> 10) & 0x3u) {
            case 0x0:
                o->kind = K_CSRLI;
                o->rd = (uint8_t)cd;
                o->rs1 = (uint8_t)cd;
                o->imm = (int32_t)((inst >> 2) & 0x1Fu);
                return true;
            case 0x1:
                o->kind = K_CSRAI;
                o->rd = (uint8_t)cd;
                o->rs1 = (uint8_t)cd;
                o->imm = (int32_t)((inst >> 2) & 0x1Fu);
                return true;
            case 0x2:
                o->kind = K_CANDI;
                o->rd = (uint8_t)cd;
                o->rs1 = (uint8_t)cd;
                o->imm = (int32_t)simm;
                return true;
            default:
                o->rd = (uint8_t)cd;
                o->rs1 = (uint8_t)cd;
                o->rs2 = (uint8_t)c_reg((inst >> 2) & 0x7u);
                switch ((inst >> 5) & 0x3u) {
                case 0x0: o->kind = K_CSUB; break;
                case 0x1: o->kind = K_CXOR; break;
                case 0x2: o->kind = K_COR; break;
                case 0x3: o->kind = K_CAND; break;
                }
                return true;
            }
        case 0x5:
            o->kind = K_CJ;
            o->imm = (int32_t)c_j_offset(inst);
            *term = true;
            return true;
        case 0x6:
            o->kind = K_CBEQZ;
            o->rs1 = (uint8_t)c_reg((inst >> 7) & 0x7u);
            o->imm = (int32_t)c_b_offset(inst);
            *term = true;
            return true;
        case 0x7:
            o->kind = K_CBNEZ;
            o->rs1 = (uint8_t)c_reg((inst >> 7) & 0x7u);
            o->imm = (int32_t)c_b_offset(inst);
            *term = true;
            return true;
        }
        break;
    }
    case 0x2: {
        uint32_t rd5 = (inst >> 7) & 0x1Fu;
        uint32_t shamt = (inst >> 2) & 0x1Fu;
        switch ((inst >> 13) & 0x7u) {
        case 0x0:
            if (rd5 == 0) {
                *term = true;
                return false;
            }
            o->kind = K_CSLLI;
            o->rd = (uint8_t)rd5;
            o->rs1 = (uint8_t)rd5;
            o->imm = (int32_t)shamt;
            return true;
        case 0x2:
            if (rd5 == 0) {
                *term = true;
                return false;
            }
            o->kind = K_CLWSP;
            o->rd = (uint8_t)rd5;
            o->rs1 = 2;
            o->imm = (int32_t)c_lwsp_off(inst);
            return true;
        case 0x6:
            o->kind = K_CSWSP;
            o->rs2 = (uint8_t)((inst >> 2) & 0x1Fu);
            o->rs1 = 2;
            o->imm = (int32_t)c_swsp_off(inst);
            return true;
        case 0x4: {
            uint32_t rs2 = (inst >> 2) & 0x1Fu;
            if (((inst >> 12) & 1u) == 0) {
                if (rs2 == 0) {
                    if (rd5 == 0) {
                        *term = true;
                        return false;
                    }
                    o->kind = K_CJR;
                    o->rd = (uint8_t)rd5;
                    o->rs1 = (uint8_t)rd5;
                    *term = true;
                    return true;
                }
                if (rd5 == 0) {
                    *term = true;
                    return false;
                }
                o->kind = K_CMV;
                o->rd = (uint8_t)rd5;
                o->rs2 = (uint8_t)rs2;
                return true;
            }
            if (rs2 == 0) {
                if (rd5 == 0) {
                    *term = true;
                    return false;
                }
                o->kind = K_CJALR;
                o->rd = (uint8_t)rd5;
                o->rs1 = (uint8_t)rd5;
                *term = true;
                return true;
            }
            if (rd5 == 0) {
                *term = true;
                return false;
            }
            o->kind = K_CADD;
            o->rd = (uint8_t)rd5;
            o->rs1 = (uint8_t)rd5;
            o->rs2 = (uint8_t)rs2;
            return true;
        }
        default:
            *term = true;
            return false;
        }
    }
    default:
        *term = true;
        return false;
    }
    *term = true;
    return false;
}

static h_block_t *block_for(cpu_t *c, uint32_t pc, uint32_t off) {
    h_block_t *blk = &c->blocks[((pc >> 1) ^ (pc >> 10)) & (BLOCK_SLOTS - 1)];
    if (blk->start == pc && blk->gen == c->code_gen[off >> 12]) return blk;

    uint8_t *mem = c->flat;
    uint32_t page = off >> 12;
    uint32_t lim = c->flat_size;
    uint32_t page_end = (page + 1) << 12;
    if (page_end < lim) lim = page_end;

    uint32_t cur = off, cur_pc = pc;
    int n = 0;
    while (n < BLOCK_MAX_OPS) {
        if (cur + 2 > lim) break;
        uint32_t low = (uint32_t)mem[cur] | ((uint32_t)mem[cur + 1] << 8);
        h_op_t o;
        memset(&o, 0, sizeof(o));
        bool ok, term;
        if ((low & 0x3u) == 0x3u) {
            if (cur + 4 > lim) break;
            uint32_t instr = low | (((uint32_t)mem[cur + 2] |
                                     ((uint32_t)mem[cur + 3] << 8))
                                    << 16);
            ok = decode_i32(instr, &o, &term);
            o.ln = 4;
        } else {
            ok = decode_i16(low, &o, &term);
            o.ln = 2;
        }
        (void)cur_pc;
        if (!ok) break;
        blk->ops[n++] = o;
        cur += o.ln;
        cur_pc += o.ln;
        if (term) break;
    }
    if (n == 0) return NULL;
    blk->start = pc;
    blk->gen = c->code_gen[page];
    blk->n = (uint8_t)n;
    c->code_page[page] = 1;
    return blk;
}

static inline void touch_code(cpu_t *c, uint32_t off) {
    uint32_t p = off >> 12;
    if (c->code_page[p]) c->code_gen[p]++;
}

static uint64_t run_block(cpu_t *c, h_block_t *blk, uint32_t budget,
                          bool *stepped) {
    uint8_t *mem = c->flat;
    uint32_t base = c->flat_base;
    uint32_t size = c->flat_size;
    uint32_t *x = c->x;
    uint32_t cur_pc = blk->start;
    uint32_t done = 0;

    for (;;) {
        uint32_t n = blk->n;
        if (budget - done < n) n = budget - done;

        for (uint32_t i = 0; i < n; i++) {
            h_op_t *o = &blk->ops[i];
            uint32_t next = cur_pc + o->ln;
            uint32_t rd = o->rd & 31u;
            uint32_t rs1 = o->rs1 & 31u;
            uint32_t rs2 = o->rs2 & 31u;

            switch ((op_kind_t)o->kind) {
            case K_ADDI:
                if (rd) x[rd] = x[rs1] + (uint32_t)o->imm;
                cur_pc = next;
                break;
            case K_SLTI:
                if (rd) x[rd] = ((int32_t)x[rs1] < o->imm) ? 1u : 0u;
                cur_pc = next;
                break;
            case K_SLTIU:
                if (rd) x[rd] = (x[rs1] < (uint32_t)o->imm) ? 1u : 0u;
                cur_pc = next;
                break;
            case K_XORI:
                if (rd) x[rd] = x[rs1] ^ (uint32_t)o->imm;
                cur_pc = next;
                break;
            case K_ORI:
                if (rd) x[rd] = x[rs1] | (uint32_t)o->imm;
                cur_pc = next;
                break;
            case K_ANDI:
                if (rd) x[rd] = x[rs1] & (uint32_t)o->imm;
                cur_pc = next;
                break;
            case K_SLLI:
                if (rd) x[rd] = x[rs1] << ((uint32_t)o->imm & 0x1Fu);
                cur_pc = next;
                break;
            case K_SRLI:
                if (rd) x[rd] = x[rs1] >> ((uint32_t)o->imm & 0x1Fu);
                cur_pc = next;
                break;
            case K_SRAI:
                if (rd)
                    x[rd] = (uint32_t)((int32_t)x[rs1] >>
                                       ((uint32_t)o->imm & 0x1Fu));
                cur_pc = next;
                break;
            case K_ADD:
                if (rd) x[rd] = x[rs1] + x[rs2];
                cur_pc = next;
                break;
            case K_SUB:
                if (rd) x[rd] = x[rs1] - x[rs2];
                cur_pc = next;
                break;
            case K_SLL:
                if (rd) x[rd] = x[rs1] << (x[rs2] & 0x1Fu);
                cur_pc = next;
                break;
            case K_SLT:
                if (rd) x[rd] = ((int32_t)x[rs1] < (int32_t)x[rs2]) ? 1u : 0u;
                cur_pc = next;
                break;
            case K_SLTU:
                if (rd) x[rd] = (x[rs1] < x[rs2]) ? 1u : 0u;
                cur_pc = next;
                break;
            case K_XOR:
                if (rd) x[rd] = x[rs1] ^ x[rs2];
                cur_pc = next;
                break;
            case K_SRL:
                if (rd) x[rd] = x[rs1] >> (x[rs2] & 0x1Fu);
                cur_pc = next;
                break;
            case K_SRA:
                if (rd)
                    x[rd] = (uint32_t)((int32_t)x[rs1] >> (x[rs2] & 0x1Fu));
                cur_pc = next;
                break;
            case K_OR:
                if (rd) x[rd] = x[rs1] | x[rs2];
                cur_pc = next;
                break;
            case K_AND:
                if (rd) x[rd] = x[rs1] & x[rs2];
                cur_pc = next;
                break;
            case K_MULEXT:
                cpu_exec_m(c, (uint32_t)o->imm, (int)rd, x[rs1], x[rs2]);
                cur_pc = next;
                break;
            case K_LUI:
                if (rd) x[rd] = (uint32_t)o->imm;
                cur_pc = next;
                break;
            case K_AUIPC:
                if (rd) x[rd] = cur_pc + (uint32_t)o->imm;
                cur_pc = next;
                break;
            case K_JAL:
                if (rd) x[rd] = cur_pc + 4;
                cur_pc += (uint32_t)o->imm;
                break;
            case K_JALR: {
                uint32_t t = (x[rs1] + (uint32_t)o->imm) & ~1u;
                if (rd) x[rd] = cur_pc + 4;
                cur_pc = t;
                break;
            }
            case K_BEQ:
                cur_pc = (x[rs1] == x[rs2]) ? cur_pc + (uint32_t)o->imm : next;
                break;
            case K_BNE:
                cur_pc = (x[rs1] != x[rs2]) ? cur_pc + (uint32_t)o->imm : next;
                break;
            case K_BLT:
                cur_pc = ((int32_t)x[rs1] < (int32_t)x[rs2])
                             ? cur_pc + (uint32_t)o->imm
                             : next;
                break;
            case K_BGE:
                cur_pc = ((int32_t)x[rs1] >= (int32_t)x[rs2])
                             ? cur_pc + (uint32_t)o->imm
                             : next;
                break;
            case K_BLTU:
                cur_pc = (x[rs1] < x[rs2]) ? cur_pc + (uint32_t)o->imm : next;
                break;
            case K_BGEU:
                cur_pc = (x[rs1] >= x[rs2]) ? cur_pc + (uint32_t)o->imm : next;
                break;

            case K_LB: {
                uint32_t addr = x[rs1] + (uint32_t)o->imm;
                if (c->priv != 0) {
                    uint32_t d = addr - base;
                    if (d < size) {
                        x[rd] = (uint32_t)(int32_t)(int8_t)mem[d];
                        cur_pc = next;
                        break;
                    }
                }
                c->pc = cur_pc;
                cpu_step(c);
                *stepped = true;
                return done + i;
            }
            case K_LH: {
                uint32_t addr = x[rs1] + (uint32_t)o->imm;
                if (c->priv != 0 && (addr & 1u) == 0) {
                    uint32_t d = addr - base;
                    if (d < size - 1) {
                        x[rd] = (uint32_t)(int32_t)(int16_t)(
                            (uint16_t)mem[d] | ((uint16_t)mem[d + 1] << 8));
                        cur_pc = next;
                        break;
                    }
                }
                c->pc = cur_pc;
                cpu_step(c);
                *stepped = true;
                return done + i;
            }
            case K_LW: {
                uint32_t addr = x[rs1] + (uint32_t)o->imm;
                if (c->priv != 0 && (addr & 3u) == 0) {
                    uint32_t d = addr - base;
                    if (d < size - 3) {
                        x[rd] = (uint32_t)mem[d] |
                                ((uint32_t)mem[d + 1] << 8) |
                                ((uint32_t)mem[d + 2] << 16) |
                                ((uint32_t)mem[d + 3] << 24);
                        cur_pc = next;
                        break;
                    }
                }
                c->pc = cur_pc;
                cpu_step(c);
                *stepped = true;
                return done + i;
            }
            case K_LBU: {
                uint32_t addr = x[rs1] + (uint32_t)o->imm;
                if (c->priv != 0) {
                    uint32_t d = addr - base;
                    if (d < size) {
                        x[rd] = (uint32_t)mem[d];
                        cur_pc = next;
                        break;
                    }
                }
                c->pc = cur_pc;
                cpu_step(c);
                *stepped = true;
                return done + i;
            }
            case K_LHU: {
                uint32_t addr = x[rs1] + (uint32_t)o->imm;
                if (c->priv != 0 && (addr & 1u) == 0) {
                    uint32_t d = addr - base;
                    if (d < size - 1) {
                        x[rd] = (uint32_t)mem[d] |
                                ((uint32_t)mem[d + 1] << 8);
                        cur_pc = next;
                        break;
                    }
                }
                c->pc = cur_pc;
                cpu_step(c);
                *stepped = true;
                return done + i;
            }
            case K_SB: {
                uint32_t addr = x[rs1] + (uint32_t)o->imm;
                if (c->priv != 0) {
                    uint32_t d = addr - base;
                    if (d < size) {
                        mem[d] = (uint8_t)x[rs2];
                        touch_code(c, d);
                        cur_pc = next;
                        break;
                    }
                }
                c->pc = cur_pc;
                cpu_step(c);
                *stepped = true;
                return done + i;
            }
            case K_SH: {
                uint32_t addr = x[rs1] + (uint32_t)o->imm;
                if (c->priv != 0 && (addr & 1u) == 0) {
                    uint32_t d = addr - base;
                    if (d < size - 1) {
                        uint32_t v = x[rs2];
                        mem[d] = (uint8_t)v;
                        mem[d + 1] = (uint8_t)(v >> 8);
                        touch_code(c, d);
                        cur_pc = next;
                        break;
                    }
                }
                c->pc = cur_pc;
                cpu_step(c);
                *stepped = true;
                return done + i;
            }
            case K_SW: {
                uint32_t addr = x[rs1] + (uint32_t)o->imm;
                if (c->priv != 0 && (addr & 3u) == 0) {
                    uint32_t d = addr - base;
                    if (d < size - 3) {
                        uint32_t v = x[rs2];
                        mem[d] = (uint8_t)v;
                        mem[d + 1] = (uint8_t)(v >> 8);
                        mem[d + 2] = (uint8_t)(v >> 16);
                        mem[d + 3] = (uint8_t)(v >> 24);
                        touch_code(c, d);
                        cur_pc = next;
                        break;
                    }
                }
                c->pc = cur_pc;
                cpu_step(c);
                *stepped = true;
                return done + i;
            }
            case K_NOP:
                cur_pc = next;
                break;

            case K_CADDI4SPN:
                x[rd] = x[2] + (uint32_t)o->imm;
                cur_pc = next;
                break;
            case K_CLW: {
                uint32_t addr = x[rs1] + (uint32_t)o->imm;
                if (c->priv != 0 && (addr & 3u) == 0) {
                    uint32_t d = addr - base;
                    if (d < size - 3) {
                        x[rd] = (uint32_t)mem[d] |
                                ((uint32_t)mem[d + 1] << 8) |
                                ((uint32_t)mem[d + 2] << 16) |
                                ((uint32_t)mem[d + 3] << 24);
                        cur_pc = next;
                        break;
                    }
                }
                c->pc = cur_pc;
                cpu_step(c);
                *stepped = true;
                return done + i;
            }
            case K_CSW: {
                uint32_t addr = x[rs1] + (uint32_t)o->imm;
                if (c->priv != 0 && (addr & 3u) == 0) {
                    uint32_t d = addr - base;
                    if (d < size - 3) {
                        uint32_t v = x[rs2];
                        mem[d] = (uint8_t)v;
                        mem[d + 1] = (uint8_t)(v >> 8);
                        mem[d + 2] = (uint8_t)(v >> 16);
                        mem[d + 3] = (uint8_t)(v >> 24);
                        touch_code(c, d);
                        cur_pc = next;
                        break;
                    }
                }
                c->pc = cur_pc;
                cpu_step(c);
                *stepped = true;
                return done + i;
            }
            case K_CADDI:
                x[rd] = x[rs1] + (uint32_t)o->imm;
                cur_pc = next;
                break;
            case K_CJAL:
                x[1] = cur_pc + 2;
                cur_pc += (uint32_t)o->imm;
                break;
            case K_CLI:
                x[rd] = (uint32_t)o->imm;
                cur_pc = next;
                break;
            case K_CADDI16SP:
                x[2] = x[2] + (uint32_t)o->imm;
                cur_pc = next;
                break;
            case K_CLUI:
                x[rd] = (uint32_t)o->imm << 12;
                cur_pc = next;
                break;
            case K_CSRLI:
                x[rd] = x[rs1] >> ((uint32_t)o->imm & 0x1Fu);
                cur_pc = next;
                break;
            case K_CSRAI:
                x[rd] = (uint32_t)((int32_t)x[rs1] >>
                                   ((uint32_t)o->imm & 0x1Fu));
                cur_pc = next;
                break;
            case K_CANDI:
                x[rd] = x[rs1] & (uint32_t)o->imm;
                cur_pc = next;
                break;
            case K_CSUB:
                x[rd] = x[rs1] - x[rs2];
                cur_pc = next;
                break;
            case K_CXOR:
                x[rd] = x[rs1] ^ x[rs2];
                cur_pc = next;
                break;
            case K_COR:
                x[rd] = x[rs1] | x[rs2];
                cur_pc = next;
                break;
            case K_CAND:
                x[rd] = x[rs1] & x[rs2];
                cur_pc = next;
                break;
            case K_CJ:
                cur_pc += (uint32_t)o->imm;
                break;
            case K_CBEQZ:
                cur_pc = (x[rs1] == 0) ? cur_pc + (uint32_t)o->imm : next;
                break;
            case K_CBNEZ:
                cur_pc = (x[rs1] != 0) ? cur_pc + (uint32_t)o->imm : next;
                break;
            case K_CSLLI:
                x[rd] = x[rs1] << ((uint32_t)o->imm & 0x1Fu);
                cur_pc = next;
                break;
            case K_CLWSP: {
                uint32_t addr = x[2] + (uint32_t)o->imm;
                if (c->priv != 0 && (addr & 3u) == 0) {
                    uint32_t d = addr - base;
                    if (d < size - 3) {
                        x[rd] = (uint32_t)mem[d] |
                                ((uint32_t)mem[d + 1] << 8) |
                                ((uint32_t)mem[d + 2] << 16) |
                                ((uint32_t)mem[d + 3] << 24);
                        cur_pc = next;
                        break;
                    }
                }
                c->pc = cur_pc;
                cpu_step(c);
                *stepped = true;
                return done + i;
            }
            case K_CSWSP: {
                uint32_t addr = x[2] + (uint32_t)o->imm;
                if (c->priv != 0 && (addr & 3u) == 0) {
                    uint32_t d = addr - base;
                    if (d < size - 3) {
                        uint32_t v = x[rs2];
                        mem[d] = (uint8_t)v;
                        mem[d + 1] = (uint8_t)(v >> 8);
                        mem[d + 2] = (uint8_t)(v >> 16);
                        mem[d + 3] = (uint8_t)(v >> 24);
                        touch_code(c, d);
                        cur_pc = next;
                        break;
                    }
                }
                c->pc = cur_pc;
                cpu_step(c);
                *stepped = true;
                return done + i;
            }
            case K_CJR:
                cur_pc = x[rd] & ~1u;
                break;
            case K_CMV:
                x[rd] = x[rs2];
                cur_pc = next;
                break;
            case K_CJALR:
                x[1] = cur_pc + 2;
                cur_pc = x[rs1] & ~1u;
                break;
            case K_CADD:
                x[rd] = x[rs1] + x[rs2];
                cur_pc = next;
                break;
            default:
                cur_pc = next;
                break;
            }
        }

        done += n;
        if (done >= budget) {
            c->pc = cur_pc;
            return done;
        }
        uint32_t off = cur_pc - base;
        if (off >= size - 3) {
            c->pc = cur_pc;
            return done;
        }
        h_block_t *succ =
            &c->blocks[((cur_pc >> 1) ^ (cur_pc >> 10)) & (BLOCK_SLOTS - 1)];
        if (succ->start == cur_pc && succ->gen == c->code_gen[off >> 12]) {
            blk = succ;
            continue;
        }
        blk = block_for(c, cur_pc, off);
        if (!blk) {
            c->pc = cur_pc;
            return done;
        }
    }
}

uint64_t cpu_run_hot(cpu_t *c, uint64_t max) {
    if (!c->flat) {
        uint64_t i = 0;
        for (; (max == 0 || i < max) && cpu_step(c); i++) {
        }
        return i;
    }
    uint32_t size = c->flat_size;
    if (c->halted) return 0;
    /* Pending interrupts are sampled once per batch, as in the Go runner. */
    cpu_sample_interrupt(c);
    if (c->halted) return 0;

    uint64_t count = 0, hot = 0;
    while (max == 0 || count < max) {
        if (c->halted) break;
        uint32_t pc = c->pc;
        uint32_t off = pc - c->flat_base;
        if (off >= size - 3) {
            for (; max == 0 || count < max;) {
                cpu_step(c);
                count++;
                if (c->halted || c->pc - c->flat_base < size - 3) break;
            }
            continue;
        }
        h_block_t *blk = block_for(c, pc, off);
        if (!blk) {
            cpu_step(c);
            count++;
            continue;
        }
        uint64_t k = 0x7FFFFFFFu;
        if (max != 0 && max - count < k) k = max - count;
        bool stepped = false;
        uint64_t n = run_block(c, blk, (uint32_t)k, &stepped);
        count += n;
        hot += n;
        if (stepped) count++;
    }
    c->csr_mcycle += hot;
    c->csr_minstret += hot;
    c->inst_count += hot;
    return count;
}

