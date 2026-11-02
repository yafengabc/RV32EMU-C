#include "rv32.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define MSTATUS_MIE  (1u << 3)
#define MSTATUS_MPIE (1u << 7)
#define MSTATUS_MPP  (3u << 11)

#define CSR_MSTATUS  0x300u
#define CSR_MISA     0x301u
#define CSR_MEDELEG  0x302u
#define CSR_MIDELEG  0x303u
#define CSR_MIE      0x304u
#define CSR_MTVEC    0x305u
#define CSR_MSCRATCH 0x340u
#define CSR_MEPC     0x341u
#define CSR_MCAUSE   0x342u
#define CSR_MTVAL    0x343u
#define CSR_MIP      0x344u
#define CSR_MCYCLE   0xB00u
#define CSR_MINSTRET 0xB02u
#define CSR_MHARTID  0xF14u
#define CSR_PMPCFG0  0x3A0u
#define CSR_PMPADDR0 0x3B0u
#define CSR_PMPADDR15 0x3BFu

#define PMP_READ  (1u << 0)
#define PMP_WRITE (1u << 1)
#define PMP_EXEC  (1u << 2)

static inline uint32_t sign_extend(uint32_t x, unsigned bits) {
    if (x & (1u << (bits - 1))) return x | (~0u << bits);
    return x;
}

static uint32_t imm_i(uint32_t i) { return sign_extend(i >> 20, 12); }
static uint32_t imm_s(uint32_t i) {
    return sign_extend(((i >> 25) << 5) | ((i >> 7) & 0x1F), 12);
}
static uint32_t imm_b(uint32_t i) {
    return sign_extend(((i >> 31) & 1) << 12 | ((i >> 7) & 1) << 11 |
                           ((i >> 25) & 0x3F) << 5 | ((i >> 8) & 0xF) << 1,
                       13);
}
static uint32_t imm_u(uint32_t i) { return i & 0xFFFFF000u; }
static uint32_t imm_j(uint32_t i) {
    return sign_extend(((i >> 31) & 1) << 20 | ((i >> 12) & 0xFF) << 12 |
                           ((i >> 20) & 1) << 11 | ((i >> 21) & 0x3FF) << 1,
                       21);
}

static inline uint32_t bool_u32(bool b) { return b ? 1u : 0u; }

