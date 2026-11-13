/*
 * opl.c - the YM3812 (opl.h), taken from the Civ64 project (same author, MIT).  Each sample runs
 * the 18 slots in their order (a channel's modulator before its carrier): the feedback input, the envelope, the
 * phase and the output; then the channels are summed and clipped, and the tremolo, vibrato and envelope timers
 * step.  An operator's output is exp(-(logsin(phase + modulation) + 8 * level)) in the chip's fixed point: the
 * 256-entry quarter-wave log-sine and exponent tables below are round(-log2(sin((i + 0.5) pi / 512)) * 256) and
 * round(2^((255 - i) / 256) * 1024), the chip's ROMs (chip fact; the YM2203 has the same, pdos_opn.c).  Portable C.
 */
#include <stdint.h>
#include <string.h>

#include "opl.h"

enum { ATTACK = 0, DECAY, SUSTAIN, RELEASE };

const uint16_t pdos_fm_logsin[256] = {
    2137, 1731, 1543, 1419, 1326, 1252, 1190, 1137, 1091, 1050, 1013, 979, 949, 920, 894, 869,
    846, 825, 804, 785, 767, 749, 732, 717, 701, 687, 672, 659, 646, 633, 621, 609,
    598, 587, 576, 566, 556, 546, 536, 527, 518, 509, 501, 492, 484, 476, 468, 461,
    453, 446, 439, 432, 425, 418, 411, 405, 399, 392, 386, 380, 375, 369, 363, 358,
    352, 347, 341, 336, 331, 326, 321, 316, 311, 307, 302, 297, 293, 289, 284, 280,
    276, 271, 267, 263, 259, 255, 251, 248, 244, 240, 236, 233, 229, 226, 222, 219,
    215, 212, 209, 205, 202, 199, 196, 193, 190, 187, 184, 181, 178, 175, 172, 169,
    167, 164, 161, 159, 156, 153, 151, 148, 146, 143, 141, 138, 136, 134, 131, 129,
    127, 125, 122, 120, 118, 116, 114, 112, 110, 108, 106, 104, 102, 100, 98, 96,
    94, 92, 91, 89, 87, 85, 83, 82, 80, 78, 77, 75, 74, 72, 70, 69,
    67, 66, 64, 63, 62, 60, 59, 57, 56, 55, 53, 52, 51, 49, 48, 47,
    46, 45, 43, 42, 41, 40, 39, 38, 37, 36, 35, 34, 33, 32, 31, 30,
    29, 28, 27, 26, 25, 24, 23, 23, 22, 21, 20, 20, 19, 18, 17, 17,
    16, 15, 15, 14, 13, 13, 12, 12, 11, 10, 10, 9, 9, 8, 8, 7,
    7, 7, 6, 6, 5, 5, 5, 4, 4, 4, 3, 3, 3, 2, 2, 2,
    2, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0,
};

const uint16_t pdos_fm_exp[256] = {
    2042, 2037, 2031, 2026, 2020, 2015, 2010, 2004, 1999, 1993, 1988, 1983, 1977, 1972, 1966, 1961,
    1956, 1951, 1945, 1940, 1935, 1930, 1924, 1919, 1914, 1909, 1904, 1898, 1893, 1888, 1883, 1878,
    1873, 1868, 1863, 1858, 1853, 1848, 1843, 1838, 1833, 1828, 1823, 1818, 1813, 1808, 1803, 1798,
    1794, 1789, 1784, 1779, 1774, 1769, 1765, 1760, 1755, 1750, 1746, 1741, 1736, 1732, 1727, 1722,
    1717, 1713, 1708, 1704, 1699, 1694, 1690, 1685, 1681, 1676, 1672, 1667, 1663, 1658, 1654, 1649,
    1645, 1640, 1636, 1631, 1627, 1623, 1618, 1614, 1609, 1605, 1601, 1596, 1592, 1588, 1584, 1579,
    1575, 1571, 1566, 1562, 1558, 1554, 1550, 1545, 1541, 1537, 1533, 1529, 1525, 1520, 1516, 1512,
    1508, 1504, 1500, 1496, 1492, 1488, 1484, 1480, 1476, 1472, 1468, 1464, 1460, 1456, 1452, 1448,
    1444, 1440, 1436, 1433, 1429, 1425, 1421, 1417, 1413, 1409, 1406, 1402, 1398, 1394, 1391, 1387,
    1383, 1379, 1376, 1372, 1368, 1364, 1361, 1357, 1353, 1350, 1346, 1342, 1339, 1335, 1332, 1328,
    1324, 1321, 1317, 1314, 1310, 1307, 1303, 1300, 1296, 1292, 1289, 1286, 1282, 1279, 1275, 1272,
    1268, 1265, 1261, 1258, 1255, 1251, 1248, 1244, 1241, 1238, 1234, 1231, 1228, 1224, 1221, 1218,
    1214, 1211, 1208, 1205, 1201, 1198, 1195, 1192, 1188, 1185, 1182, 1179, 1176, 1172, 1169, 1166,
    1163, 1160, 1157, 1154, 1150, 1147, 1144, 1141, 1138, 1135, 1132, 1129, 1126, 1123, 1120, 1117,
    1114, 1111, 1108, 1105, 1102, 1099, 1096, 1093, 1090, 1087, 1084, 1081, 1078, 1075, 1072, 1069,
    1066, 1064, 1061, 1058, 1055, 1052, 1049, 1046, 1044, 1041, 1038, 1035, 1032, 1030, 1027, 1024,
};

