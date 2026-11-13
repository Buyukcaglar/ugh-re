/* tfmx.c - the Amiga version's TFMX player, translated from its 68000 code (TFMX.obj on the game
 * disk), with the Paula sound chip it writes to.
 *
 * The structure follows the original routine by routine; the comments give the original's
 * addresses (offsets in TFMX.obj's code hunk).  Byte and word sizes, signedness and the order of
 * the steps are kept, so the music and effects come out as the Amiga played them.
 *
 *   tracks   - 8 tracks step through patterns (4-byte entries: notes, waits, jumps), placed by the
 *              trackstep table (16 bytes per step: a pattern and transposition per track).
 *   channels - 4 Paula channels, each running a macro (4-byte commands: sample, length, DMA,
 *              note, volume, envelope, vibrato, portamento ...) started by a note.
 *   effects  - sound effects are notes from the table at mdat+200h, with a priority and lock time. */
#include "tfmx.h"
#include <string.h>

#define EMPTY_PATTERN 0xFFFFFFFFu   /* the player's own "F4000000" pattern (stop) */

/* one Paula channel: the registers and the DMA state */
typedef struct Paula {
    uint32_t lc;            /* AUDxLC (offset in smpl) */
    uint16_t len;           /* AUDxLEN (words) */
    uint16_t per, vol;
    int on;
    uint32_t cur, curlen;   /* the block being played (bytes) */
    uint32_t pos;
    double frac;
} Paula;

/* a channel's player state (a5 = $1158 + 5Ch * n in the original) */
typedef struct Chan {
    uint8_t macrorun;       /* $00 */
    int8_t efxrun;          /* $01 */
    uint8_t addbegintime;   /* $03 */
    uint8_t prevnote;       /* $04 */
    uint8_t currnote;       /* $05 */
    uint8_t velocity;       /* $09 */
    int16_t finetune;       /* $0A */
    uint32_t macroptr;      /* $0C (offset in mdat) */
    uint16_t macrostep;     /* $10 */
    uint16_t macrowait;     /* $12 */
    uint16_t dmaon, dmaoff; /* $14, $16 */
    uint8_t curvol;         /* $18 */
    uint8_t loop;           /* $1A */
    uint8_t addbeginreset;  /* $1B */
    uint8_t envreset, envtime, envend, envrate;   /* $1C..$1F */
    uint8_t vibwidth;       /* $20 */
    uint8_t portareset, portatime;                /* $22, $23 */
    int16_t viboffset;      /* $24 */
    uint8_t vibreset, vibtime;                    /* $26, $27 */
    uint16_t destper;       /* $28 */
    uint32_t curaddr;       /* $2C (offset in smpl) */
    uint16_t portarate;     /* $30 */
    uint16_t portaper;      /* $32 */
    uint16_t curlen;        /* $34 */
    uint8_t keyup;          /* $36 (1 while the key is held) */
    uint32_t retptr;        /* $38 */
    uint8_t lock, prio;     /* $3C, $3D */
    int16_t locktime;       /* $3E */
    uint16_t retstep;       /* $40 */
    uint8_t sfxidx;         /* $42 */
    int32_t addbegin;       /* $50 */
    uint32_t pending;       /* $54 (a sound effect's note, started next frame) */
    uint16_t curper;        /* $58 */
    uint8_t waitflag;       /* $5A */
    Paula *hw;              /* $4C */
} Chan;

typedef struct Track {
    uint16_t mute;          /* $08 */
    uint32_t paddr;         /* $28 */
    uint8_t pnum, pxpose, ploop;   /* $48, $49, $4A */
    uint16_t pstep;         /* $68 */
    uint8_t pwait;          /* $6A */
    uint32_t raddr;         /* $88 */
    uint16_t rstep;         /* $A8 */
} Track;

