#!/usr/bin/env python3
"""ughrec.py - the static recompiler: UGH.EXE's code segments -> src/gen/gen_all.c.

Input: work/unpacked/ugh.img, the program as the PKLITE decompressor leaves it at its entry point
(memory from the load segment up; made by `UGH_DUMPIMG=work/unpacked/ugh.img build/ughtest.exe ...`),
and work/cov.txt, the CS:IP of every instruction that scripted sessions executed (UGH_COV; see
tools/coverage.ps1).

The program was built with Borland C++ in a far-code model: five code segments, the run-time C
library first.  Every basic block found by recursive descent (from the entry point, every far call
target and the executed addresses the analysis does not reach: interrupt handlers, function
pointers, switch tables) becomes one C function over the shared machine state (src/core/cpu.h).
The instructions' semantics are the interpreter's helpers (cpu_alu, cpu_shift, ...), so the
recompiled code and the interpreter agree flag for flag.  A block ends at every control transfer
and after the instructions after which an interrupt may be taken (STI, POPF, OUT, IN, INT, HLT), so
interrupts are delivered between blocks as the interpreter does.  An instruction the generator
does not translate ends its block before it; the interpreter runs it.

The segment values in the image are already relocated for the fixed load segment (LOAD_SEG, 0110h);
gen_rt.c checks a hash of every code segment at the entry point before it uses the blocks.

Usage: python tools/ughrec.py            (writes src/gen/gen_all.c)
       python tools/ughrec.py --list     (also writes work/ugh.lst, the disassembly of what was found)
"""
import os
import re
import sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_16
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG, X86_REG_CS

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOAD_SEG = 0x0110
# run-time code segments and their lengths (the MZ image's 0000, 013B, 0664, 0878, 0A32)
SEGS = {0x0110: 0x13B0, 0x024B: 0x5290, 0x0774: 0x2140, 0x0988: 0x1BA0, 0x0B42: 0x0350}
ENTRY = (0x0110, 0x0000)

R16 = {"ax": "AX", "cx": "CX", "dx": "DX", "bx": "BX", "sp": "SP", "bp": "BP", "si": "SI", "di": "DI"}
R8 = {"al": 0, "cl": 1, "dl": 2, "bl": 3, "ah": 4, "ch": 5, "dh": 6, "bh": 7}
SR = {"es": "ES", "cs": "CS", "ss": "SS", "ds": "DS"}
ALU = {"add": 0, "or": 1, "adc": 2, "sbb": 3, "and": 4, "sub": 5, "xor": 6, "cmp": 7}
SHIFT = {"rol": 0, "ror": 1, "rcl": 2, "rcr": 3, "shl": 4, "sal": 4, "shr": 5, "sar": 7}
JCC = {"jo": 0, "jno": 1, "jb": 2, "jae": 3, "je": 4, "jne": 5, "jbe": 6, "ja": 7,
       "js": 8, "jns": 9, "jp": 10, "jnp": 11, "jl": 12, "jge": 13, "jle": 14, "jg": 15}
SEGPFX = {0x26: "ES", 0x2E: "CS", 0x36: "SS", 0x3E: "DS"}
BREAK_AFTER = {"sti", "popf", "out", "in", "hlt", "int", "iret", "cli", "into"}
COND = set(JCC) | {"jcxz", "loop", "loope", "loopne"}


class Unsupported(Exception):
    pass


class Ins:
    __slots__ = ("addr", "size", "dec", "target", "kind", "far")


def load_image():
    return open(os.path.join(ROOT, "work", "unpacked", "ugh.img"), "rb").read()


def seg_bytes(img, cs):
    base = (cs - LOAD_SEG) * 16
    return img[base:base + SEGS[cs]]


