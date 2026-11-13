/* tfmxwav.c - renders a TFMX subsong or effect to a WAV file (a test of src/audio/tfmx.c).
 *   tfmxwav MDAT SMPL song|fxN SECONDS OUT.wav */
#include "../../src/audio/tfmx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *load(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb");
    uint8_t *b;
    if (!f) { perror(p); exit(1); }
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    b = (uint8_t *)malloc(*n);
    if (fread(b, 1, *n, f) != *n) exit(1);
    fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    size_t ml, sl;
    uint8_t *m, *s;
    long n, i, peak = 0;
    FILE *o;
    uint32_t hdr[11];
    if (argc < 6) { fprintf(stderr, "usage: tfmxwav MDAT SMPL song|fxN SECONDS OUT.wav\n"); return 1; }
    m = load(argv[1], &ml);
    s = load(argv[2], &sl);
    if (!tfmx_init(m, ml, s, sl)) { fprintf(stderr, "not a TFMX module\n"); return 1; }
    if (strncmp(argv[3], "fx", 2) == 0) {
        int l, r;
        tfmx_song(2);                       /* a short jingle; the effect starts once it ended */
        for (i = 0; i < 44100L * 4; i++) tfmx_render(&l, &r);
        tfmx_fx(atoi(argv[3] + 2));
    } else tfmx_song(atoi(argv[3]));
    n = (long)(atof(argv[4]) * 44100);
    o = fopen(argv[5], "wb");
    fseek(o, 44, SEEK_SET);
    for (i = 0; i < n; i++) {
        int l, r;
        int16_t v[2];
        tfmx_render(&l, &r);
        if (labs(l) > peak) peak = labs(l);
        if (l > 32767) l = 32767; if (l < -32768) l = -32768;
        if (r > 32767) r = 32767; if (r < -32768) r = -32768;
        v[0] = (int16_t)l; v[1] = (int16_t)r;
        fwrite(v, 2, 2, o);
    }
    hdr[0] = 0x46464952u; hdr[1] = 36u + (uint32_t)n * 4u; hdr[2] = 0x45564157u; hdr[3] = 0x20746D66u;
    hdr[4] = 16; hdr[5] = 0x00020001u; hdr[6] = 44100; hdr[7] = 44100 * 4; hdr[8] = 0x00100004u;
    hdr[9] = 0x61746164u; hdr[10] = (uint32_t)n * 4u;
    fseek(o, 0, SEEK_SET); fwrite(hdr, 4, 11, o); fclose(o);
    fprintf(stderr, "peak %ld, playing %d\n", peak, tfmx_playing());
    return 0;
}