static const uint16_t g_periods[64] = {   /* $15EE */
    0x6AE, 0x64E, 0x5F4, 0x59E, 0x54D, 0x501, 0x4B9, 0x475, 0x435, 0x3F9, 0x3C0, 0x38C,
    0x358, 0x32A, 0x2FC, 0x2D0, 0x2A8, 0x282, 0x25E, 0x23B, 0x21B, 0x1FD, 0x1E0, 0x1C6,
    0x1AC, 0x194, 0x17D, 0x168, 0x154, 0x140, 0x12F, 0x11E, 0x10E, 0x0FE, 0x0F0, 0x0E3,
    0x0D6, 0x0CA, 0x0BF, 0x0B4, 0x0AA, 0x0A0, 0x097, 0x08F, 0x087, 0x07F, 0x078, 0x071,
    0x0D6, 0x0CA, 0x0BF, 0x0B4, 0x0AA, 0x0A0, 0x097, 0x08F, 0x087, 0x07F, 0x078, 0x071,
    0x0D6, 0x0CA, 0x0BF, 0x0B4
};

static struct {
    const uint8_t *mdat;
    uint8_t *smpl;
    uint32_t mlen, slen;
    uint32_t trackstep, pattab, macrotab, sfxtab;   /* $26, $2A, $2E, $32 */
    uint8_t restart;        /* $09 */
    int8_t cursong;         /* $0C */
    int8_t fadedir;         /* $0D */
    int16_t speedcnt;       /* $16 */
    uint8_t on;             /* $18 */
    uint8_t cont;           /* $1A */
    uint8_t song;           /* $1B */
    uint16_t track7sfx;     /* $1C */
    uint8_t mastervol, fadeto, fadecnt, fadespeed;  /* $1E..$21 */
    int16_t tsloop;         /* $24 */
    uint16_t dmaon, dmaoff; /* $36, $38 */
    uint16_t fadebusy;      /* $15C0 */
    uint8_t songflag;       /* $15D5 */
    uint16_t first, last, curr, prescale;           /* $1438.. */
    Track trk[8];
    uint16_t savepos[32];   /* $1500 */
    uint8_t savetempo[32];  /* $1541 */
    Chan ch[4];
    Paula hw[4];
    int ready;
    double tickfrac;
    float lp_l, lp_r;
} T;

/* ---- the module's data, read with bounds checks ---- */
static uint8_t m8(uint32_t a) { return a < T.mlen ? T.mdat[a] : 0; }
static uint16_t m16(uint32_t a) { return (uint16_t)((m8(a) << 8) | m8(a + 1)); }
static uint32_t m32(uint32_t a) { return ((uint32_t)m16(a) << 16) | m16(a + 2); }

static uint32_t pattern_addr(unsigned n) { return m32(T.pattab + n * 4u); }
static uint32_t macro_addr(unsigned n) { return m32(T.macrotab + n * 4u); }

/* ---- Paula ---- */
static void dmacon(uint16_t v)
{
    int i;
    for (i = 0; i < 4; i++) {
        if (!(v & (1u << i))) continue;
        if (v & 0x8000u) {
            if (!T.hw[i].on) {
                Paula *p = &T.hw[i];
                p->on = 1;
                p->cur = p->lc;
                p->curlen = (p->len ? p->len : 0x10000u) * 2u;
                p->pos = 0;
                p->frac = 0;
            }
        } else {
            T.hw[i].on = 0;
        }
    }
}

/* ---- NotePort ($B8A): a note or command for a channel ---- */
static void noteport(uint32_t x)
{
    uint8_t b0 = (uint8_t)(x >> 24), b1 = (uint8_t)(x >> 16), b2 = (uint8_t)(x >> 8), b3 = (uint8_t)x;
    Chan *c = &T.ch[b2 & 3];   /* the table at $1118 repeats the four channels */
    if (b0 == 0xFC) {           /* lock */
        c->lock = b1;
        c->locktime = b3;
        return;
    }
    if (c->lock) return;
    if (b0 < 0x80) goto start;
    if (b0 == 0xF7) {           /* envelope */
        c->envrate = b1;
        c->envreset = c->envtime = (uint8_t)((b2 >> 4) + 1);
        c->envend = b3;
        return;
    }
    if (b0 == 0xF6) {           /* vibrato */
        c->vibreset = (uint8_t)(b1 & 0xFE);
        c->vibtime = (uint8_t)(c->vibreset >> 1);
        c->vibwidth = b3;
        c->viboffset = 0;
        return;
    }
    if (b0 == 0xF5) {           /* key up */
        c->keyup = 0;
        return;
    }
    if (b0 >= 0xBF) {           /* portamento to a note ($C9A) */
        c->portareset = b1;
        c->portatime = 1;
        if (!c->portarate) c->portaper = c->destper;
        c->portarate = b3;
        c->currnote = (uint8_t)(b0 & 0x3F);
        c->destper = g_periods[c->currnote];
        return;
    }
start:                          /* a note: start its macro ($C3A) */
    c->finetune = (int8_t)b3;
    c->velocity = (uint8_t)((b2 >> 4) & 0x0F);
    c->prevnote = c->currnote;
    c->currnote = b0;
    c->macroptr = macro_addr(b1);
    c->macrostep = 0;
    c->macrowait = 0;
    c->efxrun = 0;
    c->loop = 0xFF;
    c->macrorun = 0xFF;
    c->keyup = 1;
}

