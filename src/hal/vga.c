/* vga.c - the VGA: four 64 KB planes, the sequencer, graphics and attribute controllers, the DAC and
 * the CRTC registers that shape the picture (start address, offset, line compare, panning, scan
 * doubling).  The modes the program sets are 03h (text, shown black), 0Dh (320x200x16), 12h
 * (640x480x16) and 13h (320x200x256); the picture is built from the registers, not from the mode
 * number, so the program's own register changes take effect. */
#include "hal.h"
#include <stdio.h>
#include <string.h>

static uint8_t planes[4][65536];
static uint8_t latch[4];
static uint8_t seq[8], seq_idx;
static uint8_t gc[16], gc_idx;
static uint8_t crtc[32], crtc_idx;
static uint8_t attr[32], attr_idx, attr_ff;
static uint8_t misc_out;
static uint8_t dac[256][3], dac_mask = 0xFF;
static uint8_t dac_widx, dac_ridx, dac_wsub, dac_rsub, dac_state;
static uint16_t latched_start;
int g_vga_retrace, g_vga_hblank;
static int poll_count;

static uint32_t fb[640 * 480];

/* ---- modes ---- */
typedef struct ModeRegs {
    uint8_t misc, seq[5], crtc[25], gc[9], attr[21];
} ModeRegs;

static const ModeRegs mode03 = {
    0x67, {0x00, 0x00, 0x03, 0x00, 0x02},
    {0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F, 0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x00,
     0x9C, 0x8E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3, 0xFF},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF},
    {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
     0x0C, 0x00, 0x0F, 0x08, 0x00}};
static const ModeRegs mode0d = {
    0x63, {0x03, 0x09, 0x0F, 0x00, 0x06},
    {0x2D, 0x27, 0x28, 0x90, 0x2B, 0x80, 0xBF, 0x1F, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x9C, 0x8E, 0x8F, 0x14, 0x00, 0x96, 0xB9, 0xE3, 0xFF},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x0F, 0xFF},
    {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
     0x01, 0x00, 0x0F, 0x00, 0x00}};
static const ModeRegs mode12 = {
    0xE3, {0x03, 0x01, 0x0F, 0x00, 0x06},
    {0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0x0B, 0x3E, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0xEA, 0x8C, 0xDF, 0x28, 0x00, 0xE7, 0x04, 0xE3, 0xFF},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x0F, 0xFF},
    {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
     0x01, 0x00, 0x0F, 0x00, 0x00}};
static const ModeRegs mode13 = {
    0x63, {0x03, 0x01, 0x0F, 0x00, 0x0E},
    {0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, 0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x9C, 0x8E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3, 0xFF},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF},
    {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
     0x41, 0x00, 0x0F, 0x00, 0x00}};

/* the 64 EGA colours (rgbRGB) in the DAC, as the BIOS loads them for the 16-colour modes */
static void dac_ega(void)
{
    int i;
    for (i = 0; i < 64; i++) {
        int r = ((i >> 2) & 1) * 0x2A + ((i >> 5) & 1) * 0x15;
        int g = ((i >> 1) & 1) * 0x2A + ((i >> 4) & 1) * 0x15;
        int b = (i & 1) * 0x2A + ((i >> 3) & 1) * 0x15;
        dac[i][0] = (uint8_t)r;
        dac[i][1] = (uint8_t)g;
        dac[i][2] = (uint8_t)b;
    }
}

void vga_set_mode(int mode)
{
    const ModeRegs *m = mode == 0x0D ? &mode0d : mode == 0x12 ? &mode12 : mode == 0x13 ? &mode13 : &mode03;
    misc_out = m->misc;
    memset(seq, 0, sizeof seq);
    memset(crtc, 0, sizeof crtc);
    memset(gc, 0, sizeof gc);
    memset(attr, 0, sizeof attr);
    memcpy(seq, m->seq, 5);
    memcpy(crtc, m->crtc, 25);
    memcpy(gc, m->gc, 9);
    memcpy(attr, m->attr, 21);
    attr_idx = 0x20;
    attr_ff = 0;
    memset(planes, 0, sizeof planes);
    dac_mask = 0xFF;
    memset(dac, 0, sizeof dac);
    dac_ega();
    if (mode == 0x13) {
        int i;
        for (i = 16; i < 32; i++) dac[i][0] = dac[i][1] = dac[i][2] = (uint8_t)((i - 16) * 4 + 3);
    }
    latched_start = 0;
}

void vga_reset(void)
{
    vga_set_mode(3);
}

/* ---- memory ---- */
static int map_offset(uint32_t a, uint32_t *off)
{
    uint32_t base, size;
    switch ((gc[6] >> 2) & 3u) {
    case 0: base = 0xA0000u; size = 0x20000u; break;
    case 1: base = 0xA0000u; size = 0x10000u; break;
    case 2: base = 0xB0000u; size = 0x08000u; break;
    default: base = 0xB8000u; size = 0x08000u; break;
    }
    if (a < base || a >= base + size) return 0;
    *off = (a - base) & 0xFFFFu;
    return 1;
}

