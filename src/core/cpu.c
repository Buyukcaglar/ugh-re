/* cpu.c - an 80286 real-mode (8086 + 80186 instruction set) interpreter.
 *
 * It runs the parts of the program that the recompiler has not translated, the interrupt handlers'
 * entry, and serves as the reference the recompiled code is tested against.  80286 behaviour where
 * the processors differ: shift counts are masked to 5 bits, unknown opcodes raise interrupt 6 with
 * IP at the opcode (the game's CPU check at 0020:1C0C relies on both), PUSH SP pushes the old SP,
 * a divide error leaves IP at the instruction. */
#include "cpu.h"
#include "../hal/hal.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

uint8_t g_mem[MEM_SIZE];
Cpu g_cpu;

const uint8_t g_parity[256] = {
#define P2(n) n, n ^ 4, n ^ 4, n
#define P4(n) P2(n), P2(n ^ 4), P2(n ^ 4), P2(n)
#define P6(n) P4(n), P4(n ^ 4), P4(n ^ 4), P4(n)
    P6(4), P6(0), P6(0), P6(4)
#undef P2
#undef P4
#undef P6
};

void cpu_fatal(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    hal_fatal(buf);
}

/* ---- flags ---- */
#define SETF(c, f, on) ((c)->flags = (uint16_t)((on) ? ((c)->flags | (f)) : ((c)->flags & ~(f))))

static inline void szp8(Cpu *c, uint8_t r)
{
    c->flags = (uint16_t)((c->flags & ~(F_ZF | F_SF | F_PF)) | (r == 0 ? F_ZF : 0) | (r & 0x80u) |
                          g_parity[r]);
}
static inline void szp16(Cpu *c, uint16_t r)
{
    c->flags = (uint16_t)((c->flags & ~(F_ZF | F_SF | F_PF)) | (r == 0 ? F_ZF : 0) | ((r >> 8) & 0x80u) |
                          g_parity[r & 0xFFu]);
}
static inline void szp(Cpu *c, uint32_t r, int w)
{
    if (w) szp16(c, (uint16_t)r); else szp8(c, (uint8_t)r);
}

/* op: 0 ADD 1 OR 2 ADC 3 SBB 4 AND 5 SUB 6 XOR 7 CMP; returns the result (CMP: unchanged a) */
uint32_t cpu_alu(Cpu *c, int op, uint32_t a, uint32_t b, int w)
{
    uint32_t mask = w ? 0xFFFFu : 0xFFu, sign = w ? 0x8000u : 0x80u, r, cf;
    switch (op) {
    case 0: case 2:
        cf = (op == 2) ? (c->flags & F_CF) : 0u;
        r = a + b + cf;
        c->flags &= (uint16_t)~(F_CF | F_AF | F_OF);
        if (r > mask) c->flags |= F_CF;
        if ((a ^ b ^ r) & 0x10u) c->flags |= F_AF;
        if ((a ^ r) & (b ^ r) & sign) c->flags |= F_OF;
        r &= mask;
        szp(c, r, w);
        return r;
    case 3: case 5: case 7:
        cf = (op == 3) ? (c->flags & F_CF) : 0u;
        r = a - b - cf;
        c->flags &= (uint16_t)~(F_CF | F_AF | F_OF);
        if (r & ~mask) c->flags |= F_CF;
        if ((a ^ b ^ r) & 0x10u) c->flags |= F_AF;
        if ((a ^ b) & (a ^ r) & sign) c->flags |= F_OF;
        r &= mask;
        szp(c, r, w);
        return op == 7 ? a : r;
    default:
        r = op == 1 ? (a | b) : op == 4 ? (a & b) : (a ^ b);
        c->flags &= (uint16_t)~(F_CF | F_AF | F_OF);
        szp(c, r, w);
        return r;
    }
}

uint32_t cpu_incdec(Cpu *c, uint32_t a, int dec, int w)
{
    uint32_t mask = w ? 0xFFFFu : 0xFFu, sign = w ? 0x8000u : 0x80u;
    uint32_t r = (dec ? a - 1u : a + 1u) & mask;
    c->flags &= (uint16_t)~(F_AF | F_OF);
    if ((a ^ 1u ^ r) & 0x10u) c->flags |= F_AF;
    if (dec ? (a == sign) : (r == sign)) c->flags |= F_OF;
    szp(c, r, w);
    return r;
}