/* channel off ($CD6) */
static void channel_off(unsigned n)
{
    Chan *c = &T.ch[n & 3];
    if (c->lock) return;
    dmacon(c->dmaoff);
    c->macrorun = 0;
}

static void fade_to(uint8_t speed, uint8_t vol)
{
    T.fadebusy = 1;
    T.fadeto = vol;
    T.fadecnt = T.fadespeed = speed;
    if (speed == 0) {
        T.mastervol = T.fadeto;
        T.fadedir = 0;
        T.fadebusy = 0;
        return;
    }
    T.fadedir = 1;
    if (T.mastervol == T.fadeto) {
        T.fadedir = 0;
        T.fadebusy = 0;
    } else if (T.mastervol > T.fadeto) {
        T.fadedir = -1;
    }
}

/* ---- the trackstep ($3F8) ---- */
static void get_trackstep(void)
{
    int guard;
    for (guard = 0; guard < 4096; guard++) {
        uint32_t a = T.trackstep + (uint32_t)T.curr * 16u;
        uint16_t w = m16(a);
        unsigned i;
        if (w == 0xEFFE) {
            uint16_t cmd = m16(a + 2), p1 = m16(a + 4), p2 = m16(a + 6);
            if (cmd >= 5) cmd = 0;
            switch (cmd) {
            case 0:                 /* stop */
                T.on = 0;
                return;
            case 1:                 /* loop */
                if (T.tsloop == 0) {
                    T.tsloop = -1;
                    T.curr++;
                    continue;
                }
                if (T.tsloop < 0) T.tsloop = (int16_t)(p2 - 1);
                else T.tsloop--;
                T.curr = p1;
                continue;
            case 2:                 /* speed */
                T.prescale = p1;
                T.speedcnt = (int16_t)p1;
                T.curr++;
                continue;
            default:                /* fade */
                T.curr++;
                if (T.fadebusy) continue;
                fade_to((uint8_t)p1, (uint8_t)p2);
                continue;
            }
        }
        for (i = 0; i < 8; i++) {
            Track *t = &T.trk[i];
            uint16_t v = m16(a + i * 2u);
            if (i == 7 && T.track7sfx) break;
            t->pnum = (uint8_t)(v >> 8);
            t->pxpose = (uint8_t)v;
            if (!(v & 0x8000u)) {
                t->paddr = pattern_addr(t->pnum);
                t->pstep = 0;
                t->pwait = 0;
                t->ploop = 0xFF;
            }
        }
        return;
    }
}