/* the frequency multiple, doubled (0 is a half) */
static const uint8_t MULT[16] = {1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30};
/* the key scale level by the F-number's top four bits, in 0.75 dB at block 8 */
static const uint8_t KSL[16] = {0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64};
/* KSL's register value -> the shift of its attenuation: off, 3, 1.5 and 6 dB an octave (the bits' order is the
 * reverse of the data sheet's; chip fact) */
static const uint8_t KSL_SHIFT[4] = {8, 1, 2, 0};
/* the extra step of the fast rates (12..15) by the rate's low bits and the envelope timer's */
static const uint8_t EG_STEP[4][4] = {{0, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 1, 0}, {1, 1, 1, 0}};

/* v >> k rounding down, for a negative v too */
static int asr(int v, unsigned k)
{
    return v >= 0 ? v >> k : -((-v - 1) >> k) - 1;
}

static unsigned slot_channel(unsigned s)
{
    return (s / 6u) * 3u + s % 3u;
}

/* the register offset (0..21) -> the slot, -1 for the offsets without one */
static int reg_slot(unsigned off)
{
    return off < 22u && off % 8u < 6u ? (int)((off / 8u) * 6u + off % 8u) : -1;
}

static int rhythm(const PdosOpl *o)
{
    return (o->bd & 0x20u) != 0u;
}

void pdos_opl_reset(PdosOpl *o)
{
    unsigned i;
    memset(o, 0, sizeof *o);
    for (i = 0; i < 18u; i++) {
        o->slot[i].stage = RELEASE;
        o->slot[i].env = 0x1FFu;
        o->slot[i].level = 0x1FFu;
    }
    o->noise = 1u;
}

/* ---- the timers ------------------------------------------------------------------------------------- */

static uint64_t timer_span(const PdosOpl *o, unsigned i)
{
    return (uint64_t)(256u - o->preset[i]) * (i == 0u ? PDOS_OPL_T1_PIT : PDOS_OPL_T2_PIT);
}

/* the timer's overflows up to clock t: each sets its flag unless masked and reloads the preset */
static void timer_run(PdosOpl *o, unsigned i, uint64_t t)
{
    uint64_t span;
    if (!o->run[i] || t < o->due[i]) {
        return;
    }
    if (!o->mask[i]) {
        o->flag[i] = 1;
    }
    span = timer_span(o, i);
    o->due[i] += span * ((t - o->due[i]) / span + 1u);
}

static void timer_control(PdosOpl *o, uint8_t v, uint64_t t)
{
    unsigned i;
    timer_run(o, 0, t);
    timer_run(o, 1, t);
    if ((v & 0x80u) != 0u) {            /* the IRQ reset: both flags, the other bits ignored */
        o->flag[0] = o->flag[1] = 0;
        return;
    }
    o->mask[0] = (uint8_t)((v >> 6) & 1u);
    o->mask[1] = (uint8_t)((v >> 5) & 1u);
    for (i = 0; i < 2u; i++) {
        uint8_t start = (uint8_t)((v >> i) & 1u);
        if (o->mask[i]) {
            o->flag[i] = 0;
        }
        if (start && !o->run[i]) {
            o->due[i] = t + timer_span(o, i);
        }
        o->run[i] = start;
    }
}