/* op: 0 ROL 1 ROR 2 RCL 3 RCR 4 SHL 5 SHR 6 SHL(SAL) 7 SAR; count already read */
uint32_t cpu_shift(Cpu *c, int op, uint32_t a, unsigned n, int w)
{
    uint32_t mask = w ? 0xFFFFu : 0xFFu, sign = w ? 0x8000u : 0x80u;
    unsigned bits = w ? 16u : 8u, i;
    uint32_t r = a & mask, cf = c->flags & F_CF;
    n &= 0x1Fu;
    if (n == 0) return r;
    switch (op) {
    case 0:
        for (i = 0; i < n; i++) { cf = (r & sign) ? 1u : 0u; r = ((r << 1) | cf) & mask; }
        SETF(c, F_CF, cf);
        SETF(c, F_OF, ((r & sign) ? 1u : 0u) ^ cf);
        return r;
    case 1:
        for (i = 0; i < n; i++) { cf = r & 1u; r = (r >> 1) | (cf ? sign : 0u); }
        SETF(c, F_CF, cf);
        SETF(c, F_OF, ((r ^ (r << 1)) & sign) != 0);
        return r;
    case 2:
        for (i = 0; i < n; i++) { uint32_t o = (r & sign) ? 1u : 0u; r = ((r << 1) | cf) & mask; cf = o; }
        SETF(c, F_CF, cf);
        SETF(c, F_OF, ((r & sign) ? 1u : 0u) ^ cf);
        return r;
    case 3:
        for (i = 0; i < n; i++) { uint32_t o = r & 1u; r = (r >> 1) | (cf ? sign : 0u); cf = o; }
        SETF(c, F_CF, cf);
        SETF(c, F_OF, ((r ^ (r << 1)) & sign) != 0);
        return r;
    case 4: case 6:
        cf = n <= bits ? ((r << (n - 1)) & sign) != 0 : 0u;
        r = n < 32u ? (r << n) & mask : 0u;
        SETF(c, F_CF, cf);
        SETF(c, F_OF, ((r & sign) != 0) ^ (cf != 0));
        c->flags &= (uint16_t)~F_AF;
        szp(c, r, w);
        return r;
    case 5:
        cf = n <= bits ? (r >> (n - 1)) & 1u : 0u;
        SETF(c, F_OF, (r & sign) != 0);
        r = n < 32u ? r >> n : 0u;
        SETF(c, F_CF, cf);
        c->flags &= (uint16_t)~F_AF;
        szp(c, r, w);
        return r;
    default: {
        int32_t s = w ? (int32_t)(int16_t)r : (int32_t)(int8_t)r;
        unsigned k = n > bits ? bits : n;
        cf = (uint32_t)(s >> (k - 1)) & 1u;
        s >>= k;
        r = (uint32_t)s & mask;
        SETF(c, F_CF, cf);
        c->flags &= (uint16_t)~(F_OF | F_AF);
        szp(c, r, w);
        return r;
    }
    }
}

void cpu_interrupt(Cpu *c, uint8_t n)
{
    push16(c, c->flags);
    push16(c, c->s[CS]);
    push16(c, c->ip);
    c->flags &= (uint16_t)~(F_IF | F_TF);
    c->ip = rw((uint32_t)n * 4u);
    c->s[CS] = rw((uint32_t)n * 4u + 2u);
    c->halted = 0;
}

void cpu_reset(Cpu *c)
{
    memset(c, 0, sizeof *c);
    c->flags = 0x0002u;
}

/* ---- decoding ---- */
typedef struct Dec {
    Cpu *c;
    int seg;          /* segment override or -1 */
    int rep;          /* 0, 0xF2, 0xF3 */
    uint8_t modrm;
    int mod, reg, rm;
    uint16_t ea_seg, ea_off;
} Dec;

static inline uint8_t fetch8(Cpu *c)
{
    uint8_t v = rbs(c->s[CS], c->ip);
    c->ip++;
    return v;
}
static inline uint16_t fetch16(Cpu *c)
{
    uint16_t v = rws(c->s[CS], c->ip);
    c->ip = (uint16_t)(c->ip + 2u);
    return v;
}

static void modrm(Dec *d)
{
    Cpu *c = d->c;
    uint16_t off = 0;
    int defss = 0;
    d->modrm = fetch8(c);
    d->mod = d->modrm >> 6;
    d->reg = (d->modrm >> 3) & 7;
    d->rm = d->modrm & 7;
    if (d->mod == 3) return;
    switch (d->rm) {
    case 0: off = (uint16_t)(c->r[BX] + c->r[SI]); break;
    case 1: off = (uint16_t)(c->r[BX] + c->r[DI]); break;
    case 2: off = (uint16_t)(c->r[BP] + c->r[SI]); defss = 1; break;
    case 3: off = (uint16_t)(c->r[BP] + c->r[DI]); defss = 1; break;
    case 4: off = c->r[SI]; break;
    case 5: off = c->r[DI]; break;
    case 6:
        if (d->mod == 0) off = fetch16(c);
        else { off = c->r[BP]; defss = 1; }
        break;
    default: off = c->r[BX]; break;
    }
    if (d->mod == 1) off = (uint16_t)(off + (uint16_t)(int16_t)(int8_t)fetch8(c));
    else if (d->mod == 2) off = (uint16_t)(off + fetch16(c));
    d->ea_off = off;
    d->ea_seg = c->s[d->seg >= 0 ? d->seg : (defss ? SS : DS)];
}