/* ---- one track ($19E); sets T.restart when the trackstep moved ---- */
static void do_track(unsigned n)
{
    Track *t = &T.trk[n];
    int guard;
    if (t->pnum >= 0x90) {
        if (t->pnum == 0xFE) {
            t->pnum = 0xFF;
            if (!t->mute) channel_off(t->pxpose);
        }
        return;
    }
    T.songflag = 0xFF;
    if (t->pwait) {
        t->pwait--;
        return;
    }
    for (guard = 0; guard < 4096; guard++) {
        uint32_t x = t->paddr == EMPTY_PATTERN ? 0xF4000000u : m32(t->paddr + (uint32_t)t->pstep * 4u);
        uint8_t b0 = (uint8_t)(x >> 24), b1 = (uint8_t)(x >> 16), b2 = (uint8_t)(x >> 8), b3 = (uint8_t)x;
        uint16_t w1 = (uint16_t)x;
        if (b0 < 0xF0) {                    /* a note */
            uint8_t d7 = b0, d0;
            int waits = d7 < 0xC0 && d7 >= 0x7F;
            if (waits) {
                t->pwait = b3;
                x &= 0xFFFFFF00u;
            }
            d0 = (uint8_t)(b0 + t->pxpose);
            if (d7 < 0xC0) d0 &= 0x3F;
            x = (x & 0x00FFFFFFu) | ((uint32_t)d0 << 24);
            if (!t->mute) noteport(x);
            t->pstep++;
            if (waits) return;
            continue;
        }
        switch (b0 & 0x0F) {
        case 0x0:                           /* end: the next trackstep */
            t->pnum = 0xFF;
            if (T.curr == T.last) T.curr = T.first;
            else T.curr++;
            get_trackstep();
            T.restart = 0xFF;
            return;
        case 0x1:                           /* loop */
            if (t->ploop == 0) {
                t->ploop = 0xFF;
                t->pstep++;
                continue;
            }
            if (t->ploop == 0xFF) t->ploop = (uint8_t)(b1 - 1);
            else t->ploop--;
            t->pstep = w1;
            continue;
        case 0x2:                           /* continue in another pattern */
            t->pnum = b1;
            t->paddr = pattern_addr(b1);
            t->pstep = w1;
            continue;
        case 0x3:                           /* wait */
            t->pwait = b1;
            t->pstep++;
            return;
        case 0xE:                           /* stop the sound-effect pattern */
            T.track7sfx = 0;
            /* fall through */
        case 0x4:                           /* stop */
            t->pnum = 0xFF;
            return;
        case 0x5: case 0x6: case 0x7: case 0xC:   /* key up, vibrato, envelope, lock */
            if (!t->mute) noteport(x);
            t->pstep++;
            continue;
        case 0x8:                           /* gosub pattern */
            t->raddr = t->paddr;
            t->rstep = t->pstep;
            t->pnum = b1;
            t->paddr = pattern_addr(b1);
            t->pstep = w1;
            continue;
        case 0x9:                           /* return: the original reads track 0's saved place */
            T.trk[0].paddr = T.trk[0].raddr;
            T.trk[0].pstep = T.trk[0].rstep;
            t->pstep++;
            continue;
        case 0xA:                           /* fade */
            t->pstep++;
            if (T.fadebusy) continue;
            fade_to(b1, b3);
            continue;
        case 0xB: {                         /* play a pattern on another track */
            Track *o = &T.trk[b2 & 7];
            o->pnum = b1;
            o->pxpose = b3;
            o->paddr = pattern_addr(b1 & 0x7Fu);
            o->pstep = 0;
            o->pwait = 0;
            o->ploop = 0xFF;
            t->pstep++;
            continue;
        }
        default:                            /* D, F: nothing */
            t->pstep++;
            continue;
        }
    }
}

/* ---- the tracks, every prescale+1 frames ($110) ---- */
static void do_tracks(void)
{
    unsigned i;
    int guard;
    if (--T.speedcnt >= 0) return;
    T.speedcnt = (int16_t)T.prescale;
    for (i = 0; i < 8; i++) T.trk[i].mute = m16(0x1C0u + i * 2u);
    for (guard = 0; guard < 64; guard++) {
        T.restart = 0;
        for (i = 0; i < 8; i++) {
            do_track(i);
            if (T.restart) break;
        }
        if (!T.restart) break;
    }
}