uint8_t pdos_opl_status(PdosOpl *o, uint64_t t)
{
    uint8_t st;
    timer_run(o, 0, t);
    timer_run(o, 1, t);
    st = (uint8_t)((o->flag[0] ? 0x40u : 0u) | (o->flag[1] ? 0x20u : 0u));
    return (uint8_t)(st | (st != 0u ? 0x80u : 0u) | 0x06u);
}

/* ---- the registers ---------------------------------------------------------------------------------- */

static void drum_keys(PdosOpl *o)
{
    static const uint8_t SLOT[5] = {13, 17, 14, 16, 12};   /* BDh bits 0..4: hi-hat, cymbal, tom, snare, bass */
    unsigned i;
    for (i = 12u; i < 18u; i++) {
        o->slot[i].key = (uint8_t)(o->slot[i].key & 1u);
    }
    if (!rhythm(o)) {
        return;
    }
    for (i = 0; i < 5u; i++) {
        if ((o->bd >> i) & 1u) {
            o->slot[SLOT[i]].key |= 2u;
        }
    }
    if (o->bd & 0x10u) {
        o->slot[15].key |= 2u;          /* the bass drum keys both its slots */
    }
}

void pdos_opl_write(PdosOpl *o, uint8_t reg, uint8_t v, uint64_t t)
{
    unsigned hi = reg & 0xE0u;
    int s = reg_slot(reg & 0x1Fu);
    PdosOplSlot *p = s >= 0 ? &o->slot[s] : NULL;
    unsigned ch = reg & 0x0Fu;
    if (reg == 0x01u) {
        o->wse = (uint8_t)((v >> 5) & 1u);
    } else if (reg == 0x02u || reg == 0x03u) {
        o->preset[reg - 2u] = v;
    } else if (reg == 0x04u) {
        timer_control(o, v, t);
    } else if (reg == 0x08u) {
        o->nts = (uint8_t)((v >> 6) & 1u);  /* bit 7, the CSM speech mode, is not modelled (record 4) */
    } else if (hi == 0x20u && p != NULL) {
        p->am = (uint8_t)(v >> 7);
        p->vib = (uint8_t)((v >> 6) & 1u);
        p->egt = (uint8_t)((v >> 5) & 1u);
        p->ksr = (uint8_t)((v >> 4) & 1u);
        p->mult = (uint8_t)(v & 15u);
    } else if (hi == 0x40u && p != NULL) {
        p->ksl = (uint8_t)(v >> 6);
        p->tl = (uint8_t)(v & 0x3Fu);
    } else if (hi == 0x60u && p != NULL) {
        p->ar = (uint8_t)(v >> 4);
        p->dr = (uint8_t)(v & 15u);
    } else if (hi == 0x80u && p != NULL) {
        p->sl = (uint8_t)(v >> 4);
        if (p->sl == 15u) {
            p->sl = 31u;
        }
        p->rr = (uint8_t)(v & 15u);
    } else if (hi == 0xE0u && p != NULL) {
        p->wf = (uint8_t)(v & 3u);
    } else if (reg >= 0xA0u && reg <= 0xA8u) {
        o->fnum[ch] = (uint16_t)((o->fnum[ch] & 0x300u) | v);
    } else if (reg >= 0xB0u && reg <= 0xB8u) {
        unsigned m = (ch / 3u) * 6u + ch % 3u;
        o->fnum[ch] = (uint16_t)((o->fnum[ch] & 0xFFu) | ((unsigned)(v & 3u) << 8));
        o->block[ch] = (uint8_t)((v >> 2) & 7u);
        o->kon[ch] = (uint8_t)((v >> 5) & 1u);
        o->slot[m].key = (uint8_t)((o->slot[m].key & 2u) | o->kon[ch]);
        o->slot[m + 3u].key = (uint8_t)((o->slot[m + 3u].key & 2u) | o->kon[ch]);
    } else if (reg == 0xBDu) {
        o->bd = v;
        drum_keys(o);
    } else if (reg >= 0xC0u && reg <= 0xC8u) {
        o->fb[ch] = (uint8_t)((v >> 1) & 7u);
        o->con[ch] = (uint8_t)(v & 1u);
    }
}

/* ---- one sample ------------------------------------------------------------------------------------- */

/* the key scale value: the block and the F-number's bit 9 (or 8, with NTS) */
static unsigned ksv(const PdosOpl *o, unsigned ch)
{
    return ((unsigned)o->block[ch] << 1) | ((o->fnum[ch] >> (9u - o->nts)) & 1u);
}

static unsigned ksl_att(const PdosOpl *o, unsigned ch)
{
    int a = (int)KSL[o->fnum[ch] >> 6] * 4 - (8 - (int)o->block[ch]) * 32;
    return a < 0 ? 0u : (unsigned)a;
}