uint8_t vga_read(uint32_t a)
{
    uint32_t off;
    int p;
    if (!map_offset(a, &off)) return 0xFF;
    if (seq[4] & 0x08u) return planes[off & 3u][off & ~3u];
    if (!(seq[4] & 0x04u)) return planes[(gc[4] & 2u) | (off & 1u)][off & ~1u];   /* odd/even */
    for (p = 0; p < 4; p++) latch[p] = planes[p][off];
    if (gc[5] & 0x08u) {                       /* read mode 1: colour compare */
        uint8_t r = 0xFF;
        for (p = 0; p < 4; p++) {
            if (gc[7] & (1u << p)) {
                uint8_t want = (gc[2] & (1u << p)) ? 0xFFu : 0x00u;
                r &= (uint8_t)~(latch[p] ^ want);
            }
        }
        return r;
    }
    return latch[gc[4] & 3u];
}

static uint8_t alu_fn(uint8_t v, uint8_t l)
{
    switch ((gc[3] >> 3) & 3u) {
    case 1: return (uint8_t)(v & l);
    case 2: return (uint8_t)(v | l);
    case 3: return (uint8_t)(v ^ l);
    default: return v;
    }
}

void vga_write(uint32_t a, uint8_t v)
{
    uint32_t off;
    int p;
    uint8_t rot, mask;
    if (!map_offset(a, &off)) return;
    if (seq[4] & 0x08u) {                      /* chain 4 */
        if (seq[2] & (1u << (off & 3u))) planes[off & 3u][off & ~3u] = v;
        return;
    }
    if (!(seq[4] & 0x04u)) {                   /* odd/even */
        p = (int)(off & 1u);
        if (seq[2] & (1u << p)) planes[p][off & ~1u] = v;
        return;
    }
    rot = (uint8_t)(gc[3] & 7u);
    mask = gc[8];
    for (p = 0; p < 4; p++) {
        uint8_t val, m = mask;
        if (!(seq[2] & (1u << p))) continue;
        switch (gc[5] & 3u) {
        case 0: {
            uint8_t d = (uint8_t)((v >> rot) | (v << (8 - rot)));
            val = (gc[1] & (1u << p)) ? ((gc[0] & (1u << p)) ? 0xFFu : 0x00u) : d;
            val = alu_fn(val, latch[p]);
            break;
        }
        case 1:
            planes[p][off] = latch[p];
            continue;
        case 2:
            val = alu_fn((v & (1u << p)) ? 0xFFu : 0x00u, latch[p]);
            break;
        default: {
            uint8_t d = (uint8_t)((v >> rot) | (v << (8 - rot)));
            m = (uint8_t)(d & mask);
            val = alu_fn((gc[0] & (1u << p)) ? 0xFFu : 0x00u, latch[p]);
            break;
        }
        }
        planes[p][off] = (uint8_t)((val & m) | (latch[p] & ~m));
    }
}

/* ---- ports ---- */
uint8_t vga_in(uint16_t port)
{
    switch (port) {
    case 0x3BA: case 0x3DA: {
        uint8_t v;
        int r0 = g_vga_retrace;
        hal_video_update();
        attr_ff = 0;
        v = (uint8_t)((g_vga_retrace ? 0x08u : 0u) | (g_vga_hblank ? 0x01u : 0u));
        if (g_vga_retrace == r0) {
            if (++poll_count > 32) { poll_count = 0; hal_idle(); }
        } else poll_count = 0;
        return v;
    }
    case 0x3C0: return attr_idx;
    case 0x3C1: return attr[attr_idx & 0x1Fu];
    case 0x3C2: return 0x10;
    case 0x3C4: return seq_idx;
    case 0x3C5: return seq[seq_idx & 7u];
    case 0x3C6: return dac_mask;
    case 0x3C7: return dac_state;
    case 0x3C8: return dac_widx;
    case 0x3C9: {
        uint8_t v = dac[dac_ridx][dac_rsub];
        if (++dac_rsub == 3) { dac_rsub = 0; dac_ridx++; }
        return v;
    }
    case 0x3CC: return misc_out;
    case 0x3CE: return gc_idx;
    case 0x3CF: return gc[gc_idx & 15u];
    case 0x3B4: case 0x3D4: return crtc_idx;
    case 0x3B5: case 0x3D5: return crtc[crtc_idx & 31u];
    default: return 0xFF;
    }
}