/* ---- the effects of a channel, the fade and the volume ($A0C..$B88) ---- */
static void effects(Chan *c)
{
    Paula *hw = c->hw;
    uint32_t d0;
    if (c->efxrun < 0) goto volume;
    if (c->efxrun == 0) {
        c->efxrun = 1;
        goto volume;
    }
    if (c->addbegintime) {
        c->curaddr += (uint32_t)c->addbegin;
        hw->lc = c->curaddr;
        if (--c->addbegintime == 0) {
            c->addbegintime = c->addbeginreset;
            c->addbegin = -c->addbegin;
        }
    }
    if (c->vibreset) {
        c->viboffset = (int16_t)(c->viboffset + (int8_t)c->vibwidth);
        d0 = c->destper;
        if (c->viboffset != 0) d0 = ((d0 * (uint16_t)(c->viboffset + 0x800)) << 5) >> 16;
        if (!c->portarate) c->curper = (uint16_t)d0;
        if (--c->vibtime == 0) {
            c->vibtime = c->vibreset;
            c->vibwidth = (uint8_t)(-(int8_t)c->vibwidth);
        }
    }
    if (c->portarate && --c->portatime == 0) {
        uint16_t d1 = c->destper;
        c->portatime = c->portareset;
        d0 = c->portaper;
        if (d0 == d1) goto porta_done;
        if (d0 < d1) {
            d0 = (d0 * (uint16_t)(c->portarate + 0x100)) >> 8;
            if ((uint16_t)d0 >= d1) goto porta_done;
            goto porta_set;
        }
        d0 = (d0 * (uint16_t)(0x100 - c->portarate)) >> 8;
        if ((uint16_t)d0 == d1) goto porta_done;
        if ((uint16_t)d0 > d1) goto porta_set;
porta_done:
        c->portarate = 0;
        d0 = c->destper;
porta_set:
        d0 &= 0x7FF;
        c->portaper = (uint16_t)d0;
        c->curper = (uint16_t)d0;
    }
    if (c->envreset) {
        if (c->envtime) {
            c->envtime--;
        } else {
            int8_t end = (int8_t)c->envend;
            c->envtime = c->envreset;
            if (end > (int8_t)c->curvol) {
                c->curvol = (uint8_t)(c->curvol + c->envrate);
                if (end <= (int8_t)c->curvol) goto env_end;
            } else {
                c->curvol = (uint8_t)(c->curvol - c->envrate);
                if ((int8_t)c->curvol < 0 || end >= (int8_t)c->curvol) goto env_end;
            }
            goto volume;
env_end:
            c->curvol = c->envend;
            c->envreset = 0;
        }
    }
volume:
    if (T.fadedir && --T.fadecnt == 0) {   /* once per channel, as in the original */
        T.fadecnt = T.fadespeed;
        T.mastervol = (uint8_t)(T.mastervol + T.fadedir);
        if (T.fadeto == T.mastervol) {
            T.fadedir = 0;
            T.fadebusy = 0;
        }
    }
    {
        unsigned v = c->curvol;
        if (!(T.mastervol & 0x40)) v = (uint16_t)(v * 4u * T.mastervol) >> 8;
        hw->vol = (uint16_t)v;
    }
}