cpu_t *cpu_new(bus_t *bus) {
    cpu_t *c = (cpu_t *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->bus = bus;
    c->pc = RESET_VECTOR;
    c->priv = 3;
    c->pmp_enforce = true;
    c->flat = bus->flat;
    c->flat_base = FLAT_BASE;
    c->flat_size = FLAT_SIZE;
    c->code_page = bus->code_page;
    c->code_gen = bus->code_gen;
    c->csr_misa = (1u << 30) | (1u << 8) | (1u << 12) | (1u << 2);
    c->csr_mstatus = MSTATUS_MPP;
    return c;
}

void cpu_free(cpu_t *c) { free(c); }

uint32_t cpu_reg(cpu_t *c, int i) { return (i == 0) ? 0 : c->x[i]; }
void cpu_set_reg(cpu_t *c, int i, uint32_t v) {
    if (i != 0) c->x[i] = v;
}
void cpu_set_pc(cpu_t *c, uint32_t pc) { c->pc = pc; }
void cpu_set_pending_int(cpu_t *c, uint32_t cause) { c->pending_int = cause; }
void cpu_set_pmp_enforce(cpu_t *c, bool v) { c->pmp_enforce = v; }

static inline void set_rd(cpu_t *c, int rd, uint32_t v) {
    if (rd != 0) c->x[rd] = v;
}

/* ---- CSR ------------------------------------------------------------ */

bool cpu_csr_read(cpu_t *c, uint32_t addr, uint32_t *val) {
    switch (addr) {
    case CSR_MSTATUS: *val = c->csr_mstatus; return true;
    case CSR_MISA: *val = c->csr_misa; return true;
    case CSR_MEDELEG:
    case CSR_MIDELEG: *val = 0; return true;
    case CSR_MIE: *val = c->csr_mie; return true;
    case CSR_MTVEC: *val = c->csr_mtvec; return true;
    case CSR_MSCRATCH: *val = c->csr_mscratch; return true;
    case CSR_MEPC: *val = c->csr_mepc; return true;
    case CSR_MCAUSE: *val = c->csr_mcause; return true;
    case CSR_MTVAL: *val = c->csr_mtval; return true;
    case CSR_MIP: *val = c->csr_mip; return true;
    case CSR_MCYCLE: *val = (uint32_t)c->csr_mcycle; return true;
    case CSR_MINSTRET: *val = (uint32_t)c->csr_minstret; return true;
    case CSR_MHARTID: *val = 0; return true;
    default:
        if (addr >= CSR_PMPCFG0 && addr <= CSR_PMPCFG0 + 3) {
            *val = c->pmpcfg[addr - CSR_PMPCFG0];
            return true;
        }
        if (addr >= CSR_PMPADDR0 && addr <= CSR_PMPADDR15) {
            *val = c->pmpaddr[addr - CSR_PMPADDR0];
            return true;
        }
        *val = 0;
        return false;
    }
}

bool cpu_csr_write(cpu_t *c, uint32_t addr, uint32_t val) {
    switch (addr) {
    case CSR_MSTATUS:
        c->csr_mstatus = val & (MSTATUS_MIE | MSTATUS_MPIE | MSTATUS_MPP);
        return true;
    case CSR_MIE: c->csr_mie = val; return true;
    case CSR_MTVEC: c->csr_mtvec = val; return true;
    case CSR_MSCRATCH: c->csr_mscratch = val; return true;
    case CSR_MEPC: c->csr_mepc = val; return true;
    case CSR_MCAUSE: c->csr_mcause = val; return true;
    case CSR_MTVAL: c->csr_mtval = val; return true;
    case CSR_MCYCLE: c->csr_mcycle = (uint64_t)val; return true;
    case CSR_MINSTRET: c->csr_minstret = (uint64_t)val; return true;
    case CSR_MISA:
    case CSR_MHARTID:
    case CSR_MEDELEG:
    case CSR_MIDELEG:
        return false;
    default:
        if (addr >= CSR_PMPCFG0 && addr <= CSR_PMPCFG0 + 3) {
            c->pmpcfg[addr - CSR_PMPCFG0] = val;
            c->pmp_dirty = true;
            return true;
        }
        if (addr >= CSR_PMPADDR0 && addr <= CSR_PMPADDR15) {
            c->pmpaddr[addr - CSR_PMPADDR0] = val;
            c->pmp_dirty = true;
            return true;
        }
        return false;
    }
}

/* ---- traps ---------------------------------------------------------- */

static void enter_trap(cpu_t *c, uint32_t cause, uint32_t tval, uint32_t epc) {
    if ((cause & CAUSE_INTERRUPT_BIT) == 0) c->exc_count++;
    else c->int_count++;

    c->csr_mepc = epc;
    c->csr_mcause = cause;
    c->csr_mtval = tval;
    c->csr_mstatus = (c->csr_mstatus & ~MSTATUS_MPP) | ((c->priv & 3u) << 11);
    if (c->csr_mstatus & MSTATUS_MIE) c->csr_mstatus |= MSTATUS_MPIE;
    c->csr_mstatus &= ~MSTATUS_MIE;
    c->priv = 3;

    if (c->on_trap) c->on_trap(cause, epc, tval);

    uint32_t base = c->csr_mtvec & ~0x3u;
    uint32_t mode = c->csr_mtvec & 0x3u;
    if (mode == 1 && (cause & CAUSE_INTERRUPT_BIT)) {
        base += 4u * (cause & 0x1Fu);
    }
    c->pc = base;
}

static void mret(cpu_t *c) {
    if (c->csr_mstatus & MSTATUS_MPIE) c->csr_mstatus |= MSTATUS_MIE;
    else c->csr_mstatus &= ~MSTATUS_MIE;
    c->csr_mstatus |= MSTATUS_MPIE;
    c->priv = (c->csr_mstatus >> 11) & 3u;
    c->pc = c->csr_mepc;
}

static void check_interrupts(cpu_t *c) {
    if (c->pending_int == 0) return;
    if ((c->csr_mstatus & MSTATUS_MIE) == 0) return;
    uint32_t cause = c->pending_int;
    c->pending_int = 0;
    enter_trap(c, cause, 0, c->pc);
}

void cpu_sample_interrupt(cpu_t *c) { check_interrupts(c); }

static void illegal(cpu_t *c, uint32_t instr) {
    enter_trap(c, CAUSE_ILLEGAL_INSTR, instr, c->cur_pc);
}

/* ---- PMP ------------------------------------------------------------ */

static void pmp_rebuild(cpu_t *c) {
    c->pmp_dirty = false;
    c->pmp_n = 0;
    for (int i = 0; i < 16; i++) {
        uint32_t cfg = (c->pmpcfg[i / 4] >> (8 * (i % 4))) & 0xFFu;
        uint32_t a = (cfg >> 3) & 3u;
        if (a == 0) continue;

        pmp_entry_t e;
        memset(&e, 0, sizeof(e));
        e.perm = cfg & 0x7u;

        if (a == 1) {
            if (i > 0) e.base = c->pmpaddr[i - 1] << 2;
            e.top = c->pmpaddr[i] << 2;
        } else if (a == 2) {
            e.base = c->pmpaddr[i] << 2;
            e.top = e.base + 4;
        } else {
            uint32_t v = c->pmpaddr[i];
            int t = 0;
            while (t < 32 && ((v >> t) & 1u)) t++;
            if (t == 32) {
                e.all = true;
            } else if (t > 0) {
                e.base = (v & ~((1u << t) - 1u)) << 2;
                e.top = e.base + (1u << (t + 3));
            }
        }
        if (!e.all && e.top <= e.base) continue;
        if (c->pmp_n < 16) c->pmp_ent[c->pmp_n++] = e;
    }
}

static bool pmp_check(cpu_t *c, uint32_t addr, uint32_t size, uint32_t access) {
    if (!c->pmp_enforce || c->priv != 0) return true;
    if (c->pmp_dirty) pmp_rebuild(c);
    for (int i = 0; i < c->pmp_n; i++) {
        pmp_entry_t *e = &c->pmp_ent[i];
        if (!e->all &&
            !(addr >= e->base && addr + size > addr && addr + size <= e->top)) {
            continue;
        }
        return (access & e->perm) == access;
    }
    return false;
}

/* ---- memory --------------------------------------------------------- */

static inline bool bad_align(uint32_t addr, uint32_t size) {
    if (size == 2) return (addr & 1u) != 0;
    if (size == 4) return (addr & 3u) != 0;
    return false;
}

static uint32_t cpu_load(cpu_t *c, uint32_t addr, uint32_t size, bool sign) {
    if (bad_align(addr, size)) {
        enter_trap(c, CAUSE_LOAD_MISALIGN, addr, c->cur_pc);
        return 0;
    }
    if (!pmp_check(c, addr, size, PMP_READ)) {
        enter_trap(c, CAUSE_LOAD_FAULT, addr, c->cur_pc);
        return 0;
    }

    uint32_t v;
    if (c->flat_size >= size &&
        (uint32_t)(addr - c->flat_base) < c->flat_size - size + 1) {
        uint32_t off = addr - c->flat_base;
        const uint8_t *m = c->flat;
        switch (size) {
        case 1: v = (uint32_t)m[off]; break;
        case 2: v = (uint32_t)m[off] | ((uint32_t)m[off + 1] << 8); break;
        default:
            v = (uint32_t)m[off] | ((uint32_t)m[off + 1] << 8) |
                ((uint32_t)m[off + 2] << 16) | ((uint32_t)m[off + 3] << 24);
            break;
        }
    } else {
        v = bus_fast_read(c->bus, addr, size);
    }
    if (sign) {
        if (size == 1) v = (uint32_t)(int32_t)(int8_t)v;
        else if (size == 2) v = (uint32_t)(int32_t)(int16_t)v;
    }
    return v;
}

static void cpu_store(cpu_t *c, uint32_t addr, uint32_t size, uint32_t val) {
    if (bad_align(addr, size)) {
        enter_trap(c, CAUSE_STORE_MISALIGN, addr, c->cur_pc);
        return;
    }
    if (!pmp_check(c, addr, size, PMP_WRITE)) {
        enter_trap(c, CAUSE_STORE_FAULT, addr, c->cur_pc);
        return;
    }
    if (c->flat_size >= size &&
        (uint32_t)(addr - c->flat_base) < c->flat_size - size + 1) {
        uint32_t off = addr - c->flat_base;
        uint8_t *m = c->flat;
        switch (size) {
        case 1: m[off] = (uint8_t)val; break;
        case 2:
            m[off] = (uint8_t)val;
            m[off + 1] = (uint8_t)(val >> 8);
            break;
        default:
            m[off] = (uint8_t)val;
            m[off + 1] = (uint8_t)(val >> 8);
            m[off + 2] = (uint8_t)(val >> 16);
            m[off + 3] = (uint8_t)(val >> 24);
            break;
        }
        /* Keep decoded blocks coherent for self-modifying code. */
        bus_touch_code(c->bus, addr);
        return;
    }
    bus_fast_write(c->bus, addr, size, val);
}

/* ---- M extension ---------------------------------------------------- */

void cpu_exec_m(cpu_t *c, uint32_t f3, int rd, uint32_t a, uint32_t b) {
    switch (f3) {
    case 0: set_rd(c, rd, a * b); break;
    case 1: {
        int64_t p = (int64_t)(int32_t)a * (int64_t)(int32_t)b;
        set_rd(c, rd, (uint32_t)((uint64_t)p >> 32));
        break;
    }
    case 2: {
        int64_t p = (int64_t)(int32_t)a * (int64_t)(uint64_t)b;
        set_rd(c, rd, (uint32_t)((uint64_t)p >> 32));
        break;
    }
    case 3: {
        uint64_t p = (uint64_t)a * (uint64_t)b;
        set_rd(c, rd, (uint32_t)(p >> 32));
        break;
    }
    case 4:
        if (b == 0) set_rd(c, rd, 0xFFFFFFFFu);
        else if ((int32_t)a == INT32_MIN && (int32_t)b == -1) set_rd(c, rd, a);
        else set_rd(c, rd, (uint32_t)((int32_t)a / (int32_t)b));
        break;
    case 5:
        if (b == 0) set_rd(c, rd, 0xFFFFFFFFu);
        else set_rd(c, rd, a / b);
        break;
    case 6:
        if (b == 0) set_rd(c, rd, a);
        else if ((int32_t)a == INT32_MIN && (int32_t)b == -1) set_rd(c, rd, 0);
        else set_rd(c, rd, (uint32_t)((int32_t)a % (int32_t)b));
        break;
    case 7:
        if (b == 0) set_rd(c, rd, a);
        else set_rd(c, rd, a % b);
        break;
    default:
        break;
    }
}

/* ---- SYSTEM --------------------------------------------------------- */

static void exec_system(cpu_t *c, uint32_t instr) {
    uint32_t f3 = (instr >> 12) & 0x7u;
    int rd = (int)((instr >> 7) & 0x1Fu);
    int r1 = (int)((instr >> 15) & 0x1Fu);
    uint32_t csr = (instr >> 20) & 0xFFFu;

    switch (f3) {
    case 0:
        switch (csr) {
        case 0x0:
            enter_trap(c, c->priv == 0 ? CAUSE_ECALL_U : CAUSE_ECALL_M, 0,
                       c->cur_pc);
            break;
        case 0x1:
            c->halted = true;
            snprintf(c->halt_msg, sizeof(c->halt_msg), "EBREAK");
            break;
        case 0x302:
            mret(c);
            break;
        case 0x105:
            break;
        default:
            illegal(c, instr);
            break;
        }
        break;
    case 1: {
        uint32_t v;
        if (!cpu_csr_read(c, csr, &v)) { illegal(c, instr); return; }
        set_rd(c, rd, v);
        cpu_csr_write(c, csr, c->x[r1]);
        break;
    }
    case 2: {
        uint32_t v;
        if (!cpu_csr_read(c, csr, &v)) { illegal(c, instr); return; }
        set_rd(c, rd, v);
        cpu_csr_write(c, csr, v | c->x[r1]);
        break;
    }
    case 3: {
        uint32_t v;
        if (!cpu_csr_read(c, csr, &v)) { illegal(c, instr); return; }
        set_rd(c, rd, v);
        cpu_csr_write(c, csr, v & ~c->x[r1]);
        break;
    }
    case 5: {
        uint32_t v;
        if (!cpu_csr_read(c, csr, &v)) { illegal(c, instr); return; }
        set_rd(c, rd, v);
        cpu_csr_write(c, csr, (uint32_t)r1);
        break;
    }
    case 6: {
        uint32_t v;
        if (!cpu_csr_read(c, csr, &v)) { illegal(c, instr); return; }
        set_rd(c, rd, v);
        cpu_csr_write(c, csr, v | (uint32_t)r1);
        break;
    }
    case 7: {
        uint32_t v;
        if (!cpu_csr_read(c, csr, &v)) { illegal(c, instr); return; }
        set_rd(c, rd, v);
        cpu_csr_write(c, csr, v & ~(uint32_t)r1);
        break;
    }
    default:
        illegal(c, instr);
        break;
    }
}

/* ---- 32-bit execute ------------------------------------------------- */

static void exec_i(cpu_t *c, uint32_t instr, uint32_t cur_pc) {
    uint32_t op = instr & 0x7Fu;
    int rd = (int)((instr >> 7) & 0x1Fu);
    uint32_t f3 = (instr >> 12) & 0x7u;
    int r1 = (int)((instr >> 15) & 0x1Fu);
    int r2 = (int)((instr >> 20) & 0x1Fu);
    uint32_t f7 = instr >> 25;

    switch (op) {
    case 0x37:
        set_rd(c, rd, imm_u(instr));
        break;
    case 0x17:
        set_rd(c, rd, cur_pc + imm_u(instr));
        break;
    case 0x6F:
        set_rd(c, rd, cur_pc + 4);
        c->pc = cur_pc + imm_j(instr);
        break;
    case 0x67: {
        uint32_t t = (cpu_reg(c, r1) + imm_i(instr)) & ~1u;
        set_rd(c, rd, cur_pc + 4);
        c->pc = t;
        break;
    }
    case 0x63: {
        bool taken = false;
        uint32_t a = cpu_reg(c, r1), b = cpu_reg(c, r2);
        switch (f3) {
        case 0: taken = (a == b); break;
        case 1: taken = (a != b); break;
        case 4: taken = (int32_t)a < (int32_t)b; break;
        case 5: taken = (int32_t)a >= (int32_t)b; break;
        case 6: taken = (a < b); break;
        case 7: taken = (a >= b); break;
        default: break;
        }
        if (taken) c->pc = cur_pc + imm_b(instr);
        break;
    }
    case 0x03: {
        uint32_t addr = cpu_reg(c, r1) + imm_i(instr);
        switch (f3) {
        case 0: set_rd(c, rd, cpu_load(c, addr, 1, true)); break;
        case 1: set_rd(c, rd, cpu_load(c, addr, 2, true)); break;
        case 2: set_rd(c, rd, cpu_load(c, addr, 4, false)); break;
        case 4: set_rd(c, rd, cpu_load(c, addr, 1, false)); break;
        case 5: set_rd(c, rd, cpu_load(c, addr, 2, false)); break;
        default: illegal(c, instr); break;
        }
        break;
    }
    case 0x23: {
        uint32_t addr = cpu_reg(c, r1) + imm_s(instr);
        uint32_t v = cpu_reg(c, r2);
        switch (f3) {
        case 0: cpu_store(c, addr, 1, v); break;
        case 1: cpu_store(c, addr, 2, v); break;
        case 2: cpu_store(c, addr, 4, v); break;
        default: illegal(c, instr); break;
        }
        break;
    }
    case 0x13: {
        uint32_t a = cpu_reg(c, r1);
        uint32_t imm = imm_i(instr);
        switch (f3) {
        case 0: set_rd(c, rd, a + imm); break;
        case 2: set_rd(c, rd, bool_u32((int32_t)a < (int32_t)imm)); break;
        case 3: set_rd(c, rd, bool_u32(a < imm)); break;
        case 4: set_rd(c, rd, a ^ imm); break;
        case 6: set_rd(c, rd, a | imm); break;
        case 7: set_rd(c, rd, a & imm); break;
        case 1: set_rd(c, rd, a << (imm & 0x1Fu)); break;
        case 5:
            if (imm & 0x400u) set_rd(c, rd, (uint32_t)((int32_t)a >> (imm & 0x1Fu)));
            else set_rd(c, rd, a >> (imm & 0x1Fu));
            break;
        default: break;
        }
        break;
    }
    case 0x33: {
        uint32_t a = cpu_reg(c, r1), b = cpu_reg(c, r2);
        if (f7 == 0x01) {
            cpu_exec_m(c, f3, rd, a, b);
            break;
        }
        switch (f3) {
        case 0:
            if (f7 & 0x20u) set_rd(c, rd, a - b);
            else set_rd(c, rd, a + b);
            break;
        case 1: set_rd(c, rd, a << (b & 0x1Fu)); break;
        case 2: set_rd(c, rd, bool_u32((int32_t)a < (int32_t)b)); break;
        case 3: set_rd(c, rd, bool_u32(a < b)); break;
        case 4: set_rd(c, rd, a ^ b); break;
        case 5:
            if (f7 & 0x20u) set_rd(c, rd, (uint32_t)((int32_t)a >> (b & 0x1Fu)));
            else set_rd(c, rd, a >> (b & 0x1Fu));
            break;
        case 6: set_rd(c, rd, a | b); break;
        case 7: set_rd(c, rd, a & b); break;
        default: break;
        }
        break;
    }
    case 0x0F:
        break;
    case 0x73:
        exec_system(c, instr);
        break;
    default:
        illegal(c, instr);
        break;
    }
}

/* ---- compressed execute --------------------------------------------- */

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
    return sign_extend(imm, 10);
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
    return sign_extend(off & 0xFFFu, 12);
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
    return sign_extend(off & 0x1FFu, 9);
}

