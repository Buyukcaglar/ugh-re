/* audio_win32.c - 44100 Hz 16-bit stereo through waveOut.
 * The emulation hands over samples in virtual time (host_audio); they go into a ring and from there
 * into a small set of waveOut buffers.  The ring is kept short: when the emulation runs ahead of the
 * sound card (the two clocks drift a little), the oldest samples are dropped; when the card runs dry
 * (start, or the emulation stalled), playback waits until a cushion of about 70 ms is buffered. */
#include <windows.h>
#include <mmsystem.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "audio_win32.h"

#define NBUF 8
#define BUF_FRAMES 512
#define RING_FRAMES 16384
#define MAX_QUEUED_FRAMES 6144   /* about 140 ms */
#define PREBUF_FRAMES 3072       /* after a start or an underrun, wait for about 70 ms of sound */

static HWAVEOUT g_wo;
static WAVEHDR g_hdr[NBUF];
static int16_t g_bufs[NBUF][BUF_FRAMES * 2];
static int16_t g_ring[RING_FRAMES * 2];
static int g_rhead, g_rcount;
static int g_muted;
static unsigned long g_drop, g_underrun, g_pushes; /* UGH_AUDIOLOG statistics */
static unsigned long g_ulog[64];

int audio_open(void)
{
    WAVEFORMATEX wf;
    int i;
    memset(&wf, 0, sizeof wf);
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = 44100;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = 44100 * 4;
    if (waveOutOpen(&g_wo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        g_wo = NULL;
        return 0;
    }
    for (i = 0; i < NBUF; i++) {
        memset(&g_hdr[i], 0, sizeof g_hdr[i]);
        g_hdr[i].lpData = (LPSTR)g_bufs[i];
        g_hdr[i].dwBufferLength = BUF_FRAMES * 4;
        (void)waveOutPrepareHeader(g_wo, &g_hdr[i], sizeof g_hdr[i]);
        g_hdr[i].dwFlags |= WHDR_DONE;
    }
    return 1;
}

void audio_close(void)
{
    int i;
    if (g_wo == NULL) return;
    if (getenv("UGH_AUDIOLOG")) {
        FILE *f = fopen(getenv("UGH_AUDIOLOG"), "w");
        if (f) { fprintf(f, "pushes %lu dropped_frames %lu underruns %lu\n", g_pushes, g_drop, g_underrun); for (i = 0; i < (int)g_underrun && i < 64; i++) fprintf(f, "%lu ", g_ulog[i]); fclose(f); }
    }
    (void)waveOutReset(g_wo);
    for (i = 0; i < NBUF; i++) (void)waveOutUnprepareHeader(g_wo, &g_hdr[i], sizeof g_hdr[i]);
    (void)waveOutClose(g_wo);
    g_wo = NULL;
}

void audio_set_muted(int muted)
{
    g_muted = muted;
}

static int queued_frames(void)
{
    int i, n = 0;
    for (i = 0; i < NBUF; i++)
        if (!(g_hdr[i].dwFlags & WHDR_DONE)) n += BUF_FRAMES;
    return n;
}

static void feed(void)
{
    int i;
    if (queued_frames() == 0 && g_rcount < PREBUF_FRAMES) return;
    for (i = 0; i < NBUF && g_rcount >= BUF_FRAMES; i++) {
        WAVEHDR *h = &g_hdr[i];
        int k;
        if (!(h->dwFlags & WHDR_DONE)) continue;
        for (k = 0; k < BUF_FRAMES; k++) {
            int s = (g_rhead + k) % RING_FRAMES;
            g_bufs[i][k * 2] = g_muted ? 0 : g_ring[s * 2];
            g_bufs[i][k * 2 + 1] = g_muted ? 0 : g_ring[s * 2 + 1];
        }
        g_rhead = (g_rhead + BUF_FRAMES) % RING_FRAMES;
        g_rcount -= BUF_FRAMES;
        h->dwFlags &= ~(DWORD)WHDR_DONE;
        (void)waveOutWrite(g_wo, h, sizeof *h);
    }
}

void audio_push(const int16_t *stereo, int frames)
{
    int i;
    if (g_wo == NULL) return;
    g_pushes++;
    if (g_pushes > 70 && queued_frames() == 0) { if (g_underrun < 64) g_ulog[g_underrun] = g_pushes; g_underrun++; }
    for (i = 0; i < frames; i++) {
        int t;
        if (g_rcount == RING_FRAMES) {           /* full: drop the oldest */
            g_rhead = (g_rhead + 1) % RING_FRAMES;
            g_rcount--;
            g_drop++;
        }
        t = (g_rhead + g_rcount) % RING_FRAMES;
        g_ring[t * 2] = stereo[i * 2];
        g_ring[t * 2 + 1] = stereo[i * 2 + 1];
        g_rcount++;
    }
    /* keep the latency bounded */
    while (g_rcount + queued_frames() > MAX_QUEUED_FRAMES && g_rcount > BUF_FRAMES) {
        g_rhead = (g_rhead + 64) % RING_FRAMES;
        g_rcount -= 64;
        g_drop += 64;
    }
    feed();
}
