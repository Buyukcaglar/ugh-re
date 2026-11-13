/* headless/main.c - runs the game without a window, for tests.
 *   ughtest GAMEDIR FRAMES [OUTDIR] [script]
 * script: comma-separated "frame:key:down" events (key = set-1 scancode, hex), e.g. "300:1c:1,305:1c:0".
 * Writes OUTDIR/fNNNNN.png every 50 frames and at the end; prints a hash of each frame. */
#include "../../hal/hal.h"
#include "../pngw.h"
#include "../../gen/gen.h"
#include "../../audio/amiga_data.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

static const char *g_dir;
static const char *g_out;
static long g_frame;
static int g_every = 50;

const char *host_game_dir(void) { return g_dir; }
static const char *g_save;
const char *host_save_dir(void) { return g_save; }
static FILE *g_wav;
static long g_wav_frames;
void host_audio(const int16_t *s, int n)
{
    if (g_wav) { fwrite(s, 4, (size_t)n, g_wav); g_wav_frames += n; }
}
static void wav_close(void)
{
    uint32_t hdr[11];
    if (!g_wav) return;
    hdr[0] = 0x46464952u; hdr[1] = 36u + (uint32_t)g_wav_frames * 4u; hdr[2] = 0x45564157u; hdr[3] = 0x20746D66u;
    hdr[4] = 16; hdr[5] = 0x00020001u; hdr[6] = 44100; hdr[7] = 44100 * 4; hdr[8] = 0x00100004u;
    hdr[9] = 0x61746164u; hdr[10] = (uint32_t)g_wav_frames * 4u;
    fseek(g_wav, 0, SEEK_SET);
    fwrite(hdr, 4, 11, g_wav);
    fclose(g_wav);
}
static uint64_t g_fake_us;
uint64_t host_time_us(void) { return g_fake_us; }
void host_sleep_us(uint64_t us) { g_fake_us += us; }
void host_pump(void) {}
int host_quit_requested(void) { return 0; }
void hal_fatal(const char *msg)
{
    fprintf(stderr, "FATAL: %s (frame %ld, cs:ip %04x:%04x)\n", msg, g_frame, g_cpu.op_cs, g_cpu.op_ip);
    exit(2);
}
static FILE *g_hashes;   /* UGH_HASH: one line per frame, FNV-1a of the picture */
void host_present(const HostFrame *f)
{
    char path[512];
    if (g_hashes) {
        uint64_t h = 1469598103934665603ull;
        size_t k, n = (size_t)f->width * (size_t)f->height;
        for (k = 0; k < n; k++) h = (h ^ f->pixels[k]) * 1099511628211ull;
        fprintf(g_hashes, "%ld %016llx %llu\n", g_frame, (unsigned long long)h, (unsigned long long)g_cpu.icount);
    }
    if (g_out && (g_frame % g_every) == 0) {
        snprintf(path, sizeof path, "%s/f%05ld.png", g_out, g_frame);
        png_write_rgb(path, f->width, f->height, f->pixels);
    }
}

