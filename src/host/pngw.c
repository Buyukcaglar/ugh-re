/* pngw.c - a minimal PNG writer (stored deflate blocks) for test frames and screenshots. */
#include "pngw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];
static void crc_init(void)
{
    uint32_t i, k, c;
    if (crc_table[1]) return;
    for (i = 0; i < 256; i++) {
        c = i;
        for (k = 0; k < 8; k++) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}
static uint32_t crc(uint32_t c, const uint8_t *p, size_t n)
{
    size_t i;
    c = ~c;
    for (i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return ~c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void chunk(FILE *f, const char *type, const uint8_t *d, size_t n)
{
    uint8_t h[8];
    uint32_t c;
    be32(h, (uint32_t)n);
    memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    if (n) fwrite(d, 1, n, f);
    c = crc(0, h + 4, 4);
    c = crc(c, d, n);
    be32(h, c);
    fwrite(h, 1, 4, f);
}

int png_write_rgb(const char *path, int w, int h, const uint32_t *px)
{
    size_t raw_n = (size_t)h * ((size_t)w * 3u + 1u), blocks = (raw_n + 65534u) / 65535u;
    size_t z_n = 2u + raw_n + blocks * 5u + 4u, i, o = 0, left;
    uint8_t *raw = (uint8_t *)malloc(raw_n), *z = (uint8_t *)malloc(z_n), ihdr[13];
    uint32_t a = 1, b = 0;
    FILE *f;
    int x, y;
    if (raw == NULL || z == NULL) { free(raw); free(z); return 0; }
    crc_init();
    for (y = 0; y < h; y++) {
        uint8_t *r = raw + (size_t)y * ((size_t)w * 3u + 1u);
        *r++ = 0;
        for (x = 0; x < w; x++) {
            uint32_t p = px[(size_t)y * (size_t)w + (size_t)x];
            *r++ = (uint8_t)(p >> 16); *r++ = (uint8_t)(p >> 8); *r++ = (uint8_t)p;
        }
    }
    z[o++] = 0x78; z[o++] = 0x01;
    for (i = 0, left = raw_n; left > 0;) {
        size_t n = left > 65535u ? 65535u : left;
        z[o++] = (uint8_t)(left == n ? 1 : 0);
        z[o++] = (uint8_t)n; z[o++] = (uint8_t)(n >> 8);
        z[o++] = (uint8_t)~n; z[o++] = (uint8_t)(~n >> 8);
        memcpy(z + o, raw + i, n);
        o += n; i += n; left -= n;
    }
    for (i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521u; b = (b + a) % 65521u; }
    be32(z + o, (b << 16) | a);
    o += 4;
    f = fopen(path, "wb");
    if (f == NULL) { free(raw); free(z); return 0; }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    be32(ihdr, (uint32_t)w); be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, o);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
    return 1;
}
