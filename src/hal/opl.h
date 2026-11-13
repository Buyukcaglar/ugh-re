/*
 * pdos_opl.h - the Yamaha YM3812 (OPL2), the FM chip of the AdLib card and of the Sound Blaster's AdLib part, as
 * the DOS AdLib source plays it (OPT-AUDIO-ADLIB-03; docs/dos-audio-adlib-record.md 4).  Nine two-operator
 * channels (or six and five rhythm voices), four waveforms, the two timers and the status register, one sample at
 * a time at the chip's own rate: 3.579545 MHz / 72 = 49,716 Hz, 24 PIT clocks.  The registers, the operator's
 * log-sine and exponent tables, the envelope's rate steps, the phase, feedback, vibrato, tremolo, key scaling and
 * rhythm logic follow the chip as its die was analysed and published (chip fact); docs/dos-audio-adlib-record.md 4
 * names what is inferred.  No floating point; deterministic.  Portable C (DESIGN.md 1).
 */
#ifndef PDOS_OPL_H
#define PDOS_OPL_H

#include <stdint.h>

#define PDOS_OPL_SAMPLE_PIT 24u         /* one sample, 72 clocks of the card's 3.579545 MHz, in PIT clocks */
#define PDOS_OPL_T1_PIT 96u             /* timer 1's step, 80.4 us (four samples) */
#define PDOS_OPL_T2_PIT 384u            /* timer 2's step, 321.8 us */

/* one operator ("slot"); slot s is channel (s / 6) * 3 + s % 3, its modulator when s % 6 < 3; its registers are
 * at offset (s / 6) * 8 + s % 6 of 20h, 40h, 60h, 80h and E0h */
typedef struct PdosOplSlot {
    uint8_t am, vib, egt, ksr, mult;    /* 20h: tremolo, vibrato, sustaining envelope, key scale rate, multiple */
    uint8_t ksl, tl;                    /* 40h: key scale level, total level (0.75 dB) */
    uint8_t ar, dr, sl, rr;             /* 60h, 80h; sl 15 is held as 31, the chip's "off" */
    uint8_t wf;                         /* E0h: the waveform (bits 0-1; sine unless 01h bit 5) */
    uint8_t key;                        /* bit 0: the channel's key on; bit 1: its rhythm bit of BDh */
    uint8_t stage;                      /* attack, decay, sustain, release */
    uint8_t restart;                    /* this sample restarted the attack: the phase returns to 0 */
    uint16_t env;                       /* the envelope's attenuation, 9 bits of 0.1875 dB */
    uint16_t level;                     /* env + TL + KSL + tremolo, at most 1FFh */
    uint32_t phase;                     /* the phase accumulator: bits 9-18 the wave's position */
    uint16_t pos;                       /* this sample's 10-bit position */
    int16_t out, prev;                  /* this and the previous sample's output (13 bits, signed) */
    int16_t fb;                         /* the feedback input (a channel's modulator) */
} PdosOplSlot;

typedef struct PdosOpl {
    PdosOplSlot slot[18];
    uint16_t fnum[9];                   /* A0h, B0h bits 0-1 */
    uint8_t block[9], kon[9];           /* B0h bits 2-4 and 5 */
    uint8_t fb[9], con[9];              /* C0h bits 1-3 and 0 */
    uint8_t wse;                        /* 01h bit 5: the waveform registers in effect */
    uint8_t nts;                        /* 08h bit 6: the key scale's note select */
    uint8_t bd;                         /* BDh: tremolo depth, vibrato depth, rhythm, the five drums */
    uint32_t samples;                   /* samples made (the tremolo's and vibrato's steps) */
    uint8_t trem_pos;                   /* 0..209 */
    uint8_t trem;                       /* the tremolo's attenuation */
    uint8_t vib_pos;                    /* 0..7 */
    uint64_t eg_timer;                  /* the envelope's timer (counts every other sample) */
    uint8_t eg_odd;                     /* the envelope's half step: rates under 12 move on these samples */
    uint8_t eg_add;                     /* 1 + the timer's trailing zero bits (0 past 12) */
    uint8_t eg_lo;                      /* the timer's bits 0-1 */
    uint32_t noise;                     /* the 23-bit noise register */
    uint8_t hh2, hh3, hh7, hh8, tc3, tc5;   /* the hi-hat's and top cymbal's phase bits (rhythm) */
    /* the timers, on the PIT clock */
    uint8_t preset[2], run[2], flag[2], mask[2];
    uint64_t due[2];                    /* the clock of the next overflow (running) */
} PdosOpl;

/* the operator's ROMs (pdos_opl.c), shared with the YM2203 (pdos_opn.c): the quarter-wave log-sine,
 * round(-log2(sin((i + 0.5) pi / 512)) * 256), and the exponent, round(2^((255 - i) / 256) * 1024) */
extern const uint16_t pdos_fm_logsin[256];
extern const uint16_t pdos_fm_exp[256];

/* the chip after power on (or the card's reset): every register 0, every envelope off */
void pdos_opl_reset(PdosOpl *o);

/* a write of register `reg` (the address the card latched) at PIT clock `t` (the timers) */
void pdos_opl_write(PdosOpl *o, uint8_t reg, uint8_t v, uint64_t t);

/* the status register at PIT clock `t`: bit 7 IRQ, 6 timer 1, 5 timer 2, bits 1-2 set (the OPL2's) */
uint8_t pdos_opl_status(PdosOpl *o, uint64_t t);

/* the next sample: the nine channels' sum, clipped to 16 bits */
int32_t pdos_opl_sample(PdosOpl *o);

#endif /* PDOS_OPL_H */