static void exec_c(cpu_t *c, uint16_t in) {
    uint32_t inst = in;
    uint32_t op = inst & 0x3u;
    uint32_t f3 = inst >> 13;

    switch (op) {
    case 0:
        switch (f3) {
        case 0: {
            int rd = c_reg((inst >> 2) & 0x7u);
            uint32_t imm = c_addi4spn_imm(inst);
            if (imm == 0) illegal(c, inst);
            else set_rd(c, rd, cpu_reg(c, 2) + imm);
            break;
        }
        case 2: {
            int rd = c_reg((inst >> 2) & 0x7u);
            int rs = c_reg((inst >> 7) & 0x7u);
            set_rd(c, rd, cpu_load(c, cpu_reg(c, rs) + c_lw_off(inst), 4, false));
            break;
        }
        case 6: {
            int rs2 = c_reg((inst >> 2) & 0x7u);
            int rs1 = c_reg((inst >> 7) & 0x7u);
            cpu_store(c, cpu_reg(c, rs1) + c_lw_off(inst), 4, cpu_reg(c, rs2));
            break;
        }
        default:
            illegal(c, inst);
            break;
        }
        break;
    case 1: {
        uint32_t rd5 = (inst >> 7) & 0x1Fu;
        int cd = c_reg((inst >> 7) & 0x7u);
        uint32_t imm5 = (inst >> 12) & 1u;
        uint32_t imm40 = (inst >> 2) & 0x1Fu;
        uint32_t simm = sign_extend((imm5 << 5) | imm40, 6);
        switch (f3) {
        case 0:
            set_rd(c, (int)rd5, cpu_reg(c, (int)rd5) + simm);
            break;
        case 1: {
            uint32_t off = c_j_offset(inst);
            set_rd(c, 1, c->pc);
            c->pc = c->cur_pc + off;
            break;
        }
        case 2:
            set_rd(c, (int)rd5, simm);
            break;
        case 3:
            if (rd5 == 2) set_rd(c, 2, cpu_reg(c, 2) + c_addi16sp_imm(inst));
            else set_rd(c, (int)rd5, simm << 12);
            break;
        case 4: {
            uint32_t sub = (inst >> 10) & 0x3u;
            uint32_t shamt = imm40;
            switch (sub) {
            case 0:
                set_rd(c, cd, cpu_reg(c, cd) >> shamt);
                break;
            case 1:
                set_rd(c, cd, (uint32_t)((int32_t)cpu_reg(c, cd) >> shamt));
                break;
            case 2:
                set_rd(c, cd, cpu_reg(c, cd) & simm);
                break;
            default: {
                int rs2 = c_reg((inst >> 2) & 0x7u);
                switch ((inst >> 5) & 0x3u) {
                case 0: set_rd(c, cd, cpu_reg(c, cd) - cpu_reg(c, rs2)); break;
                case 1: set_rd(c, cd, cpu_reg(c, cd) ^ cpu_reg(c, rs2)); break;
                case 2: set_rd(c, cd, cpu_reg(c, cd) | cpu_reg(c, rs2)); break;
                case 3: set_rd(c, cd, cpu_reg(c, cd) & cpu_reg(c, rs2)); break;
                }
                break;
            }
            }
            break;
        }
        case 5:
            c->pc = c->cur_pc + c_j_offset(inst);
            break;
        case 6: {
            int rs1 = c_reg((inst >> 7) & 0x7u);
            if (cpu_reg(c, rs1) == 0) c->pc = c->cur_pc + c_b_offset(inst);
            break;
        }
        case 7: {
            int rs1 = c_reg((inst >> 7) & 0x7u);
            if (cpu_reg(c, rs1) != 0) c->pc = c->cur_pc + c_b_offset(inst);
            break;
        }
        default:
            illegal(c, inst);
            break;
        }
        break;
    }
    case 2: {
        uint32_t rd = (inst >> 7) & 0x1Fu;
        uint32_t shamt = (inst >> 2) & 0x1Fu;
        switch (f3) {
        case 0:
            set_rd(c, (int)rd, cpu_reg(c, (int)rd) << (shamt & 0x1Fu));
            break;
        case 2:
            set_rd(c, (int)rd, cpu_load(c, cpu_reg(c, 2) + c_lwsp_off(inst), 4,
                                        false));
            break;
        case 6: {
            uint32_t rs2 = (inst >> 2) & 0x1Fu;
            cpu_store(c, cpu_reg(c, 2) + c_swsp_off(inst), 4, cpu_reg(c, (int)rs2));
            break;
        }
        case 4: {
            uint32_t b12 = (inst >> 12) & 1u;
            uint32_t rs2 = (inst >> 2) & 0x1Fu;
            if (b12 == 0) {
                if (rs2 == 0) c->pc = cpu_reg(c, (int)rd) & ~1u;
                else set_rd(c, (int)rd, cpu_reg(c, (int)rs2));
            } else {
                if (rd == 0 && rs2 == 0) {
                    c->halted = true;
                    snprintf(c->halt_msg, sizeof(c->halt_msg), "C.EBREAK");
                } else if (rs2 == 0) {
                    set_rd(c, 1, c->pc);
                    c->pc = cpu_reg(c, (int)rd) & ~1u;
                } else {
                    set_rd(c, (int)rd, cpu_reg(c, (int)rd) + cpu_reg(c, (int)rs2));
                }
            }
            break;
        }
        default:
            illegal(c, inst);
            break;
        }
        break;
    }
    default:
        illegal(c, inst);
        break;
    }
}

