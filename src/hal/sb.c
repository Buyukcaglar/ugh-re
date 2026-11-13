/* sb.c - Sound Blaster 2.0 (DSP at 220h, IRQ 7, 8-bit DMA channel 1), its YM3812 (AdLib at
 * 388h/389h, mirrored at 228h/229h), the 8237's channel 1 and the PC speaker, mixed to 44100 Hz
 * stereo for the host.  The Amiga sound (amiga_audio.c) takes the DOS sound's place when chosen.
 * The DSP plays the bytes of the DMA block at the time constant's rate in virtual time; the block's
 * end raises IRQ 7 (single-cycle: once; auto-init: every block). */
#include "hal.h"
#include "opl.h"
#include "../audio/amiga_audio.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
static int g_sblog = -1;

#define SB_IRQ 7
#define OUT_RATE 44100u

/* ---- 8237 channel 1 ---- */
static uint16_t dma_base_addr, dma_base_count, dma_addr, dma_count;
static uint8_t dma_page, dma_mode, dma_masked = 1, dma_ff;

/* ---- DSP ---- */
static uint8_t dsp_out[16];
static int dsp_out_n;
static uint8_t dsp_cmd, dsp_args[4];
static int dsp_need, dsp_have;
static int dsp_reset_state;
static int speaker_on_sb;
static uint32_t sb_rate = 11025u;
static uint16_t sb_block = 0x7FFu;
static int sb_irq_pending;

/* playback */
static int play_active, play_auto;
static uint32_t play_len;          /* bytes in the current block */
static uint64_t play_start;        /* PIT tick when the block began */
static uint64_t play_end;
static uint8_t direct_level = 0x80;

/* PC speaker */
static int spk_on;
static uint32_t spk_div;

/* the YM3812 */
static PdosOpl g_opl;
static uint8_t opl_addr;
static uint64_t opl_next;          /* PIT tick of the next chip sample */
static int32_t opl_prev, opl_cur;  /* the last two chip samples (linear interpolation) */
static uint64_t opl_cur_t;

/* the source the player hears */
static int g_amiga_audio;
static int g_mute;

void hal_set_audio_source(int amiga) { g_amiga_audio = amiga != 0; }
int hal_audio_source(void) { return g_amiga_audio; }
void hal_set_mute(int mute) { g_mute = mute != 0; }

uint8_t adlib_in(uint16_t port)
{
    (void)port;
    return pdos_opl_status(&g_opl, hal_now());
}

unsigned long g_stat_opl_writes, g_stat_sb_blocks;
void adlib_out(uint16_t port, uint8_t v)
{
    g_stat_opl_writes++;
    if ((port & 1u) == 0) opl_addr = v;
    else pdos_opl_write(&g_opl, opl_addr, v, hal_now());
}

/* output */
static uint64_t out_tick_num;      /* the next output sample's time: out_n * PIT_HZ / OUT_RATE */
static uint64_t out_n;
static int16_t outbuf[2048 * 2];
static int outcount;

void speaker_set(int on, uint16_t divisor)
{
    spk_on = on;
    spk_div = divisor ? divisor : 65536u;
}

void sb_reset(void)
{
    dsp_out_n = 0;
    dsp_need = dsp_have = 0;
    dsp_cmd = 0;
    play_active = 0;
    speaker_on_sb = 0;
    sb_irq_pending = 0;
    direct_level = 0x80;
    out_n = 0;
    out_tick_num = 0;
    outcount = 0;
    pdos_opl_reset(&g_opl);
    opl_addr = 0;
    opl_next = 0;
    opl_prev = opl_cur = 0;
    opl_cur_t = 0;
}

static void dsp_push(uint8_t v)
{
    if (dsp_out_n < (int)sizeof dsp_out) dsp_out[dsp_out_n++] = v;
}

static uint32_t dma_phys(void)
{
    return ((uint32_t)dma_page << 16) + dma_addr;
}

static void start_block(uint32_t len, int autoinit)
{
    if (g_sblog < 0) g_sblog = getenv("UGH_SBLOG") != NULL;
    if (g_sblog) {
        uint32_t a = dma_phys(), i, sum = 0;
        for (i = 0; i < len && i < 4096u; i++) sum += g_mem[(a + i) & 0xFFFFFu];
        fprintf(stderr, "sb block len %u auto %d rate %u dma %05x base %04x count %04x mode %02x mask %d avg %u\n",
                len, autoinit, sb_rate, a, dma_base_addr, dma_base_count, dma_mode, dma_masked, len ? sum / (len < 4096u ? len : 4096u) : 0);
    }
    g_stat_sb_blocks++;
    play_active = 1;
    play_auto = autoinit;
    play_len = len;
    play_start = hal_now();
    play_end = play_start + ((uint64_t)len * PIT_HZ + sb_rate - 1u) / sb_rate;
}