class Explorer:
    """recursive descent over all code segments; ins[(cs, ip)] = Ins"""

    def __init__(self, img, executed=()):
        self.img = img
        self.executed = set(executed)
        self.code = {cs: seg_bytes(img, cs) for cs in SEGS}
        self.md = Cs(CS_ARCH_X86, CS_MODE_16)
        self.md.detail = True
        self.ins = {}
        self.owner = {}
        self.indirect = []
        self.cswrites = []
        self.warn = []

    def explore(self, entries):
        work = list(entries)
        while work:
            cs, a = work.pop()
            if cs not in self.code:
                continue
            code = self.code[cs]
            while 0 <= a < len(code) and (cs, a) not in self.ins:
                if (cs, a) in self.owner:
                    if (cs, a) not in self.executed:
                        self.warn.append(f"overlap at {cs:04x}:{a:04x}")
                        break
                    # an executed address inside a decode that ran into data: the executed one wins
                    bad = self.owner[(cs, a)]
                    bi = self.ins.pop((cs, bad), None)
                    if bi is not None:
                        for k in range(bad, bad + bi.size):
                            self.owner.pop((cs, k), None)
                dec = next(self.md.disasm(code[a:a + 16], a), None)
                if dec is None or a + dec.size > len(code):
                    self.warn.append(f"bad opcode at {cs:04x}:{a:04x}")
                    break
                i = Ins()
                i.addr, i.size, i.dec, i.target, i.kind, i.far = a, dec.size, dec, None, None, None
                self.ins[(cs, a)] = i
                for k in range(a, a + dec.size):
                    self.owner[(cs, k)] = a
                mn = dec.mnemonic
                ops = dec.operands
                if ops and ops[0].type == X86_OP_MEM and ops[0].mem.segment == X86_REG_CS and \
                        mn not in ("jmp", "call", "lcall", "ljmp", "cmp", "test", "push") and not ops[0].mem.base and not ops[0].mem.index:
                    self.cswrites.append((cs, a, ops[0].mem.disp & 0xFFFF, ops[0].size))
                imm = ops[0].imm & 0xFFFF if ops and ops[0].type == X86_OP_IMM else None
                nxt = a + dec.size
                if mn in COND:
                    i.kind, i.target = "jcc", imm
                    work.append((cs, imm))
                elif mn == "jmp":
                    if imm is not None and len(ops) == 1:
                        i.kind, i.target = "jmp", imm
                        work.append((cs, imm))
                    else:
                        i.kind = "ijmp"
                        self.indirect.append((cs, a, dec.op_str))
                    break
                elif mn == "call":
                    if imm is not None:
                        i.kind, i.target = "call", imm
                        work.append((cs, imm))
                    else:
                        i.kind = "icall"
                        self.indirect.append((cs, a, dec.op_str))
                elif mn in ("lcall", "ljmp"):
                    i.kind = "far"
                    if len(ops) == 2:
                        i.far = (ops[0].imm & 0xFFFF, ops[1].imm & 0xFFFF)
                        work.append(i.far)
                    else:
                        self.indirect.append((cs, a, dec.op_str))
                    if mn == "ljmp":
                        break
                elif mn in ("ret", "retf", "iret", "hlt"):
                    i.kind = mn
                    break
                a = nxt
        return self