/* ---- fetch / step --------------------------------------------------- */

static uint32_t fetch16(cpu_t *c, uint32_t pc) {
    uint32_t off = pc - c->flat_base;
    if (off + 1 < c->flat_size) {
        return (uint32_t)c->flat[off] | ((uint32_t)c->flat[off + 1] << 8);
    }
    return bus_read(c->bus, pc, 2);
}

bool cpu_step(cpu_t *c) {
    if (c->halted) return false;
    if (c->pending_int != 0) check_interrupts(c);

    uint32_t cur_pc = c->pc;
    c->cur_pc = cur_pc;
    uint32_t low = fetch16(c, c->pc);
    uint32_t instr;
    bool compressed;

    switch (low & 0x3u) {
    case 0x0:
    case 0x1:
    case 0x2:
        instr = low;
        compressed = true;
        c->pc += 2;
        break;
    default:
        instr = low | (fetch16(c, c->pc + 2) << 16);
        compressed = false;
        c->pc += 4;
        break;
    }

    if (compressed) exec_c(c, (uint16_t)instr);
    else exec_i(c, instr, cur_pc);

    c->csr_mcycle++;
    c->csr_minstret++;
    c->inst_count++;
    return !c->halted;
}

/* ---- register dump -------------------------------------------------- */

void cpu_dump(cpu_t *c, char *buf, size_t cap) {
    size_t n = 0;
    for (int i = 0; i < 32; i++) {
        n += (size_t)snprintf(buf + n, cap - n, "x%-2d = 0x%08X   ",
                              i, cpu_reg(c, i));
        if (i % 4 == 3) n += (size_t)snprintf(buf + n, cap - n, "\n");
        if (n >= cap) return;
    }
    snprintf(buf + n, cap - n,
             "pc = 0x%08X  mstatus=0x%08X  mtvec=0x%08X  mepc=0x%08X  "
             "mcause=0x%08X  mtval=0x%08X\n",
             c->pc, c->csr_mstatus, c->csr_mtvec, c->csr_mepc,
             c->csr_mcause, c->csr_mtval);
}