/* the envelope: this sample's level from the attenuation so far, then the attenuation's step */
static void slot_envelope(PdosOpl *o, PdosOplSlot *p, unsigned ch)
{
    unsigned level = p->env + ((unsigned)p->tl << 2) + (ksl_att(o, ch) >> KSL_SHIFT[p->ksl]) + (p->am ? o->trem : 0u);
    unsigned reg;
    unsigned rate, hi, lo;
    unsigned shift = 0;
    int env = p->env;
    int inc = 0;
    int off;
    int restart = p->key != 0u && p->stage == RELEASE;
    p->level = (uint16_t)(level > 0x1FFu ? 0x1FFu : level);
    if (restart) {
        reg = p->ar;
    } else if (p->stage == ATTACK) {
        reg = p->ar;
    } else if (p->stage == DECAY) {
        reg = p->dr;
    } else if (p->stage == SUSTAIN) {
        reg = p->egt ? 0u : p->rr;
    } else {
        reg = p->rr;
    }
    p->restart = (uint8_t)restart;
    rate = (ksv(o, ch) >> (p->ksr ? 0u : 2u)) + reg * 4u;
    hi = rate >> 2;
    lo = rate & 3u;
    if (hi > 15u) {
        hi = 15u;
    }
    if (reg != 0u) {
        if (hi < 12u) {
            if (o->eg_odd) {
                unsigned at = hi + o->eg_add;
                shift = at == 12u ? 1u : at == 13u ? (lo >> 1) & 1u : at == 14u ? lo & 1u : 0u;
            }
        } else {
            shift = (hi & 3u) + EG_STEP[lo][o->eg_lo];
            if (shift & 4u) {
                shift = 3u;
            }
            if (shift == 0u) {
                shift = o->eg_odd;
            }
        }
    }
    if (restart && hi == 15u) {
        env = 0;                        /* the fastest attack is instant */
    }
    off = (p->env & 0x1F8u) == 0x1F8u;
    if (p->stage != ATTACK && !restart && off) {
        env = 0x1FF;
    }
    if (p->stage == ATTACK) {
        if (p->env == 0u) {
            p->stage = DECAY;
        } else if (p->key != 0u && shift > 0u && hi != 15u) {
            inc = -(int)((p->env >> (4u - shift)) + 1u);    /* toward 0 by a part of what is left */
        }
    } else if (p->stage == DECAY && (p->env >> 4) == p->sl) {
        p->stage = SUSTAIN;
    } else if (!off && !restart && shift > 0u) {
        inc = 1 << (shift - 1u);
    }
    p->env = (uint16_t)((env + inc) & 0x1FF);
    if (restart) {
        p->stage = ATTACK;
    }
    if (p->key == 0u) {
        p->stage = RELEASE;
    }
}

/* the phase: this sample's position, then the step (vibrato on the F-number) */
static void slot_phase(PdosOpl *o, PdosOplSlot *p, unsigned ch)
{
    unsigned f = o->fnum[ch];
    if (p->vib) {
        int r = (int)((f >> 7) & 7u);
        unsigned v = o->vib_pos;
        if ((v & 3u) == 0u) {
            r = 0;
        } else if (v & 1u) {
            r >>= 1;
        }
        if ((o->bd & 0x40u) == 0u) {
            r >>= 1;                    /* 7 cents unless BDh bit 6 (14) */
        }
        f = (unsigned)((int)f + ((v & 4u) ? -r : r));
    }
    p->pos = (uint16_t)((p->phase >> 9) & 0x3FFu);
    if (p->restart) {
        p->phase = 0;
    }
    p->phase += ((((uint32_t)f << o->block[ch]) >> 1) * MULT[p->mult]) >> 1;
}

/* the rhythm's phases: the hi-hat (slot 13), snare (16) and cymbal (17) from the hi-hat's and cymbal's bits and
 * the noise */