class Gen:
    def __init__(self, cs):
        self.cs = cs

    def memaddr(self, i, op, segpfx):
        m = op.mem
        parts = []
        base = i.reg_name(m.base) if m.base else None
        index = i.reg_name(m.index) if m.index else None
        if base:
            parts.append(f"c->r[{R16[base]}]")
        if index:
            parts.append(f"c->r[{R16[index]}]")
        disp = m.disp & 0xFFFF
        if disp or not parts:
            parts.append(f"0x{disp:X}u")
        off = parts[0] if len(parts) == 1 and not parts[0].startswith("c->") else f"(uint16_t)({' + '.join(parts)})"
        if segpfx:
            seg = segpfx
        elif m.segment:
            seg = SR[i.reg_name(m.segment)]
        elif base == "bp":
            seg = "SS"
        else:
            seg = "DS"
        return f"c->s[{seg}]", off

    @staticmethod
    def width(i, op):
        if op.type == X86_OP_REG:
            return 0 if i.reg_name(op.reg) in R8 else 1
        return 1 if op.size == 2 else 0

    def get(self, i, op, w, segpfx):
        if op.type == X86_OP_REG:
            n = i.reg_name(op.reg)
            if n in R16:
                return f"c->r[{R16[n]}]"
            if n in R8:
                return f"r8get(c, {R8[n]})"
            if n in SR:
                return f"c->s[{SR[n]}]"
            raise Unsupported(n)
        if op.type == X86_OP_IMM:
            return f"0x{op.imm & (0xFFFF if w else 0xFF):X}u"
        seg, off = self.memaddr(i, op, segpfx)
        return f"rws({seg}, {off})" if w else f"rbs({seg}, {off})"

    def set(self, i, op, w, val, segpfx):
        if op.type == X86_OP_REG:
            n = i.reg_name(op.reg)
            if n in R16:
                return f"c->r[{R16[n]}] = (uint16_t)({val});"
            if n in R8:
                return f"r8set(c, {R8[n]}, (uint8_t)({val}));"
            if n in SR:
                return f"c->s[{SR[n]}] = (uint16_t)({val});"
            raise Unsupported(n)
        seg, off = self.memaddr(i, op, segpfx)
        return f"wws({seg}, {off}, (uint16_t)({val}));" if w else f"wbs({seg}, {off}, (uint8_t)({val}));"

    def emit(self, i):
        """one instruction: (C lines, ends_block)"""
        raw = bytes(i.bytes)
        k = 0
        segpfx = None
        rep = 0
        while raw[k] in (0x26, 0x2E, 0x36, 0x3E, 0xF2, 0xF3, 0xF0):
            if raw[k] in SEGPFX:
                segpfx = SEGPFX[raw[k]]
            elif raw[k] in (0xF2, 0xF3):
                rep = raw[k]
            k += 1
        opc = raw[k]
        mn = i.mnemonic
        ops = i.operands
        nxt = (i.address + i.size) & 0xFFFF
        a = i.address
        if rep and opc not in (0xA4, 0xA5, 0xA6, 0xA7, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0x6C, 0x6D, 0x6E, 0x6F):
            raise Unsupported(f"rep {mn}")
        if mn in ALU:
            w = self.width(i, ops[0])
            L = [f"{{ uint32_t r = cpu_alu(c, {ALU[mn]}, {self.get(i, ops[0], w, segpfx)}, {self.get(i, ops[1], w, segpfx)}, {w});"]
            L.append("  " + (self.set(i, ops[0], w, "r", segpfx) if mn != "cmp" else "(void)r;"))
            L.append("}")
            return L, False
        if mn == "test":
            w = self.width(i, ops[0])
            return [f"(void)cpu_alu(c, 4, {self.get(i, ops[0], w, segpfx)}, {self.get(i, ops[1], w, segpfx)}, {w});"], False
        if mn in ("inc", "dec"):
            w = self.width(i, ops[0])
            return [self.set(i, ops[0], w, f"cpu_incdec(c, {self.get(i, ops[0], w, segpfx)}, {1 if mn == 'dec' else 0}, {w})", segpfx)], False
        if mn == "neg":
            w = self.width(i, ops[0])
            return [f"{{ uint32_t v = {self.get(i, ops[0], w, segpfx)}; uint32_t r = cpu_alu(c, 5, 0, v, {w});",
                    "  " + self.set(i, ops[0], w, "r", segpfx),
                    "  if (v) c->flags |= F_CF; else c->flags &= (uint16_t)~F_CF; }"], False
        if mn == "not":
            w = self.width(i, ops[0])
            return [self.set(i, ops[0], w, f"~{self.get(i, ops[0], w, segpfx)}", segpfx)], False
        if mn == "mov":
            if ops[0].type == X86_OP_REG:
                w = self.width(i, ops[0])
            elif ops[1].type == X86_OP_REG:
                w = self.width(i, ops[1])
            else:
                w = 1 if ops[0].size == 2 else 0
            if (ops[0].type == X86_OP_REG and i.reg_name(ops[0].reg) in SR) or (ops[1].type == X86_OP_REG and i.reg_name(ops[1].reg) in SR):
                w = 1
            if ops[0].type == X86_OP_REG and i.reg_name(ops[0].reg) == "cs":
                raise Unsupported("mov cs")
            return [self.set(i, ops[0], w, self.get(i, ops[1], w, segpfx), segpfx)], False
        if mn == "xchg":
            w = self.width(i, ops[0])
            return [f"{{ uint32_t t0 = {self.get(i, ops[0], w, segpfx)}; uint32_t t1 = {self.get(i, ops[1], w, segpfx)};",
                    "  " + self.set(i, ops[0], w, "t1", segpfx), "  " + self.set(i, ops[1], w, "t0", segpfx), "}"], False
        if mn == "lea":
            _, off = self.memaddr(i, ops[1], segpfx)
            return [f"c->r[{R16[i.reg_name(ops[0].reg)]}] = (uint16_t)({off});"], False
        if mn in ("les", "lds"):
            seg, off = self.memaddr(i, ops[1], segpfx)
            sreg = "ES" if mn == "les" else "DS"
            return [f"{{ uint16_t o = {off}; uint16_t s = {seg}; c->r[{R16[i.reg_name(ops[0].reg)]}] = rws(s, o); c->s[{sreg}] = rws(s, (uint16_t)(o + 2u)); }}"], False
        if mn == "push":
            if ops[0].type == X86_OP_IMM:
                return [f"push16(c, 0x{ops[0].imm & 0xFFFF:X}u);"], False
            if ops[0].type == X86_OP_REG and i.reg_name(ops[0].reg) == "sp":
                return ["push16(c, c->r[SP]);"], False      # the 286 pushes the old SP; push16 reads it first
            return [f"push16(c, {self.get(i, ops[0], 1, segpfx)});"], False
        if mn == "pop":
            if ops[0].type == X86_OP_REG and i.reg_name(ops[0].reg) == "cs":
                raise Unsupported("pop cs")
            if ops[0].type == X86_OP_REG and i.reg_name(ops[0].reg) == "ss":
                return ["c->s[SS] = pop16(c);"], False
            return [f"{{ uint16_t v = pop16(c); {self.set(i, ops[0], 1, 'v', segpfx)} }}"], False
        if mn == "pusha":
            return ["{ uint16_t sp = c->r[SP]; int k; for (k = 0; k < 8; k++) push16(c, k == SP ? sp : c->r[k]); }"], False
        if mn == "popa":
            return ["{ int k; for (k = 7; k >= 0; k--) { uint16_t v = pop16(c); if (k != SP) c->r[k] = v; } }"], False
        if mn == "enter":
            size = ops[0].imm & 0xFFFF
            level = ops[1].imm & 0x1F
            if level != 0:
                raise Unsupported("enter level")
            return [f"push16(c, c->r[BP]); c->r[BP] = c->r[SP]; c->r[SP] = (uint16_t)(c->r[SP] - 0x{size:X}u);"], False
        if mn == "leave":
            return ["c->r[SP] = c->r[BP]; c->r[BP] = pop16(c);"], False
        if mn in SHIFT:
            w = self.width(i, ops[0])
            if len(ops) == 1:
                cnt = "1u"
            elif ops[1].type == X86_OP_IMM:
                cnt = f"{ops[1].imm & 0xFF}u"
            else:
                cnt = "(unsigned)(c->r[CX] & 0xFFu)"
            return [self.set(i, ops[0], w, f"cpu_shift(c, {SHIFT[mn]}, {self.get(i, ops[0], w, segpfx)}, {cnt}, {w})", segpfx)], False
        if mn in ("mul", "imul", "div", "idiv") and len(ops) == 1:
            w = self.width(i, ops[0])
            code = {"mul": 4, "imul": 5, "div": 6, "idiv": 7}[mn]
            if mn in ("div", "idiv"):
                return [f"c->ip = 0x{nxt:04X}u; c->op_ip = 0x{a:04X}u; c->op_cs = c->s[CS];",
                        f"cpu_mul(c, {code}, {self.get(i, ops[0], w, segpfx)}, {w});",
                        f"if (c->ip != 0x{nxt:04X}u) return 1;"], False
            return [f"cpu_mul(c, {code}, {self.get(i, ops[0], w, segpfx)}, {w});"], False
        if mn == "imul":
            src = self.get(i, ops[1], 1, segpfx)
            mul = f"0x{ops[2].imm & 0xFFFF:X}u" if len(ops) == 3 else self.get(i, ops[0], 1, segpfx)
            return [f"{{ int32_t r = (int32_t)(int16_t){src} * (int32_t)(int16_t){mul};",
                    f"  c->r[{R16[i.reg_name(ops[0].reg)]}] = (uint16_t)r;",
                    "  if (r != (int16_t)r) c->flags |= F_CF | F_OF; else c->flags &= (uint16_t)~(F_CF | F_OF); }"], False
        if opc == 0x98:
            return ["c->r[AX] = (uint16_t)(int16_t)(int8_t)c->r[AX];"], False
        if opc == 0x99:
            return ["c->r[DX] = (c->r[AX] & 0x8000u) ? 0xFFFFu : 0u;"], False
        if opc == 0xD7:
            seg = segpfx or "DS"
            return [f"r8set(c, 0, rbs(c->s[{seg}], (uint16_t)(c->r[BX] + (c->r[AX] & 0xFFu))));"], False
        if opc in (0xA4, 0xA5, 0xA6, 0xA7, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0x6C, 0x6D, 0x6E, 0x6F):
            seg = {"ES": 0, "CS": 1, "SS": 2, "DS": 3}[segpfx] if segpfx else -1
            return [f"cpu_string(c, 0x{opc:02X}, {seg}, 0x{rep:02X});"], False
        if mn in ("clc", "stc", "cli", "sti", "cld", "std", "cmc"):
            return [{"clc": "c->flags &= (uint16_t)~F_CF;", "stc": "c->flags |= F_CF;",
                     "cli": "c->flags &= (uint16_t)~F_IF;", "sti": "c->flags |= F_IF; c->irq_pending = 1;",
                     "cld": "c->flags &= (uint16_t)~F_DF;", "std": "c->flags |= F_DF;",
                     "cmc": "c->flags ^= F_CF;"}[mn]], False
        if mn == "pushf":
            return ["push16(c, (uint16_t)((c->flags & 0x0FD5u) | 0x0002u));"], False
        if mn == "popf":
            return ["c->flags = (uint16_t)((pop16(c) & 0x0FD5u) | 0x0002u); c->irq_pending = 1;"], False
        if mn == "lahf":
            return ["r8set(c, 4, (uint8_t)(c->flags | 2u));"], False
        if mn == "sahf":
            return ["c->flags = (uint16_t)((c->flags & 0xFF00u) | ((c->r[AX] >> 8) & 0xD5u) | 2u);"], False
        if mn in ("nop", "wait"):
            return [], False
        if mn == "in":
            port = f"0x{ops[1].imm & 0xFF:X}u" if ops[1].type == X86_OP_IMM else "c->r[DX]"
            if i.reg_name(ops[0].reg) == "al":
                return [f"r8set(c, 0, io_in8({port}));"], False
            return [f"c->r[AX] = (uint16_t)(io_in8({port}) | (io_in8((uint16_t)({port} + 1u)) << 8));"], False
        if mn == "out":
            port = f"0x{ops[0].imm & 0xFF:X}u" if ops[0].type == X86_OP_IMM else "c->r[DX]"
            if i.reg_name(ops[1].reg) == "al":
                return [f"io_out8({port}, (uint8_t)c->r[AX]);"], False
            return [f"io_out8({port}, (uint8_t)c->r[AX]); io_out8((uint16_t)({port} + 1u), (uint8_t)(c->r[AX] >> 8));"], False
        # ---- control transfers ----
        if mn in JCC:
            t = ops[0].imm & 0xFFFF
            return [f"c->ip = cpu_cond(c, {JCC[mn]}) ? 0x{t:04X}u : 0x{nxt:04X}u;"], True
        if mn in ("loop", "loope", "loopne"):
            t = ops[0].imm & 0xFFFF
            cond = {"loop": "1", "loope": "(c->flags & F_ZF)", "loopne": "!(c->flags & F_ZF)"}[mn]
            return [f"c->r[CX]--; c->ip = (c->r[CX] != 0 && {cond}) ? 0x{t:04X}u : 0x{nxt:04X}u;"], True
        if mn == "jcxz":
            t = ops[0].imm & 0xFFFF
            return [f"c->ip = c->r[CX] == 0 ? 0x{t:04X}u : 0x{nxt:04X}u;"], True
        if mn == "jmp":
            if ops[0].type == X86_OP_IMM and len(ops) == 1:
                return [f"c->ip = 0x{ops[0].imm & 0xFFFF:04X}u;"], True
            return [f"c->ip = {self.get(i, ops[0], 1, segpfx)};"], True
        if mn == "call":
            if ops[0].type == X86_OP_IMM:
                return [f"push16(c, 0x{nxt:04X}u); c->ip = 0x{ops[0].imm & 0xFFFF:04X}u;"], True
            return [f"{{ uint16_t t = {self.get(i, ops[0], 1, segpfx)}; push16(c, 0x{nxt:04X}u); c->ip = t; }}"], True
        if mn in ("lcall", "ljmp"):
            if len(ops) == 2:
                segv = ops[0].imm & 0xFFFF
                pre = f"push16(c, c->s[CS]); push16(c, 0x{nxt:04X}u); " if mn == "lcall" else ""
                return [f"{pre}c->s[CS] = 0x{segv:04X}u; c->ip = 0x{ops[1].imm & 0xFFFF:04X}u;"], True
            seg, off = self.memaddr(i, ops[0], segpfx)
            pre = "push16(c, c->s[CS]); push16(c, 0x%04Xu); " % nxt if mn == "lcall" else ""
            return [f"{{ uint16_t s = {seg}; uint16_t o = {off}; uint16_t nip = rws(s, o), ncs = rws(s, (uint16_t)(o + 2u));",
                    f"  {pre}c->s[CS] = ncs; c->ip = nip; }}"], True
        if mn == "ret":
            if ops:
                return [f"c->ip = pop16(c); c->r[SP] = (uint16_t)(c->r[SP] + {ops[0].imm & 0xFFFF}u);"], True
            return ["c->ip = pop16(c);"], True
        if mn == "retf":
            n = ops[0].imm & 0xFFFF if ops else 0
            return [f"c->ip = pop16(c); c->s[CS] = pop16(c); c->r[SP] = (uint16_t)(c->r[SP] + {n}u);"], True
        if mn == "iret":
            return ["c->ip = pop16(c); c->s[CS] = pop16(c); c->flags = (uint16_t)((pop16(c) & 0x0FD5u) | 0x0002u); c->irq_pending = 1;"], True
        if mn == "int":
            return [f"c->ip = 0x{nxt:04X}u; cpu_interrupt(c, 0x{ops[0].imm & 0xFF:02X});"], True
        if mn == "hlt":
            return [f"c->ip = 0x{nxt:04X}u; c->halted = 1;"], True
        raise Unsupported(f"{mn} {i.op_str}")