static inline uint32_t rm_get(Dec *d, int w)
{
    if (d->mod == 3) return w ? d->c->r[d->rm] : r8get(d->c, d->rm);
    return w ? rws(d->ea_seg, d->ea_off) : rbs(d->ea_seg, d->ea_off);
}
static inline void rm_set(Dec *d, int w, uint32_t v)
{
    if (d->mod == 3) {
        if (w) d->c->r[d->rm] = (uint16_t)v; else r8set(d->c, d->rm, (uint8_t)v);
        return;
    }
    if (w) wws(d->ea_seg, d->ea_off, (uint16_t)v); else wbs(d->ea_seg, d->ea_off, (uint8_t)v);
}
static inline uint32_t reg_get(Dec *d, int w)
{
    return w ? d->c->r[d->reg] : r8get(d->c, d->reg);
}
static inline void reg_set(Dec *d, int w, uint32_t v)
{
    if (w) d->c->r[d->reg] = (uint16_t)v; else r8set(d->c, d->reg, (uint8_t)v);
}

int cpu_cond(const Cpu *c, int cc)
{
    uint16_t f = c->flags;
    int r;
    switch (cc >> 1) {
    case 0: r = (f & F_OF) != 0; break;
    case 1: r = (f & F_CF) != 0; break;
    case 2: r = (f & F_ZF) != 0; break;
    case 3: r = (f & (F_CF | F_ZF)) != 0; break;
    case 4: r = (f & F_SF) != 0; break;
    case 5: r = (f & F_PF) != 0; break;
    case 6: r = ((f & F_SF) != 0) != ((f & F_OF) != 0); break;
    default: r = ((f & F_ZF) != 0) || (((f & F_SF) != 0) != ((f & F_OF) != 0)); break;
    }
    return (cc & 1) ? !r : r;
}

static void invalid(Cpu *c)
{
    c->ip = c->op_ip;
    c->s[CS] = c->op_cs;
    cpu_interrupt(c, 6);
}

void cpu_mul(Cpu *c, int op, uint32_t v, int w)   /* op 4 MUL 5 IMUL 6 DIV 7 IDIV; returns 0 / raises int 0 */
{
    if (!w) {
        switch (op) {
        case 4: {
            uint16_t r = (uint16_t)((c->r[AX] & 0xFFu) * v);
            c->r[AX] = r;
            SETF(c, F_CF | F_OF, (r & 0xFF00u) != 0);
            szp8(c, (uint8_t)r);
            return;
        }
        case 5: {
            int16_t r = (int16_t)((int8_t)c->r[AX] * (int8_t)v);
            c->r[AX] = (uint16_t)r;
            SETF(c, F_CF | F_OF, r != (int8_t)r);
            szp8(c, (uint8_t)r);
            return;
        }
        case 6: {
            uint16_t a = c->r[AX], q;
            if (v == 0 || (q = (uint16_t)(a / v)) > 0xFFu) { c->ip = c->op_ip; cpu_interrupt(c, 0); return; }
            c->r[AX] = (uint16_t)(((a % v) << 8) | q);
            return;
        }
        default: {
            int16_t a = (int16_t)c->r[AX];
            int8_t b = (int8_t)v;
            int q;
            if (b == 0 || (q = a / b) > 127 || q < -128) { c->ip = c->op_ip; cpu_interrupt(c, 0); return; }
            c->r[AX] = (uint16_t)((((uint8_t)(a % b)) << 8) | (uint8_t)q);
            return;
        }
        }
    }
    switch (op) {
    case 4: {
        uint32_t r = (uint32_t)c->r[AX] * v;
        c->r[AX] = (uint16_t)r;
        c->r[DX] = (uint16_t)(r >> 16);
        SETF(c, F_CF | F_OF, c->r[DX] != 0);
        szp16(c, c->r[AX]);
        return;
    }
    case 5: {
        int32_t r = (int32_t)(int16_t)c->r[AX] * (int32_t)(int16_t)v;
        c->r[AX] = (uint16_t)r;
        c->r[DX] = (uint16_t)((uint32_t)r >> 16);
        SETF(c, F_CF | F_OF, r != (int16_t)r);
        szp16(c, c->r[AX]);
        return;
    }
    case 6: {
        uint32_t a = ((uint32_t)c->r[DX] << 16) | c->r[AX], q;
        if (v == 0 || (q = a / v) > 0xFFFFu) { c->ip = c->op_ip; cpu_interrupt(c, 0); return; }
        c->r[AX] = (uint16_t)q;
        c->r[DX] = (uint16_t)(a % v);
        return;
    }
    default: {
        int32_t a = (int32_t)(((uint32_t)c->r[DX] << 16) | c->r[AX]);
        int32_t b = (int16_t)v;
        int32_t q;
        if (b == 0 || (a == INT32_MIN && b == -1) || (q = a / b) > 32767 || q < -32768) {
            c->ip = c->op_ip;
            cpu_interrupt(c, 0);
            return;
        }
        c->r[AX] = (uint16_t)q;
        c->r[DX] = (uint16_t)(a % b);
        return;
    }
    }
}

