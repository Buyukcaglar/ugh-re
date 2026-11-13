/* amiga_audio.c - see amiga_audio.h.  The module's data is copied (the player writes to smpl). */
#include "amiga_audio.h"
#include "tfmx.h"
#include <stdlib.h>
#include <string.h>

static uint8_t *g_mdat, *g_smpl;
static int g_ready;

int amiga_audio_load(const uint8_t *mdat, size_t mdat_len, const uint8_t *smpl, size_t smpl_len)
{
    uint8_t *m, *s;
    if (mdat == NULL || smpl == NULL || mdat_len < 0x200 || smpl_len < 4 || memcmp(mdat, "TFMX-SONG", 9) != 0)
        return 0;                           /* (what tfmx_init would refuse; the old module plays on) */
    m = (uint8_t *)malloc(mdat_len);
    s = (uint8_t *)malloc(smpl_len);
    if (m == NULL || s == NULL) {
        free(m);
        free(s);
        return 0;
    }
    memcpy(m, mdat, mdat_len);
    memcpy(s, smpl, smpl_len);
    tfmx_init(m, mdat_len, s, smpl_len);
    free(g_mdat);
    free(g_smpl);
    g_mdat = m;
    g_smpl = s;
    g_ready = 1;
    return 1;
}

int amiga_audio_ready(void) { return g_ready; }

void amiga_audio_sample(int *left, int *right)
{
    if (!g_ready) {
        *left = *right = 0;
        return;
    }
    tfmx_render(left, right);
}

void amiga_audio_song(int song)
{
    if (!g_ready) return;
    if (song < 0) tfmx_stop();
    else tfmx_song(song);
}

void amiga_audio_fx(int fx)
{
    if (g_ready) tfmx_fx(fx);
}

void amiga_audio_stop(void)
{
    if (g_ready) tfmx_stop();
}