static void slot_rhythm(PdosOpl *o, PdosOplSlot *p, unsigned s)
{
    unsigned x;
    unsigned n = o->noise & 1u;
    if (s == 13u) {
        o->hh2 = (uint8_t)((p->pos >> 2) & 1u);
        o->hh3 = (uint8_t)((p->pos >> 3) & 1u);
        o->hh7 = (uint8_t)((p->pos >> 7) & 1u);
        o->hh8 = (uint8_t)((p->pos >> 8) & 1u);
    }
    if (!rhythm(o)) {
        return;
    }
    if (s == 17u) {
        o->tc3 = (uint8_t)((p->pos >> 3) & 1u);
        o->tc5 = (uint8_t)((p->pos >> 5) & 1u);
    }
    x = (unsigned)((o->hh2 ^ o->hh7) | (o->hh3 ^ o->tc5) | (o->tc3 ^ o->tc5));
    if (s == 13u) {
        p->pos = (uint16_t)((x << 9) | ((x ^ n) ? 0xD0u : 0x34u));
    } else if (s == 16u) {
        p->pos = (uint16_t)(((unsigned)o->hh8 << 9) | ((unsigned)(o->hh8 ^ n) << 8));
    } else if (s == 17u) {
        p->pos = (uint16_t)((x << 9) | 0x80u);
    }
}

/* the operator's output at position `pos` and level `level` (waveforms: sine, half sine, absolute sine, pulse) */
static int16_t wave(unsigned wf, unsigned pos, unsigned level)
{
    unsigned x;
    unsigned q = (pos & 0x100u) ? pdos_fm_logsin[(pos & 0xFFu) ^ 0xFFu] : pdos_fm_logsin[pos & 0xFFu];
    int neg = 0;
    int v;
    pos &= 0x3FFu;
    if (wf == 0u) {
        x = q;
        neg = (pos & 0x200u) != 0u;
    } else if (wf == 1u) {
        x = (pos & 0x200u) ? 0x1000u : q;
    } else if (wf == 2u) {
        x = q;
    } else {
        x = (pos & 0x100u) ? 0x1000u : pdos_fm_logsin[pos & 0xFFu];
    }
    x += level << 3;
    if (x > 0x1FFFu) {
        x = 0x1FFFu;
    }
    v = (int)(((unsigned)pdos_fm_exp[x & 0xFFu] << 1) >> (x >> 8));
    return (int16_t)(neg ? -v - 1 : v);
}

int32_t pdos_opl_sample(PdosOpl *o)
{
    unsigned s;
    int32_t sum = 0;
    for (s = 0; s < 18u; s++) {
        PdosOplSlot *p = &o->slot[s];
        unsigned ch = slot_channel(s);
        int carrier = s % 6u >= 3u;
        int drum = rhythm(o) && ch >= 7u;
        int mod = 0;
        unsigned n;
        p->fb = (int16_t)(o->fb[ch] != 0u ? asr(p->prev + p->out, 9u - o->fb[ch]) : 0);
        p->prev = p->out;
        slot_envelope(o, p, ch);
        slot_phase(o, p, ch);
        slot_rhythm(o, p, s);
        n = ((o->noise >> 14) ^ o->noise) & 1u;
        o->noise = (o->noise >> 1) | (n << 22);
        if (!drum) {
            if (!carrier) {
                mod = p->fb;
            } else if (o->con[ch] == 0u) {
                mod = o->slot[s - 3u].out;
            }
        }
        p->out = wave(o->wse ? p->wf : 0u, (unsigned)(uint16_t)(p->pos + mod), p->level);
    }
    for (s = 0; s < 9u; s++) {
        unsigned m = (s / 3u) * 6u + s % 3u;
        int32_t a = o->slot[m].out;
        int32_t c = o->slot[m + 3u].out;
        if (rhythm(o) && s >= 6u) {
            sum += s == 6u ? 2 * c : 2 * (a + c);
        } else {
            sum += o->con[s] ? a + c : c;
        }
    }
    if ((o->samples & 0x3Fu) == 0x3Fu) {
        o->trem_pos = (uint8_t)((o->trem_pos + 1u) % 210u);
    }
    o->trem = (uint8_t)((o->trem_pos < 105u ? o->trem_pos : 210u - o->trem_pos) >> ((o->bd & 0x80u) ? 2u : 4u));
    if ((o->samples & 0x3FFu) == 0x3FFu) {
        o->vib_pos = (uint8_t)((o->vib_pos + 1u) & 7u);
    }
    o->samples++;
    if (o->eg_odd) {
        unsigned z = 0;
        while (z < 13u && ((o->eg_timer >> z) & 1u) == 0u) {
            z++;
        }
        o->eg_add = (uint8_t)(z > 12u ? 0u : z + 1u);
        o->eg_lo = (uint8_t)(o->eg_timer & 3u);
        o->eg_timer++;
    }
    o->eg_odd ^= 1u;
    return sum > 32767 ? 32767 : sum < -32768 ? -32768 : sum;
}