/* ---- a channel's macro ($5E2) ---- */
static void do_channel(Chan *c)
{
    Paula *hw = c->hw;
    int guard;
    if (c->locktime >= 0) c->locktime--;
    else {
        c->lock = 0;
        c->prio = 0;
    }
    if (c->pending) {
        uint32_t p = c->pending;
        c->pending = 0;
        c->lock = 0;
        noteport(p);
        c->lock = c->prio;
    }
    if (!c->macrorun) goto fx;
    if (c->macrowait) {
        c->macrowait--;
        goto fx;
    }
    for (guard = 0; guard < 1024; guard++) {
        uint32_t x = m32(c->macroptr + (uint32_t)c->macrostep * 4u);
        uint8_t cmd = (uint8_t)(x >> 24), b1 = (uint8_t)(x >> 16), b2 = (uint8_t)(x >> 8), b3 = (uint8_t)x;
        uint16_t w1 = (uint16_t)x;
        x &= 0x00FFFFFFu;
        switch (cmd) {
        case 0x00:                          /* DMA off and reset the effects */
            c->envreset = 0;
            c->vibreset = 0;
            c->portarate = 0;
            /* fall through */
        case 0x13:                          /* DMA off */
            c->macrostep++;
            if (b1 == 0) {
                dmacon(c->dmaoff);
                continue;
            }
            T.dmaoff |= c->dmaoff;
            c->waitflag = 0;
            goto fx;
        case 0x01:                          /* DMA on */
            c->efxrun = (int8_t)b1;
            c->macrostep++;
            T.dmaon |= c->dmaon;
            continue;
        case 0x02:                          /* sample start */
            c->addbegintime = 0;
            c->curaddr = x;
            hw->lc = c->curaddr;
            c->macrostep++;
            continue;
        case 0x03:                          /* sample length */
            c->curlen = w1;
            hw->len = w1;
            c->macrostep++;
            continue;
        case 0x04:                          /* wait */
            c->macrowait = w1;
            goto wait;
        case 0x05: case 0x1B: case 0x1C: case 0x1D: case 0x1E:
loop:                                       /* loop */
            if (c->loop == 0) {
                c->loop = 0xFF;
                c->macrostep++;
                continue;
            }
            if (c->loop == 0xFF) c->loop = (uint8_t)(b1 - 1);
            else c->loop--;
            c->macrostep = w1;
            continue;
        case 0x15:                          /* gosub macro */
            c->retptr = c->macroptr;
            c->retstep = c->macrostep;
            /* fall through */
        case 0x06:                          /* continue in another macro */
            c->macroptr = macro_addr(b1 & 0x7Fu);
            c->macrostep = w1;
            c->loop = 0xFF;
            continue;
        case 0x07:                          /* stop */
            c->macrorun = 0;
            goto fx;
        case 0x08: case 0x09: case 0x1F: {  /* add note, set note, previous note */
            uint8_t base = cmd == 0x08 ? c->currnote : cmd == 0x1F ? c->prevnote : 0;
            uint32_t per = g_periods[(uint8_t)(b1 + base) & 0x3F];
            uint16_t ft = (uint16_t)(c->finetune + w1);
            if (ft != 0) per = (per * (uint16_t)(ft + 0x100)) >> 8;
            c->destper = (uint16_t)per;
            if (!c->portarate) c->curper = (uint16_t)per;
            goto wait;
        }
        case 0x0A:                          /* clear the effects */
            c->addbegintime = 0;
            c->envreset = 0;
            c->vibreset = 0;
            c->portarate = 0;
            c->macrostep++;
            continue;
        case 0x0B:                          /* portamento */
            c->portareset = b1;
            c->portatime = 1;
            if (!c->portarate) c->portaper = c->destper;
            c->portarate = w1;
            c->macrostep++;
            continue;
        case 0x0C:                          /* vibrato */
            c->vibreset = b1;
            c->vibtime = (uint8_t)(b1 >> 1);
            c->vibwidth = b3;
            if (!c->portarate) {
                c->curper = c->destper;
                c->viboffset = 0;
            }
            c->macrostep++;
            continue;
        case 0x0D:                          /* add volume (to 3 x the note's velocity) */
            c->curvol = (uint8_t)(c->velocity * 3u + w1);
            c->macrostep++;
            continue;
        case 0x0E:                          /* set volume */
            c->curvol = b3;
            c->macrostep++;
            continue;
        case 0x0F:                          /* envelope */
            c->envreset = b2;
            c->envrate = b1;
            c->envtime = b2;
            c->envend = b3;
            c->macrostep++;
            continue;
        case 0x10:                          /* loop while the key is held */
            if (c->keyup) goto loop;
            c->macrostep++;
            continue;
        case 0x11:                          /* add to the sample start, repeatedly */
            c->addbegintime = c->addbeginreset = b1;
            c->addbegin = (int16_t)w1;
            c->curaddr += (uint32_t)c->addbegin;
            hw->lc = c->curaddr;
            c->macrostep++;
            continue;
        case 0x12:                          /* add to the sample length */
            c->curlen = (uint16_t)(c->curlen + w1);
            hw->len = c->curlen;
            c->macrostep++;
            continue;
        case 0x14:                          /* wait for the key's release */
            if (!c->keyup) {
                c->macrostep++;
                continue;
            }
            if (c->loop == 0) {
                c->loop = 0xFF;
                c->macrostep++;
                continue;
            }
            if (c->loop == 0xFF) c->loop = (uint8_t)(b3 - 1);
            else c->loop--;
            goto fx;
        case 0x16:                          /* return from a gosub */
            c->macroptr = c->retptr;
            c->macrostep = (uint16_t)(c->retstep + 1u);
            continue;
        case 0x17:                          /* set the period */
            c->destper = w1;
            if (!c->portarate) c->curper = w1;
            c->macrostep++;
            continue;
        case 0x18:                          /* sample loop: the start moves on, the length shrinks */
            c->curaddr += x;
            hw->lc = c->curaddr;
            c->curlen = (uint16_t)(c->curlen - (w1 >> 1));
            hw->len = c->curlen;
            c->macrostep++;
            continue;
        case 0x19:                          /* one-shot: the silent word at the start of smpl */
            c->addbegintime = 0;
            c->curaddr = 0;
            hw->lc = 0;
            c->curlen = 1;
            hw->len = 1;
            c->macrostep++;
            continue;
        case 0x21:                          /* play a macro (a note on a channel) */
            noteport(((uint32_t)c->currnote << 24) | (x & 0x00FFFFFFu) | ((uint32_t)(c->velocity & 0x0F) << 12));
            c->macrostep++;
            continue;
        default:
            if (cmd < 0x2A) {               /* 1A, 20, 22..29: nothing in this version */
                c->macrostep++;
                continue;
            }
            goto wait;
        }
wait:                                       /* ($6FC) */
        if (c->waitflag) {
            c->macrostep++;
            goto fx;
        }
        c->waitflag = 0xFF;
        c->macrostep++;
    }
fx:
    effects(c);
}

