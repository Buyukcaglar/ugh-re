/* crt.h - the "CRT scanlines" filter, the same as Civ64's (Civ1RE port/present/crt.c, its extensions CRT..CRT6).
 *
 * The look is DOSBox Pure Unleashed's CRT filter with "Only Scanlines", "Normal gaps", "Sharpest", no curvature,
 * no corners and no border.  The math follows that filter's fragment shader (fragment_shader_crt_src in DOSBox Pure
 * Unleashed's main.cpp, (C) Bernhard Schelling, GPL-2.0-or-later), reimplemented in our own C; no code is copied.
 * With ScanlineThinness T = 0.65 the weight of an image row at distance a (0 .. 0.5 scanlines) from its scanline's
 * centre is w = scan(a T) + scan((1 - a) T), scan(x) = cos(min(0.5, x) 2 pi) / 2 + 1/2, and each pixel's colour is
 * multiplied by w and passed through the shader's tonal curve f(p) = p / (p ty + tz) in linear light.
 *
 * No resampling: every image pixel keeps its scaled colour and only gets its row's weight, so the picture stays as
 * sharp as with the filter off.  The scanlines are double-scanned (two per source row: the 200-line modes' 400) and
 * spaced as over the rows the picture has full screen on the window's monitor, so a window shows the same scanlines
 * as full screen.  Where that spacing would make them vanish or band (about 1 or 2 output rows per scanline), they
 * lie on the output grid instead: one every two output rows. */
#ifndef UGH_CRT_H
#define UGH_CRT_H
#include <stdint.h>

/* the scan weight level (0..255) of each of the sh image rows: `lines` scanlines spaced over ph rows */
void crt_levels(uint8_t *lvl, int sh, int lines, int ph);

/* filters n pixels (XRGB) of one image row in place with that row's level */
void crt_row(uint32_t *px, int n, int level);

#endif
