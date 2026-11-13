/* crt.c - the "CRT scanlines" filter (crt.h): Civ64's, table-driven, portable C. */
#include "crt.h"
#include <math.h>
#include <string.h>

#define CRT_T 0.65                /* ScanlineThinness: 0.5 + 0.05 x "Normal gaps" (3) */
#define CRT_PI2 6.28318530717958
#define LEVELS 256

static int g_ready;
static uint16_t g_dec[256];       /* sRGB byte -> linear, 0..65535 */
static uint8_t g_enc[65536];      /* linear -> sRGB byte */
static uint32_t g_fac[LEVELS][256];   /* 16.16 factor at level, brightest channel m: w f(p) / p, p = w lin(m) */

static void tables(void)
{
    double mid_out = 0.18 / (1.5 - CRT_T), mid_in = 0.18;
    double ty = (mid_out - mid_in) / ((1.0 - mid_in) * mid_out);
    double tz = (mid_in - mid_in * mid_out) / (mid_out - mid_in * mid_out);
    int i, m;
    if (g_ready) return;
    for (i = 0; i < 256; i++) {
        double v = floor(pow(((double)i / 255.0 + 0.055) / 1.055, 2.4) * 65535.0 + 0.5);
        g_dec[i] = (uint16_t)(v < 0.0 ? 0.0 : v > 65535.0 ? 65535.0 : v);
    }
    for (i = 0; i < 65536; i++) {
        double v = floor((1.055 * pow((double)i / 65535.0, 1.0 / 2.4) - 0.055) * 255.0 + 0.5);
        g_enc[i] = (uint8_t)(v < 0.0 ? 0.0 : v > 255.0 ? 255.0 : v);
    }
    for (i = 0; i < LEVELS; i++) {
        double w = (double)i / (double)(LEVELS - 1);
        for (m = 0; m < 256; m++) {
            double p = w * (double)g_dec[m] / 65535.0;
            if (p < 1.0 / (256.0 * 65536.0)) p = 1.0 / (256.0 * 65536.0);
            g_fac[i][m] = (uint32_t)floor(w / (p * ty + tz) * 65536.0 + 0.5);
        }
    }
    g_ready = 1;
}

static double scan(double x)
{
    return cos((x < 0.5 ? x : 0.5) * CRT_PI2) * 0.5 + 0.5;
}

static uint8_t level(double a)
{
    double w = scan(a * CRT_T) + scan((1.0 - a) * CRT_T);
    return (uint8_t)floor((w > 1.0 ? 1.0 : w) * (double)(LEVELS - 1) + 0.5);
}

/* 1: the scanlines on the two-row output grid (P = ph / lines outside P >= 2.25 and 1.4 <= P < 1.75) */
static int grid_rule(int ph, int lines)
{
    int64_t s = ph, n = lines < 1 ? 1 : lines;
    return !(4 * s >= 9 * n || (5 * s >= 7 * n && 4 * s < 7 * n));
}

void crt_levels(uint8_t *lvl, int sh, int lines, int ph)
{
    int e, grid;
    tables();
    if (lines < 1) lines = 1;
    if (ph < 1) ph = sh;
    grid = grid_rule(ph, lines);
    for (e = 0; e < sh; e++) {
        double py = ((double)e + 0.5) * (double)lines / (double)ph;
        double a = fabs(py - (floor(py) + 0.5));
        if (grid) a = (e & 1) ? 0.5 : 0.0;
        lvl[e] = level(a);
    }
}

void crt_row(uint32_t *px, int n, int lvl)
{
    const uint32_t *fac = g_fac[lvl];
    uint32_t last_in, last_out = 0;
    int x;
    if (n <= 0) return;
    last_in = px[0] ^ 1u;
    for (x = 0; x < n; x++) {
        uint32_t p = px[x];
        if (p != last_in) {
            uint32_t r = (p >> 16) & 0xFFu, g = (p >> 8) & 0xFFu, b = p & 0xFFu;
            uint32_t m = r > g ? r : g, f, v;
            m = m > b ? m : b;
            f = fac[m];
            v = (uint32_t)(((uint64_t)g_dec[r] * f) >> 16); r = g_enc[v > 65535u ? 65535u : v];
            v = (uint32_t)(((uint64_t)g_dec[g] * f) >> 16); g = g_enc[v > 65535u ? 65535u : v];
            v = (uint32_t)(((uint64_t)g_dec[b] * f) >> 16); b = g_enc[v > 65535u ? 65535u : v];
            last_in = p;
            last_out = (r << 16) | (g << 8) | b;
        }
        px[x] = last_out;
    }
}