/* ---- the vertical-blank routine ($64) ---- */
static void tick(void)
{
    int i;
    if (T.dmaoff) {
        dmacon(T.dmaoff);
        for (i = 0; i < 4; i++)
            if (T.dmaoff & (1u << i)) T.hw[i].per = 9;
        T.dmaoff = 0;
    }
    if (!T.on) return;
    for (i = 0; i < 4; i++) do_channel(&T.ch[i]);
    if (T.cursong >= 0) do_tracks();
    for (i = 0; i < 4; i++) T.hw[i].per = T.ch[i].curper;
    dmacon(T.dmaon);
    T.dmaon = 0;
}

/* ---- the entry points ---- */
void tfmx_stop(void)                /* $E4C */
{
    int i;
    T.on = 0;
    T.dmaon = 0;
    for (i = 0; i < 4; i++) {
        T.ch[i].macrorun = 0;
        T.ch[i].lock = 0;
        T.ch[i].prio = 0;
        T.ch[i].locktime = 0;
        T.hw[i].vol = 0;
    }
    dmacon(0x000F);
    T.songflag = 0;
}

static void start_song(void)        /* $ECA */
{
    unsigned s, i;
    uint16_t tempo;
    tfmx_stop();
    T.on = 0;
    T.track7sfx = 0;
    s = T.song & 0x1Fu;
    if (T.cursong >= 0) {
        unsigned k = (uint8_t)T.cursong & 0x1Fu;
        T.savepos[k] = T.curr;
        T.savetempo[k] = (uint8_t)T.prescale;
    }
    T.curr = T.first = m16(0x100u + s * 2u);
    T.last = m16(0x140u + s * 2u);
    tempo = m16(0x180u + s * 2u);
    if (T.cont & 1) {
        T.curr = T.savepos[s];
        tempo = T.savetempo[s];
    }
    for (i = 0; i < 8; i++) {
        T.trk[i].paddr = EMPTY_PATTERN;
        T.trk[i].pnum = 0xFF;
        T.trk[i].pxpose = 0;
        T.trk[i].pstep = 0;
        T.trk[i].pwait = 0;
    }
    T.prescale = tempo;
    if (!(T.song & 0x80)) get_trackstep();
    T.restart = 0;
    T.speedcnt = 0;
    T.tsloop = -256;                 /* st.b on the high byte: negative */
    T.cursong = (int8_t)T.song;
    T.cont = 0;
    T.dmaon = 0;
    T.fadebusy = 0;
    T.songflag = 0;
    T.on = 1;
}

void tfmx_song(int song)
{
    if (!T.ready) return;
    T.song = (uint8_t)song;
    start_song();
}

void tfmx_note(uint32_t note)
{
    if (T.ready) noteport(note);
}

void tfmx_fade(int speed, int vol)
{
    if (!T.ready) return;
    fade_to((uint8_t)speed, (uint8_t)vol);
}

void tfmx_fx(int fx)                /* $DB4 */
{
    uint32_t e;
    uint8_t chn, prio, idx;
    Chan *c;
    if (!T.ready) return;
    e = T.sfxtab + ((uint32_t)fx & 0x1FFFu) * 8u;
    idx = (uint8_t)(fx << 3);
    if (m8(e) == 0xFB) {            /* a pattern on track 7 ($D52) */
        Track *t = &T.trk[7];
        uint16_t v = m16(e + 2);
        T.track7sfx = 1;
        t->pnum = (uint8_t)(v >> 8);
        t->pxpose = (uint8_t)v;
        t->paddr = pattern_addr(t->pnum);
        t->pstep = 0;
        t->pwait = 0;
        t->ploop = 0xFF;
        return;
    }
    chn = m8(e + 2);
    if (T.cursong < 0) chn = m8(e + 4);
    chn &= 0x0F;
    c = &T.ch[chn & 3];
    prio = (uint8_t)(m8(e + 5) & 0x7F);
    if (prio < c->prio && c->locktime >= 0) return;
    if (c->sfxidx == idx && c->locktime >= 0 && (m8(e + 5) & 0x80)) return;
    c->pending = (m32(e) & 0xFFFFF0FFu) | ((uint32_t)chn << 8);
    c->prio = prio;
    c->locktime = (int16_t)m16(e + 6);
    c->sfxidx = idx;
}