int main(int argc, char **argv)
{
    char err[512];
    long frames, i;
    const char *script = argc > 4 ? argv[4] : "";
    if (argc < 3) { fprintf(stderr, "usage: ughtest GAMEDIR FRAMES [OUTDIR] [script]\n"); return 1; }
    g_dir = argv[1];
    g_save = getenv("UGH_SAVE") ? getenv("UGH_SAVE") : ".";
    frames = atol(argv[2]);
    g_out = argc > 3 && strcmp(argv[3], "-") != 0 && argv[3][0] ? argv[3] : NULL;   /* "-": no pictures */
    if (getenv("UGH_EVERY")) g_every = atoi(getenv("UGH_EVERY"));
    if (getenv("UGH_HASH")) g_hashes = fopen(getenv("UGH_HASH"), "w");
    if (getenv("UGH_WAV")) { g_wav = fopen(getenv("UGH_WAV"), "wb"); if (g_wav) fseek(g_wav, 44, SEEK_SET); }
    if (getenv("UGH_COV")) g_cov = (uint16_t *)calloc(0x100000, sizeof(uint16_t));
    g_trace_on = getenv("UGH_TRACE") != NULL;
    if (getenv("UGH_ADF")) {             /* the Amiga disk: its sound, heard when UGH_AMIGA is set */
        FILE *af = fopen(getenv("UGH_ADF"), "rb");
        static uint8_t adf[901120];
        size_t an = af ? fread(adf, 1, sizeof adf, af) : 0;
        if (af) fclose(af);
        fprintf(stderr, "amiga sound: %s\n", amiga_data_load_adf(adf, an) ? "loaded" : "not found");
        hal_set_audio_source(getenv("UGH_AMIGA") != NULL);
    }
    if (!hal_init(err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    for (i = 0; i < frames; i++) {
        const char *p = script;
        g_frame = i;
        while (p && *p) {
            long fr; unsigned sc; int down;
            if (sscanf(p, "%ld:%x:%d", &fr, &sc, &down) == 3 && fr == i) hal_key((uint8_t)(sc & 0x7F), down, (sc & 0x100) != 0);
            p = strchr(p, ',');
            if (p) p++;
        }
        if (!hal_frame()) { fprintf(stderr, "program ended at frame %ld: %s\n", i, g_hal_exit_msg); break; }
    }
    wav_close();
    if (g_trace_on) {
        FILE *tf = fopen(getenv("UGH_TRACE"), "w");
        unsigned k, n = g_trace_pos < TRACE_RING ? g_trace_pos : TRACE_RING;
        for (k = 0; tf && k < n; k++) {
            uint32_t v = g_trace_ring[(g_trace_pos - n + k) & (TRACE_RING - 1u)];
            fprintf(tf, "%04x:%04x\n", (unsigned)(v >> 16), (unsigned)(v & 0xFFFFu));
        }
        if (tf) fclose(tf);
    }
    if (getenv("UGH_VRAM")) vga_debug_dump(getenv("UGH_VRAM"));
    if (g_cov) {
        FILE *cf = fopen(getenv("UGH_COV"), "ab");
        if (cf) {
            long k;
            for (k = 0; k < 0x100000; k++)
                if (g_cov[k]) fprintf(cf, "%04x:%04x\n", g_cov[k], (unsigned)((k - (long)g_cov[k] * 16) & 0xFFFF));
            fclose(cf);
        }
    }
    fprintf(stderr, "ran %ld frames, %llu instructions, cs:ip %04x:%04x, %llu native blocks\n", i,
            (unsigned long long)g_cpu.icount, g_cpu.s[CS], g_cpu.ip, (unsigned long long)g_gen_blocks_run);
    {
        extern unsigned long g_stat_opl_writes, g_stat_sb_blocks;
        fprintf(stderr, "opl writes %lu, sb blocks %lu\n", g_stat_opl_writes, g_stat_sb_blocks);
    }
    if (getenv("UGH_MEMDUMP")) {        /* the whole first megabyte at the end */
        FILE *mf = fopen(getenv("UGH_MEMDUMP"), "wb");
        if (mf) { fwrite(g_mem, 1, 0x100000, mf); fclose(mf); }
    }
    if (getenv("UGH_PEEK")) {           /* "SSSS:OOOO,n" words at a runtime address */
        unsigned sg, of, n = 8, k;
        if (sscanf(getenv("UGH_PEEK"), "%x:%x,%u", &sg, &of, &n) >= 2)
            for (k = 0; k < n; k++) fprintf(stderr, "%04x:%04x = %04x\n", sg, of + 2 * k, rws((uint16_t)sg, (uint16_t)(of + 2 * k)));
    }
    if (g_hashes) fclose(g_hashes);
    return 0;
}