static void dsp_exec(void)
{
    switch (dsp_cmd) {
    case 0x10: direct_level = dsp_args[0]; break;
    case 0x14: case 0x91:
        start_block((uint32_t)(dsp_args[0] | (dsp_args[1] << 8)) + 1u, 0);
        break;
    case 0x1C: case 0x90:
        start_block((uint32_t)sb_block + 1u, 1);
        break;
    case 0x40: sb_rate = 1000000u / (256u - dsp_args[0]); if (sb_rate == 0) sb_rate = 4000u; break;
    case 0x48: sb_block = (uint16_t)(dsp_args[0] | (dsp_args[1] << 8)); break;
    case 0xD0: play_active = 0; break;
    case 0xD4: if (play_len) { play_active = 1; play_start = hal_now(); play_end = play_start + ((uint64_t)play_len * PIT_HZ) / sb_rate; } break;
    case 0xD1: speaker_on_sb = 1; break;
    case 0xD3: speaker_on_sb = 0; break;
    case 0xD8: dsp_push(speaker_on_sb ? 0xFF : 0x00); break;
    case 0xDA: play_auto = 0; break;
    case 0xE0: dsp_push((uint8_t)~dsp_args[0]); break;
    case 0xE1: dsp_push(2); dsp_push(1); break;
    case 0xF2: sb_irq_pending = 1; pic_raise(SB_IRQ); break;
    default: break;
    }
}

static int dsp_arg_count(uint8_t cmd)
{
    switch (cmd) {
    case 0x10: case 0x40: case 0xE0: return 1;
    case 0x14: case 0x16: case 0x17: case 0x24: case 0x48: case 0x74: case 0x75: case 0x76: case 0x77: return 2;
    default: return 0;
    }
}

uint8_t sb_in(uint16_t port)
{
    switch (port) {
    case 0x22A:
        if (dsp_out_n > 0) {
            uint8_t v = dsp_out[0];
            memmove(dsp_out, dsp_out + 1, (size_t)(dsp_out_n - 1));
            dsp_out_n--;
            return v;
        }
        return 0xFF;
    case 0x22C: return 0x7F;                           /* ready for a command */
    case 0x228: case 0x229: return adlib_in(port);
    case 0x22E:
        if (sb_irq_pending) { sb_irq_pending = 0; pic_lower(SB_IRQ); }
        return dsp_out_n > 0 ? 0xFF : 0x7F;
    default: return 0xFF;                              /* nothing at the other bases */
    }
}

void sb_out(uint16_t port, uint8_t v)
{
    switch (port) {
    case 0x228: case 0x229: adlib_out(port, v); return;
    case 0x226:
        if (v & 1u) dsp_reset_state = 1;
        else if (dsp_reset_state) {
            dsp_reset_state = 0;
            dsp_out_n = 0;
            dsp_need = dsp_have = 0;
            play_active = 0;
            dsp_push(0xAA);
        }
        return;
    case 0x22C:
        if (dsp_need > dsp_have) {
            dsp_args[dsp_have++] = v;
            if (dsp_have == dsp_need) { dsp_need = 0; dsp_exec(); }
            return;
        }
        dsp_cmd = v;
        dsp_have = 0;
        dsp_need = dsp_arg_count(v);
        if (dsp_need == 0) dsp_exec();
        return;
    default: return;
    }
}

/* ---- the DMA controller (channel 1 only; the others are ignored) ---- */
void dma_out(uint16_t port, uint8_t v)
{
    switch (port) {
    case 0x02:
        dma_base_addr = dma_ff ? (uint16_t)((dma_base_addr & 0xFFu) | (v << 8)) : (uint16_t)((dma_base_addr & 0xFF00u) | v);
        dma_addr = dma_base_addr;
        dma_ff ^= 1u;
        return;
    case 0x03:
        dma_base_count = dma_ff ? (uint16_t)((dma_base_count & 0xFFu) | (v << 8)) : (uint16_t)((dma_base_count & 0xFF00u) | v);
        dma_count = dma_base_count;
        dma_ff ^= 1u;
        return;
    case 0x0A: if ((v & 3u) == 1u) dma_masked = (v & 4u) != 0; return;
    case 0x0B: if ((v & 3u) == 1u) dma_mode = v; return;
    case 0x0C: dma_ff = 0; return;
    case 0x0D: dma_masked = 1; dma_ff = 0; return;
    case 0x0F: dma_masked = (v & 2u) != 0; return;
    case 0x83: dma_page = v; return;
    default: return;
    }
}