int tfmx_playing(void)
{
    return T.ready && T.on;
}

int tfmx_init(const uint8_t *mdat, size_t mdat_len, uint8_t *smpl, size_t smpl_len)   /* $F9E */
{
    static const uint16_t on[4] = { 0x8201, 0x8202, 0x8204, 0x8208 };
    int i;
    memset(&T, 0, sizeof T);
    if (mdat == NULL || smpl == NULL || mdat_len < 0x200 || smpl_len < 4 || memcmp(mdat, "TFMX-SONG", 9) != 0)
        return 0;
    T.mdat = mdat;
    T.mlen = (uint32_t)mdat_len;
    T.smpl = smpl;
    T.slen = (uint32_t)smpl_len;
    memset(smpl, 0, 4);
    T.mastervol = T.fadeto = 0x40;
    T.trackstep = m32(0x1D0);
    T.pattab = m32(0x1D4);
    T.macrotab = m32(0x1D8);
    if (T.trackstep == 0) T.trackstep = 0x800;   /* the older modules' fixed places */
    if (T.pattab == 0) T.pattab = 0x400;
    if (T.macrotab == 0) T.macrotab = 0x600;
    T.sfxtab = 0x200;
    T.prescale = 5;
    for (i = 0; i < 32; i++) T.savetempo[i] = 5;
    for (i = 0; i < 4; i++) {
        T.ch[i].hw = &T.hw[i];
        T.ch[i].dmaon = on[i];
        T.ch[i].dmaoff = (uint16_t)(on[i] & 0x000F);
        T.ch[i].loop = 0xFF;
    }
    for (i = 0; i < 8; i++) {
        T.trk[i].paddr = EMPTY_PATTERN;
        T.trk[i].pnum = 0xFF;
    }
    T.cursong = 0;
    T.ready = 1;
    return 1;
}

/* ---- the output: 50 Hz frames, Paula at the PAL clock, the A500's fixed low-pass ---- */
#define OUT_RATE 44100.0
#define PAULA_CLOCK 3546895.0
#define FRAME_RATE 50.0

static int paula_sample(Paula *p)
{
    int8_t s;
    if (!p->on || p->per == 0) return 0;
    s = (p->cur + p->pos) < T.slen ? (int8_t)T.smpl[p->cur + p->pos] : 0;
    p->frac += PAULA_CLOCK / ((double)(p->per < 113 ? 113 : p->per) * OUT_RATE);
    while (p->frac >= 1.0) {
        p->frac -= 1.0;
        if (++p->pos >= p->curlen) {   /* the block ends: the registers are loaded again */
            p->cur = p->lc;
            p->curlen = (p->len ? p->len : 0x10000u) * 2u;
            p->pos = 0;
        }
    }
    return (int)s * (p->vol > 64 ? 64 : p->vol);
}

void tfmx_render(int *left, int *right)
{
    int a, b, c, d;
    float l, r;
    if (!T.ready) {
        *left = *right = 0;
        return;
    }
    T.tickfrac += FRAME_RATE / OUT_RATE;
    if (T.tickfrac >= 1.0) {
        T.tickfrac -= 1.0;
        tick();
    }
    a = paula_sample(&T.hw[0]);
    b = paula_sample(&T.hw[1]);
    c = paula_sample(&T.hw[2]);
    d = paula_sample(&T.hw[3]);
    /* channels 0 and 3 are left, 1 and 2 right; a little of each side goes to the other */
    l = (float)(a + d) * 0.8f + (float)(b + c) * 0.2f;
    r = (float)(b + c) * 0.8f + (float)(a + d) * 0.2f;
    T.lp_l += 0.5f * (l - T.lp_l);
    T.lp_r += 0.5f * (r - T.lp_r);
    *left = (int)(T.lp_l * 1.6f);
    *right = (int)(T.lp_r * 1.6f);
}