void vga_out(uint16_t port, uint8_t v)
{
    poll_count = 0;
    switch (port) {
    case 0x3C0:
        if (!attr_ff) attr_idx = v;
        else if ((attr_idx & 0x1Fu) < 21u) attr[attr_idx & 0x1Fu] = v;
        attr_ff ^= 1u;
        return;
    case 0x3C2: misc_out = v; return;
    case 0x3C4: seq_idx = v; return;
    case 0x3C5: seq[seq_idx & 7u] = v; return;
    case 0x3C6: dac_mask = v; return;
    case 0x3C7: dac_ridx = v; dac_rsub = 0; dac_state = 0; return;
    case 0x3C8: dac_widx = v; dac_wsub = 0; dac_state = 3; return;
    case 0x3C9:
        dac[dac_widx][dac_wsub] = (uint8_t)(v & 0x3Fu);
        if (++dac_wsub == 3) { dac_wsub = 0; dac_widx++; }
        return;
    case 0x3CE: gc_idx = v; return;
    case 0x3CF: gc[gc_idx & 15u] = v; return;
    case 0x3B4: case 0x3D4: crtc_idx = v; return;
    case 0x3B5: case 0x3D5: crtc[crtc_idx & 31u] = v; return;
    default: return;
    }
}

void vga_vsync_latch(void)
{
    latched_start = (uint16_t)((crtc[0x0C] << 8) | crtc[0x0D]);
}

/* ---- the picture ---- */
void vga_render(HostFrame *f)
{
    uint32_t pal[256];
    int i, w, h, lpr, shift, offset, mode256, row, split_row, lc, vde, pan;
    uint32_t amask;
    for (i = 0; i < 256; i++) {
        int j = i & dac_mask;
        uint32_t r = dac[j][0], g = dac[j][1], b = dac[j][2];
        pal[i] = ((r << 2 | r >> 4) << 16) | ((g << 2 | g >> 4) << 8) | (b << 2 | b >> 4);
    }
    f->pixels = fb;
    if (!(attr[0x10] & 1u) || (seq[1] & 0x20u)) {     /* text mode or screen off: black */
        f->width = 320;
        f->height = 200;
        memset(fb, 0, 320u * 200u * 4u);
        return;
    }
    mode256 = (gc[5] & 0x40u) != 0;
    w = (crtc[1] + 1) * (mode256 ? 4 : 8);
    vde = (crtc[0x12] | ((crtc[7] & 0x02) << 7) | ((crtc[7] & 0x40) << 3)) + 1;
    lpr = ((crtc[9] & 0x1F) + 1) * ((crtc[9] & 0x80u) ? 2 : 1);
    h = vde / lpr;
    if (w > 640) w = 640;
    if (h > 480) h = 480;
    if (w <= 0 || h <= 0) { w = 320; h = 200; }
    shift = (crtc[0x14] & 0x40u) ? 2 : (crtc[0x17] & 0x40u) ? 0 : 1;
    offset = crtc[0x13] * 2;
    lc = crtc[0x18] | ((crtc[7] & 0x10) << 4) | ((crtc[9] & 0x40) << 3);
    split_row = (lc + 1 + lpr - 1) / lpr;
    pan = attr[0x13] & 7;
    /* CRTC mode control bits 0 and 1 clear: memory address bits 13 and 14 come from the row scan
     * counter (CGA/Hercules banking).  The rows here are whole character rows, whose first scan
     * line has row scan 0, so those address bits are 0 and the picture wraps within 8 or 16 KB.
     * The difficulty screen's diagonal scroll relies on the 8 KB wrap. */
    amask = 0xFFFFu;
    if (!(crtc[0x17] & 1u)) amask &= ~0x2000u;
    if (!(crtc[0x17] & 2u)) amask &= ~0x4000u;
    for (row = 0; row < h; row++) {
        uint32_t *out = fb + (size_t)row * (size_t)w;
        uint32_t counter;
        int rpan = pan, x;
        if (row >= split_row) {
            counter = (uint32_t)((row - split_row) * offset);
            if (attr[0x10] & 0x20u) rpan = 0;
        } else counter = latched_start + (uint32_t)(row * offset);
        if (mode256) {
            int skip = rpan >> 1;
            for (x = 0; x < w; x++) {
                int px = x + skip;
                uint32_t addr = ((counter + (uint32_t)(px >> 2)) << shift) & amask;
                out[x] = pal[planes[px & 3][addr]];
            }
        } else {
            uint8_t cpe = attr[0x12] & 0x0Fu;
            for (x = 0; x < w; x++) {
                int px = x + rpan;
                uint32_t addr = ((counter + (uint32_t)(px >> 3)) << shift) & amask;
                int bit = 7 - (px & 7);
                int c = ((planes[0][addr] >> bit) & 1) | (((planes[1][addr] >> bit) & 1) << 1) |
                        (((planes[2][addr] >> bit) & 1) << 2) | (((planes[3][addr] >> bit) & 1) << 3);
                int idx;
                c &= cpe;
                idx = attr[c];
                if (attr[0x10] & 0x80u) idx = (idx & 0x0F) | ((attr[0x14] & 3) << 4);
                else idx &= 0x3F;
                idx |= (attr[0x14] & 0x0C) << 4;
                out[x] = pal[idx];
            }
        }
    }
    f->width = w;
    f->height = h;
}

/* diagnostics (ughtest, UGH_VRAM): the four planes, raw, into a file */
void vga_debug_dump(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) return;
    (void)fwrite(planes, 1, sizeof planes, f);
    fclose(f);
}