/* the bytes of the block played so far */
static uint32_t played(uint64_t now)
{
    uint64_t el;
    if (!play_active || now <= play_start) return 0;
    el = ((now - play_start) * sb_rate) / PIT_HZ;
    return el > play_len ? play_len : (uint32_t)el;
}

uint8_t dma_in(uint16_t port)
{
    uint16_t v;
    switch (port) {
    case 0x02: v = (uint16_t)(dma_addr + played(hal_now())); break;
    case 0x03: v = (uint16_t)(dma_count - played(hal_now())); break;
    case 0x08: return (uint8_t)(play_active ? 0x00 : 0x02);
    default: return 0xFF;
    }
    dma_ff ^= 1u;
    return dma_ff ? (uint8_t)v : (uint8_t)(v >> 8);
}

uint64_t sb_next_event(void)
{
    return play_active ? play_end : UINT64_MAX;
}

static uint8_t dma_byte(uint32_t i)
{
    uint32_t span = (uint32_t)dma_base_count + 1u;
    uint32_t off = (uint32_t)(dma_addr - dma_base_addr) + i;
    if (dma_mode & 0x10u) off %= span;
    else if (off >= span) return 0x80;
    return g_mem[(((uint32_t)dma_page << 16) + (uint16_t)(dma_base_addr + off)) & 0xFFFFFu];
}

static void block_done(void)
{
    uint32_t n = play_len;
    uint32_t span = (uint32_t)dma_base_count + 1u;
    uint32_t pos = (uint32_t)(dma_addr - dma_base_addr) + n;
    if (dma_mode & 0x10u) pos %= span;
    else if (pos > span) pos = span;
    dma_addr = (uint16_t)(dma_base_addr + pos);
    dma_count = (uint16_t)(dma_base_count - pos);
    sb_irq_pending = 1;
    pic_raise(SB_IRQ);
    if (play_auto) {
        play_start = play_end;
        play_end = play_start + ((uint64_t)play_len * PIT_HZ + sb_rate - 1u) / sb_rate;
    } else play_active = 0;
}

static void flush_audio(void)
{
    if (outcount > 0) host_audio(outbuf, outcount);
    outcount = 0;
}

void sb_advance(uint64_t now)
{
    for (;;) {
        uint64_t t = (out_n * PIT_HZ) / OUT_RATE;
        int s = 0;
        if (play_active && play_end <= now && play_end <= t) {
            block_done();
            continue;
        }
        if (t > now) {
            if (play_active && play_end <= now) { block_done(); continue; }
            break;
        }
        if (play_active && t >= play_start && !dma_masked && speaker_on_sb) {
            uint32_t i = (uint32_t)(((t - play_start) * sb_rate) / PIT_HZ);
            if (i < play_len) s = ((int)dma_byte(i) - 128) * 200;
        } else if (direct_level != 0x80 && speaker_on_sb) {
            s = ((int)direct_level - 128) * 160;
        }
        if (spk_on && spk_div > 40u) {
            uint64_t ph = (t % spk_div) * 2u / spk_div;
            s += ph ? 3000 : -3000;
        }
        /* the YM3812: its samples up to t, interpolated at t */
        while (opl_next <= t) {
            opl_prev = opl_cur;
            opl_cur = pdos_opl_sample(&g_opl);
            opl_cur_t = opl_next;
            opl_next += PDOS_OPL_SAMPLE_PIT;
        }
        {
            int64_t frac = (int64_t)(t - opl_cur_t);
            int32_t o = opl_prev + (int32_t)(((int64_t)(opl_cur - opl_prev) * frac) / (int64_t)PDOS_OPL_SAMPLE_PIT);
            s += o * 6;                  /* the card's FM level against its DAC (UGH!'s tunes peak near 1/30 full scale) */
        }
        {   /* the card's output: AC coupling (about 20 Hz) and its low-pass (about 6 kHz) */
            static float hp_x, hp_y, lp;
            float x = (float)s, y = x - hp_x + 0.99715f * hp_y;
            hp_x = x;
            hp_y = y;
            lp += 0.575f * (y - lp);
            s = (int)lp;
        }
        {
            int l = s, r = s;
            int al = 0, ar = 0;
            amiga_audio_sample(&al, &ar);    /* both run all the time, so either can be heard at once */
            if (g_amiga_audio) {
                l = al;
                r = ar;
            }
            if (g_mute) l = r = 0;
            if (l > 32767) l = 32767;
            if (l < -32768) l = -32768;
            if (r > 32767) r = 32767;
            if (r < -32768) r = -32768;
            outbuf[outcount * 2] = (int16_t)l;
            outbuf[outcount * 2 + 1] = (int16_t)r;
        }
        if (++outcount == 2048) flush_audio();
        out_n++;
    }
    if (outcount >= 256) flush_audio();
    (void)dma_phys;
}
