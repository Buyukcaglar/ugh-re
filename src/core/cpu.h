/* cpu.h - the 8086/80186 real-mode machine that runs UGH!'s program.
 *
 * The state is explicit (registers, flags, a flat 1 MB + 64 KB memory image), so the interpreter
 * (cpu.c) and the recompiled routines (src/gen) share it.  Memory below A0000h and above C0000h is
 * plain RAM; A0000h..BFFFFh goes through the VGA (hal/vga.c).  Ports and the trap interrupts go to
 * the HAL (hal.h). */
#ifndef UGH_CPU_H
#define UGH_CPU_H

#include <stdint.h>
#include <stddef.h>

enum { AX, CX, DX, BX, SP, BP, SI, DI };
enum { ES, CS, SS, DS };

#define F_CF 0x0001u
#define F_PF 0x0004u
#define F_AF 0x0010u
#define F_ZF 0x0040u
#define F_SF 0x0080u
#define F_TF 0x0100u
#define F_IF 0x0200u
#define F_DF 0x0400u
#define F_OF 0x0800u

#define MEM_SIZE 0x110000u

/* the BIOS/DOS trap stubs: IVT entries point to F000:(n*4); each stub is F1 n CF (trap n, iret) */
#define TRAP_SEG 0xF000u
#define TRAP_OPCODE 0xF1u

typedef struct Cpu {
    uint16_t r[8];
    uint16_t s[4];
    uint16_t ip;
    uint16_t flags;
    uint64_t icount;          /* instructions executed (the virtual clock's base) */
    int halted;               /* HLT executed; cleared by an interrupt */
    int stop;                 /* request to leave cpu_run */
    int irq_pending;          /* set by the HAL: check the PIC before the next instruction */
    uint16_t op_cs, op_ip;    /* the current instruction's address (diagnostics) */
} Cpu;

extern uint8_t g_mem[MEM_SIZE];
extern Cpu g_cpu;

/* byte registers: AL CL DL BL AH CH DH BH */
static inline uint8_t r8get(const Cpu *c, int i)
{
    return i < 4 ? (uint8_t)c->r[i] : (uint8_t)(c->r[i - 4] >> 8);
}
static inline void r8set(Cpu *c, int i, uint8_t v)
{
    if (i < 4) c->r[i] = (uint16_t)((c->r[i] & 0xFF00u) | v);
    else c->r[i - 4] = (uint16_t)((c->r[i - 4] & 0x00FFu) | ((uint16_t)v << 8));
}

/* memory: the VGA window A0000..BFFFF is decoded by vga.c */
uint8_t vga_read(uint32_t a);
void vga_write(uint32_t a, uint8_t v);

static inline uint32_t lin(uint16_t seg, uint16_t off)
{
    return ((uint32_t)seg << 4) + off;
}
static inline uint8_t rb(uint32_t a)
{
    if (a - 0xA0000u < 0x20000u) return vga_read(a);
    return g_mem[a < MEM_SIZE ? a : a & 0xFFFFFu];
}
static inline void wb(uint32_t a, uint8_t v)
{
    if (a - 0xA0000u < 0x20000u) { vga_write(a, v); return; }
    if (a >= 0xC0000u && a < 0x100000u) return;      /* ROM */
    g_mem[a < MEM_SIZE ? a : a & 0xFFFFFu] = v;
}
static inline uint16_t rw(uint32_t a)
{
    return (uint16_t)(rb(a) | ((uint16_t)rb(a + 1) << 8));
}
static inline void ww(uint32_t a, uint16_t v)
{
    wb(a, (uint8_t)v);
    wb(a + 1, (uint8_t)(v >> 8));
}
/* segment:offset accesses wrap the offset at 64 KB, as the 8086 does */
static inline uint8_t rbs(uint16_t s, uint16_t o) { return rb(lin(s, o)); }
static inline void wbs(uint16_t s, uint16_t o, uint8_t v) { wb(lin(s, o), v); }
static inline uint16_t rws(uint16_t s, uint16_t o)
{
    return (uint16_t)(rb(lin(s, o)) | ((uint16_t)rb(lin(s, (uint16_t)(o + 1))) << 8));
}
static inline void wws(uint16_t s, uint16_t o, uint16_t v)
{
    wb(lin(s, o), (uint8_t)v);
    wb(lin(s, (uint16_t)(o + 1)), (uint8_t)(v >> 8));
}

static inline void push16(Cpu *c, uint16_t v)
{
    c->r[SP] = (uint16_t)(c->r[SP] - 2u);
    wws(c->s[SS], c->r[SP], v);
}
static inline uint16_t pop16(Cpu *c)
{
    uint16_t v = rws(c->s[SS], c->r[SP]);
    c->r[SP] = (uint16_t)(c->r[SP] + 2u);
    return v;
}

void cpu_reset(Cpu *c);
/* runs until c->icount reaches `until` or c->stop is set */
void cpu_run(Cpu *c, uint64_t until);
/* raise interrupt n now (pushes flags, CS, IP) */
void cpu_interrupt(Cpu *c, uint8_t n);

/* the HAL's side */
uint8_t io_in8(uint16_t port);
void io_out8(uint16_t port, uint8_t v);
void hal_trap(Cpu *c, uint8_t n);       /* a BIOS/DOS service, from the trap stub */
int pic_ack(void);                      /* the pending unmasked IRQ's vector, or -1 */
void cpu_fatal(const char *fmt, ...);

/* ALU helpers shared with the recompiled code */
uint32_t cpu_alu(Cpu *c, int op, uint32_t a, uint32_t b, int w);
uint32_t cpu_incdec(Cpu *c, uint32_t a, int dec, int w);
uint32_t cpu_shift(Cpu *c, int op, uint32_t a, unsigned n, int w);
void cpu_mul(Cpu *c, int op, uint32_t v, int w);
int cpu_cond(const Cpu *c, int cc);
void cpu_string(Cpu *c, uint8_t op, int seg, int rep);   /* seg: override or -1; rep: 0, F2h, F3h */
extern int (*g_native_hook)(Cpu *c, uint64_t until);
extern const uint8_t g_parity[256];
#define TRACE_RING 65536u
extern uint32_t g_trace_ring[TRACE_RING];
extern unsigned g_trace_pos;
extern int g_trace_on;
extern uint16_t *g_cov;

#endif