/* string instructions; returns after the whole REP */
static void string_op(Dec *d, uint8_t op)
{
    Cpu *c = d->c;
    cpu_string(c, op, d->seg, d->rep);
}

void cpu_string(Cpu *c, uint8_t op, int seg, int rep)
{
    struct { int seg, rep; } dd, *d = &dd;
    dd.seg = seg;
    dd.rep = rep;
    int w = op & 1;
    int16_t step = (int16_t)((c->flags & F_DF) ? -(w ? 2 : 1) : (w ? 2 : 1));
    uint16_t sseg = c->s[d->seg >= 0 ? d->seg : DS];
    for (;;) {
        if (d->rep && c->r[CX] == 0) break;
        switch (op) {
        case 0xA4: case 0xA5:
            if (w) wws(c->s[ES], c->r[DI], rws(sseg, c->r[SI]));
            else wbs(c->s[ES], c->r[DI], rbs(sseg, c->r[SI]));
            c->r[SI] = (uint16_t)(c->r[SI] + step);
            c->r[DI] = (uint16_t)(c->r[DI] + step);
            break;
        case 0xA6: case 0xA7: {
            uint32_t a = w ? rws(sseg, c->r[SI]) : rbs(sseg, c->r[SI]);
            uint32_t b = w ? rws(c->s[ES], c->r[DI]) : rbs(c->s[ES], c->r[DI]);
            (void)cpu_alu(c, 7, a, b, w);
            c->r[SI] = (uint16_t)(c->r[SI] + step);
            c->r[DI] = (uint16_t)(c->r[DI] + step);
            break;
        }
        case 0xAA: case 0xAB:
            if (w) wws(c->s[ES], c->r[DI], c->r[AX]); else wbs(c->s[ES], c->r[DI], (uint8_t)c->r[AX]);
            c->r[DI] = (uint16_t)(c->r[DI] + step);
            break;
        case 0xAC: case 0xAD:
            if (w) c->r[AX] = rws(sseg, c->r[SI]); else r8set(c, 0, rbs(sseg, c->r[SI]));
            c->r[SI] = (uint16_t)(c->r[SI] + step);
            break;
        case 0xAE: case 0xAF: {
            uint32_t b = w ? rws(c->s[ES], c->r[DI]) : rbs(c->s[ES], c->r[DI]);
            (void)cpu_alu(c, 7, w ? c->r[AX] : (c->r[AX] & 0xFFu), b, w);
            c->r[DI] = (uint16_t)(c->r[DI] + step);
            break;
        }
        case 0x6C: case 0x6D:
            wbs(c->s[ES], c->r[DI], io_in8(c->r[DX]));
            if (w) wbs(c->s[ES], (uint16_t)(c->r[DI] + 1), io_in8((uint16_t)(c->r[DX] + 1)));
            c->r[DI] = (uint16_t)(c->r[DI] + step);
            break;
        default: /* 6E 6F */
            io_out8(c->r[DX], rbs(sseg, c->r[SI]));
            if (w) io_out8((uint16_t)(c->r[DX] + 1), rbs(sseg, (uint16_t)(c->r[SI] + 1)));
            c->r[SI] = (uint16_t)(c->r[SI] + step);
            break;
        }
        if (!d->rep) break;
        c->r[CX]--;
        c->icount++;
        if ((op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF) &&
            (((c->flags & F_ZF) != 0) != (d->rep == 0xF3)))
            break;
    }
}

static void daa_das(Cpu *c, int sub)
{
    uint8_t al = (uint8_t)c->r[AX], old = al;
    int cf = (c->flags & F_CF) != 0;
    if ((al & 0x0F) > 9 || (c->flags & F_AF)) {
        al = (uint8_t)(sub ? al - 6 : al + 6);
        c->flags |= F_AF;
    } else c->flags &= (uint16_t)~F_AF;
    if (old > 0x99 || cf) {
        al = (uint8_t)(sub ? al - 0x60 : al + 0x60);
        c->flags |= F_CF;
    } else c->flags &= (uint16_t)~F_CF;
    r8set(c, 0, al);
    szp8(c, al);
}