def fnv(b):
    h = 1469598103934665603
    for x in b:
        h = ((h ^ x) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def main():
    img = load_image()
    covp = os.path.join(ROOT, "work", "cov.txt")
    cov = []
    if os.path.exists(covp):
        for line in open(covp).read().split():
            s, o = line.split(":")
            s, o = int(s, 16), int(o, 16)
            if s in SEGS and o < SEGS[s]:
                cov.append((s, o))
    ex = Explorer(img, cov)
    ex.explore([ENTRY])
    # far call targets found along the way are explored by explore(); executed addresses that the
    # analysis did not reach become entries
    for e in sorted(cov):
        if e not in ex.ins and e not in ex.owner:
            ex.explore([e])
    starts = {ENTRY}
    # block starts: entries, branch targets, fall-throughs after transfers and interrupt windows,
    # and every executed address that starts a stretch the descent reached only from coverage
    for (cs, a), i in ex.ins.items():
        if i.target is not None:
            starts.add((cs, i.target))
        if i.far is not None:
            starts.add(i.far)
        if i.kind is not None or i.dec.mnemonic in BREAK_AFTER:
            starts.add((cs, a + i.size))
        if i.kind in ("call", "icall", "far"):
            starts.add((cs, a + i.size))
    # the targets of indirect jumps/calls and handlers: the executed addresses whose predecessor is
    # not a fall-through (cheap approximation: every executed address that follows a transfer)
    covset = set(cov)
    for (cs, a) in cov:
        if (cs, a) not in ex.ins:
            continue
        prev = None
        for back in range(1, 7):
            p = ex.ins.get((cs, a - back))
            if p is not None and p.addr + p.size == a:
                prev = p
                break
        if prev is None or prev.kind in ("ijmp", "jmp", "ret", "retf", "iret", "far", "icall", "call"):
            starts.add((cs, a))
    # the addresses src/hal/game.c watches (gen_watch) must start blocks, or a recompiled block
    # would run past them unseen
    src = open(os.path.join(ROOT, "src", "hal", "game.c")).read()
    for m in re.finditer(r"gen_watch\((GAME|REL\(0x([0-9A-Fa-f]+)\)),\s*0x([0-9A-Fa-f]+)", src):
        seg = 0x024B if m.group(1) == "GAME" else int(m.group(2), 16) + LOAD_SEG
        hook = (seg, int(m.group(3), 16))
        if hook not in ex.ins:
            print(f"warning: watched {hook[0]:04x}:{hook[1]:04x} is not a decoded instruction")
        starts.add(hook)
    starts = sorted(s for s in starts if s in ex.ins)
    # CS-relative writes onto decoded instructions would be self-modifying code
    smc = []
    for cs, a, disp, size in ex.cswrites:
        for k in range(disp, disp + max(size, 1)):
            if (cs, k) in ex.owner:
                smc.append((cs, a, disp))
                break
    smc_bytes = set()
    for cs, a, disp in smc:
        smc_bytes.update((cs, k) for k in range(disp, disp + 2))
    out = ["/* gen_all.c - GENERATED by tools/ughrec.py from UGH.EXE's code segments; do not edit.",
           " * Each function is one basic block of the original program; see tools/ughrec.py. */",
           '#include "gen.h"', ""]
    names = []
    unsupported = {}
    startset = set(starts)
    for (cs, s) in starts:
        g = Gen(cs)
        a = s
        body = []
        n = 0
        while True:
            i = ex.ins.get((cs, a))
            if i is None:
                body.append(f"    c->ip = 0x{a:04X}u;")
                break
            dec = i.dec
            if any((cs, k) in smc_bytes for k in range(a, a + i.size)):
                if n == 0:
                    body = None
                else:
                    body.append(f"    c->ip = 0x{a:04X}u;   /* written at run time: the interpreter */")
                break
            try:
                lines, ends = g.emit(dec)
            except Unsupported as e:
                unsupported[str(e).split(" ")[0]] = unsupported.get(str(e).split(" ")[0], 0) + 1
                if n == 0:
                    body = None
                else:
                    body.append(f"    c->ip = 0x{a:04X}u;   /* {dec.mnemonic}: the interpreter */")
                break
            body.append(f"    /* {cs:04X}:{a:04X}: {dec.mnemonic} {dec.op_str} */")
            body.extend("    " + ln for ln in lines)
            n += 1
            a = (a + dec.size) & 0xFFFF
            if ends:
                break
            if dec.mnemonic in BREAK_AFTER or (cs, a) in startset:
                body.append(f"    c->ip = 0x{a:04X}u;")
                break
        if body is None:
            continue
        names.append((cs, s))
        out.append(f"static int b_{cs:04X}_{s:04X}(Cpu *c)")
        out.append("{")
        out.append(f"    c->icount += {n}u;")
        out.extend(body)
        out.append("    return 1;")
        out.append("}")
        out.append("")
    out.append("const GenBlock g_gen_blocks[] = {")
    for cs, s in names:
        out.append(f"    {{0x{cs:04X}u, 0x{s:04X}u, b_{cs:04X}_{s:04X}}},")
    out.append("};")
    out.append(f"const unsigned g_gen_block_count = {len(names)}u;")
    out.append("const GenRange g_gen_ranges[] = {")
    for cs, n in sorted(SEGS.items()):
        out.append(f"    {{0x{cs * 16:05X}u, 0x{n:04X}u, 0x{fnv(seg_bytes(img, cs)):016X}ull}},")
    out.append("};")
    out.append(f"const unsigned g_gen_range_count = {len(SEGS)}u;")
    out.append(f"const uint16_t g_gen_entry_cs = 0x{ENTRY[0]:04X}u, g_gen_entry_ip = 0x{ENTRY[1]:04X}u;")
    with open(os.path.join(ROOT, "src", "gen", "gen_all.c"), "w", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    total = sum(SEGS.values())
    covered = sum(i.size for i in ex.ins.values())
    print(f"{len(ex.ins)} instructions ({covered:#x} of {total:#x} code bytes, {100 * covered / total:.1f}%), "
          f"{len(names)} blocks, {len(cov)} executed addresses ({len([c for c in cov if c in ex.ins])} decoded)")
    if unsupported:
        print("  ended at (interpreter):", ", ".join(f"{k} x{v}" for k, v in sorted(unsupported.items())))
    if smc:
        print(f"  WARNING: {len(smc)} cs: writes onto decoded code:", ", ".join(f"{c:04x}:{a:04x}->{d:04x}" for c, a, d in smc[:10]))
    if ex.warn:
        print(f"  {len(ex.warn)} warnings, first: {ex.warn[:5]}")
    if "--list" in sys.argv:
        with open(os.path.join(ROOT, "work", "ugh.lst"), "w") as f:
            for (cs, a) in sorted(ex.ins):
                i = ex.ins[(cs, a)]
                mark = ">" if (cs, a) in startset else " "
                f.write(f"{mark}{cs:04X}:{a:04X}  {bytes(i.dec.bytes).hex():<16} {i.dec.mnemonic} {i.dec.op_str}\n")


if __name__ == "__main__":
    main()