static void aaa_aas(Cpu *c, int sub)
{
    if ((c->r[AX] & 0x0F) > 9 || (c->flags & F_AF)) {
        if (sub) { c->r[AX] = (uint16_t)(c->r[AX] - 6); r8set(c, 4, (uint8_t)(r8get(c, 4) - 1)); }
        else { c->r[AX] = (uint16_t)(c->r[AX] + 0x106); }
        c->flags |= F_AF | F_CF;
    } else c->flags &= (uint16_t)~(F_AF | F_CF);
    c->r[AX] &= 0xFF0Fu;
}

/* the last instructions' addresses (diagnostics: UGH_TRACE) */
uint32_t g_trace_ring[TRACE_RING];
unsigned g_trace_pos;
int g_trace_on;

uint16_t *g_cov;          /* coverage (tests): per linear address, the CS it ran under (0: never) */

/* one instruction */
static void step(Cpu *c)
{
    Dec d;
    uint8_t op;
    d.c = c;
    d.seg = -1;
    d.rep = 0;
    c->op_cs = c->s[CS];
    c->op_ip = c->ip;
    if (g_cov != NULL) g_cov[lin(c->s[CS], c->ip) & 0xFFFFFu] = c->s[CS];
    if (g_trace_on) g_trace_ring[g_trace_pos++ & (TRACE_RING - 1u)] = ((uint32_t)c->s[CS] << 16) | c->ip;
prefix:
    op = fetch8(c);
    switch (op) {
    case 0x26: d.seg = ES; goto prefix;
    case 0x2E: d.seg = CS; goto prefix;
    case 0x36: d.seg = SS; goto prefix;
    case 0x3E: d.seg = DS; goto prefix;
    case 0xF0: goto prefix;
    case 0xF2: case 0xF3: d.rep = op; goto prefix;
    default: break;
    }
    c->icount++;

    if (op < 0x40 && (op & 7) < 6) {             /* ALU forms 00-3D */
        int alu = op >> 3, w = op & 1;
        switch (op & 7) {
        case 0: case 1: {
            uint32_t r;
            modrm(&d);
            r = cpu_alu(c, alu, rm_get(&d, w), reg_get(&d, w), w);
            if (alu != 7) rm_set(&d, w, r);
            return;
        }
        case 2: case 3: {
            uint32_t r;
            modrm(&d);
            r = cpu_alu(c, alu, reg_get(&d, w), rm_get(&d, w), w);
            if (alu != 7) reg_set(&d, w, r);
            return;
        }
        case 4: {
            uint32_t r = cpu_alu(c, alu, c->r[AX] & 0xFFu, fetch8(c), 0);
            if (alu != 7) r8set(c, 0, (uint8_t)r);
            return;
        }
        default: {
            uint32_t r = cpu_alu(c, alu, c->r[AX], fetch16(c), 1);
            if (alu != 7) c->r[AX] = (uint16_t)r;
            return;
        }
        }
    }

    switch (op) {
    case 0x06: push16(c, c->s[ES]); return;
    case 0x07: c->s[ES] = pop16(c); return;
    case 0x0E: push16(c, c->s[CS]); return;
    case 0x16: push16(c, c->s[SS]); return;
    case 0x17: c->s[SS] = pop16(c); return;
    case 0x1E: push16(c, c->s[DS]); return;
    case 0x1F: c->s[DS] = pop16(c); return;
    case 0x27: daa_das(c, 0); return;
    case 0x2F: daa_das(c, 1); return;
    case 0x37: aaa_aas(c, 0); return;
    case 0x3F: aaa_aas(c, 1); return;
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        c->r[op & 7] = (uint16_t)cpu_incdec(c, c->r[op & 7], 0, 1);
        return;
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        c->r[op & 7] = (uint16_t)cpu_incdec(c, c->r[op & 7], 1, 1);
        return;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        push16(c, c->r[op & 7]);
        return;
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        c->r[op & 7] = pop16(c);
        return;
    case 0x60: {
        uint16_t sp = c->r[SP];
        int i;
        for (i = 0; i < 8; i++) push16(c, i == SP ? sp : c->r[i]);
        return;
    }
    case 0x61: {
        int i;
        for (i = 7; i >= 0; i--) {
            uint16_t v = pop16(c);
            if (i != SP) c->r[i] = v;
        }
        return;
    }
    case 0x62: {                                 /* BOUND */
        int16_t v, lo, hi;
        modrm(&d);
        v = (int16_t)c->r[d.reg];
        lo = (int16_t)rws(d.ea_seg, d.ea_off);
        hi = (int16_t)rws(d.ea_seg, (uint16_t)(d.ea_off + 2));
        if (v < lo || v > hi) { c->ip = c->op_ip; cpu_interrupt(c, 5); }
        return;
    }
    case 0x68: push16(c, fetch16(c)); return;
    case 0x6A: push16(c, (uint16_t)(int16_t)(int8_t)fetch8(c)); return;
    case 0x69: case 0x6B: {
        int32_t a, b, r;
        modrm(&d);
        a = (int16_t)rm_get(&d, 1);
        b = op == 0x69 ? (int16_t)fetch16(c) : (int8_t)fetch8(c);
        r = a * b;
        c->r[d.reg] = (uint16_t)r;
        SETF(c, F_CF | F_OF, r != (int16_t)r);
        return;
    }
    case 0x6C: case 0x6D: case 0x6E: case 0x6F:
        string_op(&d, op);
        return;
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        int8_t rel = (int8_t)fetch8(c);
        if (cpu_cond(c, op & 0x0F)) c->ip = (uint16_t)(c->ip + rel);
        return;
    }
    case 0x80: case 0x81: case 0x82: case 0x83: {
        int w = op & 1;
        uint32_t a, b, r;
        modrm(&d);
        a = rm_get(&d, w);
        b = op == 0x81 ? fetch16(c) : op == 0x83 ? (uint16_t)(int16_t)(int8_t)fetch8(c) : fetch8(c);
        r = cpu_alu(c, d.reg, a, b, w);
        if (d.reg != 7) rm_set(&d, w, r);
        return;
    }
    case 0x84: case 0x85: {
        int w = op & 1;
        modrm(&d);
        (void)cpu_alu(c, 4, rm_get(&d, w), reg_get(&d, w), w);
        return;
    }
    case 0x86: case 0x87: {
        int w = op & 1;
        uint32_t a, b;
        modrm(&d);
        a = rm_get(&d, w);
        b = reg_get(&d, w);
        rm_set(&d, w, b);
        reg_set(&d, w, a);
        return;
    }
    case 0x88: case 0x89: modrm(&d); rm_set(&d, op & 1, reg_get(&d, op & 1)); return;
    case 0x8A: case 0x8B: modrm(&d); reg_set(&d, op & 1, rm_get(&d, op & 1)); return;
    case 0x8C: modrm(&d); rm_set(&d, 1, c->s[d.reg & 3]); return;
    case 0x8D: modrm(&d); c->r[d.reg] = d.ea_off; return;
    case 0x8E: modrm(&d); c->s[d.reg & 3] = (uint16_t)rm_get(&d, 1); return;
    case 0x8F: {
        uint16_t v = pop16(c);
        modrm(&d);
        rm_set(&d, 1, v);
        return;
    }
    case 0x90: return;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
        uint16_t t = c->r[AX];
        c->r[AX] = c->r[op & 7];
        c->r[op & 7] = t;
        return;
    }
    case 0x98: c->r[AX] = (uint16_t)(int16_t)(int8_t)c->r[AX]; return;
    case 0x99: c->r[DX] = (c->r[AX] & 0x8000u) ? 0xFFFFu : 0u; return;
    case 0x9A: {
        uint16_t ip = fetch16(c), cs = fetch16(c);
        push16(c, c->s[CS]);
        push16(c, c->ip);
        c->s[CS] = cs;
        c->ip = ip;
        return;
    }
    case 0x9B: return;
    case 0x9C: push16(c, (uint16_t)((c->flags & 0x0FD5u) | 0x0002u)); return;   /* 286: bits 12-15 read 0 */
    case 0x9D: c->flags = (uint16_t)((pop16(c) & 0x0FD5u) | 0x0002u); c->irq_pending = 1; return;
    case 0x9E: c->flags = (uint16_t)((c->flags & 0xFF00u) | ((c->r[AX] >> 8) & 0xD5u) | 2u); return;
    case 0x9F: r8set(c, 4, (uint8_t)(c->flags | 2u)); return;
    case 0xA0: r8set(c, 0, rbs(c->s[d.seg >= 0 ? d.seg : DS], fetch16(c))); return;
    case 0xA1: c->r[AX] = rws(c->s[d.seg >= 0 ? d.seg : DS], fetch16(c)); return;
    case 0xA2: wbs(c->s[d.seg >= 0 ? d.seg : DS], fetch16(c), (uint8_t)c->r[AX]); return;
    case 0xA3: wws(c->s[d.seg >= 0 ? d.seg : DS], fetch16(c), c->r[AX]); return;
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        string_op(&d, op);
        return;
    case 0xA8: (void)cpu_alu(c, 4, c->r[AX] & 0xFFu, fetch8(c), 0); return;
    case 0xA9: (void)cpu_alu(c, 4, c->r[AX], fetch16(c), 1); return;
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        r8set(c, op & 7, fetch8(c));
        return;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        c->r[op & 7] = fetch16(c);
        return;
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        int w = op & 1;
        unsigned n;
        uint32_t r;
        modrm(&d);
        n = (op <= 0xC1) ? fetch8(c) : (op <= 0xD1) ? 1u : (unsigned)(c->r[CX] & 0xFFu);
        r = cpu_shift(c, d.reg, rm_get(&d, w), n, w);
        rm_set(&d, w, r);
        return;
    }
    case 0xC2: { uint16_t n = fetch16(c); c->ip = pop16(c); c->r[SP] = (uint16_t)(c->r[SP] + n); return; }
    case 0xC3: c->ip = pop16(c); return;
    case 0xC4: case 0xC5:
        modrm(&d);
        c->r[d.reg] = rws(d.ea_seg, d.ea_off);
        c->s[op == 0xC4 ? ES : DS] = rws(d.ea_seg, (uint16_t)(d.ea_off + 2));
        return;
    case 0xC6: modrm(&d); rm_set(&d, 0, fetch8(c)); return;
    case 0xC7: modrm(&d); rm_set(&d, 1, fetch16(c)); return;
    case 0xC8: {                                 /* ENTER */
        uint16_t size = fetch16(c);
        uint8_t level = (uint8_t)(fetch8(c) & 0x1F);
        uint16_t fp;
        push16(c, c->r[BP]);
        fp = c->r[SP];
        if (level) {
            uint8_t i;
            for (i = 1; i < level; i++) {
                c->r[BP] = (uint16_t)(c->r[BP] - 2u);
                push16(c, rws(c->s[SS], c->r[BP]));
            }
            push16(c, fp);
        }
        c->r[BP] = fp;
        c->r[SP] = (uint16_t)(c->r[SP] - size);
        return;
    }
    case 0xC9: c->r[SP] = c->r[BP]; c->r[BP] = pop16(c); return;
    case 0xCA: {
        uint16_t n = fetch16(c);
        c->ip = pop16(c);
        c->s[CS] = pop16(c);
        c->r[SP] = (uint16_t)(c->r[SP] + n);
        return;
    }
    case 0xCB: c->ip = pop16(c); c->s[CS] = pop16(c); return;
    case 0xCC: cpu_interrupt(c, 3); return;
    case 0xCD: cpu_interrupt(c, fetch8(c)); return;
    case 0xCE: if (c->flags & F_OF) cpu_interrupt(c, 4); return;
    case 0xCF:
        c->ip = pop16(c);
        c->s[CS] = pop16(c);
        c->flags = (uint16_t)((pop16(c) & 0x0FD5u) | 0x0002u);
        if (c->flags & F_IF) c->irq_pending = 1;
        return;
    case 0xD4: {
        uint8_t b = fetch8(c), al = (uint8_t)c->r[AX];
        if (b == 0) { cpu_interrupt(c, 0); return; }
        c->r[AX] = (uint16_t)(((al / b) << 8) | (al % b));
        szp8(c, (uint8_t)c->r[AX]);
        return;
    }
    case 0xD5: {
        uint8_t b = fetch8(c);
        uint8_t al = (uint8_t)((c->r[AX] & 0xFFu) + (c->r[AX] >> 8) * b);
        c->r[AX] = al;
        szp8(c, al);
        return;
    }
    case 0xD6: r8set(c, 0, (c->flags & F_CF) ? 0xFFu : 0u); return;
    case 0xD7:
        r8set(c, 0, rbs(c->s[d.seg >= 0 ? d.seg : DS], (uint16_t)(c->r[BX] + (c->r[AX] & 0xFFu))));
        return;
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        modrm(&d);                               /* no FPU */
        return;
    case 0xE0: case 0xE1: case 0xE2: {
        int8_t rel = (int8_t)fetch8(c);
        int z = (c->flags & F_ZF) != 0;
        c->r[CX]--;
        if (c->r[CX] != 0 && (op == 0xE2 || (op == 0xE1 ? z : !z))) c->ip = (uint16_t)(c->ip + rel);
        return;
    }
    case 0xE3: { int8_t rel = (int8_t)fetch8(c); if (c->r[CX] == 0) c->ip = (uint16_t)(c->ip + rel); return; }
    case 0xE4: r8set(c, 0, io_in8(fetch8(c))); return;
    case 0xE5: { uint8_t p = fetch8(c); c->r[AX] = (uint16_t)(io_in8(p) | (io_in8((uint16_t)(p + 1)) << 8)); return; }
    case 0xE6: io_out8(fetch8(c), (uint8_t)c->r[AX]); return;
    case 0xE7: { uint8_t p = fetch8(c); io_out8(p, (uint8_t)c->r[AX]); io_out8((uint16_t)(p + 1), (uint8_t)(c->r[AX] >> 8)); return; }
    case 0xE8: { uint16_t rel = fetch16(c); push16(c, c->ip); c->ip = (uint16_t)(c->ip + rel); return; }
    case 0xE9: { uint16_t rel = fetch16(c); c->ip = (uint16_t)(c->ip + rel); return; }
    case 0xEA: { uint16_t ip = fetch16(c), cs = fetch16(c); c->ip = ip; c->s[CS] = cs; return; }
    case 0xEB: { int8_t rel = (int8_t)fetch8(c); c->ip = (uint16_t)(c->ip + rel); return; }
    case 0xEC: r8set(c, 0, io_in8(c->r[DX])); return;
    case 0xED: c->r[AX] = (uint16_t)(io_in8(c->r[DX]) | (io_in8((uint16_t)(c->r[DX] + 1)) << 8)); return;
    case 0xEE: io_out8(c->r[DX], (uint8_t)c->r[AX]); return;
    case 0xEF: io_out8(c->r[DX], (uint8_t)c->r[AX]); io_out8((uint16_t)(c->r[DX] + 1), (uint8_t)(c->r[AX] >> 8)); return;
    case 0xF1:
        if (c->s[CS] == TRAP_SEG) { hal_trap(c, fetch8(c)); return; }
        invalid(c);
        return;
    case 0xF4: c->halted = 1; return;
    case 0xF5: c->flags ^= F_CF; return;
    case 0xF6: case 0xF7: {
        int w = op & 1;
        uint32_t v;
        modrm(&d);
        v = rm_get(&d, w);
        switch (d.reg) {
        case 0: case 1: (void)cpu_alu(c, 4, v, w ? fetch16(c) : fetch8(c), w); return;
        case 2: rm_set(&d, w, ~v); return;
        case 3: {
            uint32_t r = cpu_alu(c, 5, 0, v, w);
            rm_set(&d, w, r);
            SETF(c, F_CF, v != 0);
            return;
        }
        default: cpu_mul(c, d.reg, v, w); return;
        }
    }
    case 0xF8: c->flags &= (uint16_t)~F_CF; return;
    case 0xF9: c->flags |= F_CF; return;
    case 0xFA: c->flags &= (uint16_t)~F_IF; return;
    case 0xFB: c->flags |= F_IF; c->irq_pending = 1; return;
    case 0xFC: c->flags &= (uint16_t)~F_DF; return;
    case 0xFD: c->flags |= F_DF; return;
    case 0xFE: {
        modrm(&d);
        if (d.reg > 1) { invalid(c); return; }
        rm_set(&d, 0, cpu_incdec(c, rm_get(&d, 0), d.reg, 0));
        return;
    }
    case 0xFF: {
        uint16_t v;
        modrm(&d);
        switch (d.reg) {
        case 0: case 1: rm_set(&d, 1, cpu_incdec(c, rm_get(&d, 1), d.reg, 1)); return;
        case 2: v = (uint16_t)rm_get(&d, 1); push16(c, c->ip); c->ip = v; return;
        case 3: {
            uint16_t ip = rws(d.ea_seg, d.ea_off), cs = rws(d.ea_seg, (uint16_t)(d.ea_off + 2));
            push16(c, c->s[CS]);
            push16(c, c->ip);
            c->ip = ip;
            c->s[CS] = cs;
            return;
        }
        case 4: c->ip = (uint16_t)rm_get(&d, 1); return;
        case 5:
            c->ip = rws(d.ea_seg, d.ea_off);
            c->s[CS] = rws(d.ea_seg, (uint16_t)(d.ea_off + 2));
            return;
        case 6: push16(c, (uint16_t)rm_get(&d, 1)); return;
        default: invalid(c); return;
        }
    }
    default:
        invalid(c);
        return;
    }
}

/* the recompiled code's entry: returns 1 when it ran a routine at CS:IP */
int (*g_native_hook)(Cpu *c, uint64_t until);

void cpu_run(Cpu *c, uint64_t until)
{
    while (c->icount < until && !c->stop) {
        if (c->irq_pending) {
            c->irq_pending = 0;
            if (c->flags & F_IF) {
                int v = pic_ack();
                if (v >= 0) cpu_interrupt(c, (uint8_t)v);
            }
        }
        if (c->halted) {                         /* idle until the next event */
            c->icount = until;
            break;
        }
        if (g_native_hook != NULL && g_native_hook(c, until)) continue;
        step(c);
    }
}
