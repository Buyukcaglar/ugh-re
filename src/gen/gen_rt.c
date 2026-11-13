/* gen_rt.c - runs the recompiled blocks: per code segment, a table from offsets to blocks, and the
 * hook through which cpu_run hands CS:IP over to them.  Code without a block (not found by the
 * analysis) runs in the interpreter.
 *
 * The hook stays off while the PKLITE decompressor works (the code is not there yet); it arms when
 * execution first reaches the program's entry point and the code bytes match the ones the blocks
 * were made from.  UGH_DUMPIMG=file writes the unpacked program there at that moment (the input of
 * tools/ughrec.py). */
#include "gen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SEGS 16
static struct { uint16_t cs; GenFn *tab; } g_segs[MAX_SEGS];
static unsigned g_nsegs;
static int g_on, g_armed, g_checked;
static const char *g_dump;
uint64_t g_gen_blocks_run;

#define MAX_WATCH 64
static struct { uint16_t cs, ip; GenWatchFn fn; } g_watch[MAX_WATCH];
static unsigned g_nwatch;
static uint8_t g_watch_ip[65536 / 8];   /* a quick filter on IP */

int gen_watch(uint16_t cs, uint16_t ip, GenWatchFn fn)
{
    if (g_nwatch == MAX_WATCH) return 0;
    g_watch[g_nwatch].cs = cs;
    g_watch[g_nwatch].ip = ip;
    g_watch[g_nwatch].fn = fn;
    g_nwatch++;
    g_watch_ip[ip >> 3] |= (uint8_t)(1u << (ip & 7));
    return 1;
}

static void watch(Cpu *c)
{
    unsigned i;
    if (!(g_watch_ip[c->ip >> 3] & (1u << (c->ip & 7)))) return;
    for (i = 0; i < g_nwatch; i++)
        if (g_watch[i].ip == c->ip && g_watch[i].cs == c->s[CS]) g_watch[i].fn(c);
}

static GenFn *seg_table(uint16_t cs)
{
    unsigned i;
    for (i = 0; i < g_nsegs; i++)
        if (g_segs[i].cs == cs) return g_segs[i].tab;
    return NULL;
}

static uint64_t fnv(const uint8_t *p, uint32_t n)
{
    uint64_t h = 1469598103934665603ull;
    uint32_t i;
    for (i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

/* at the entry point: the dump, then the code check */
static void arm(void)
{
    unsigned i;
    g_checked = 1;
    if (g_dump != NULL) {
        FILE *f = fopen(g_dump, "wb");
        if (f != NULL) {
            fwrite(g_mem + LOAD_SEG * 16u, 1, 0xA0000u - LOAD_SEG * 16u, f);
            fclose(f);
        }
    }
    if (!g_on || g_gen_block_count == 0) return;
    for (i = 0; i < g_gen_range_count; i++) {
        const GenRange *r = &g_gen_ranges[i];
        if (r->lin + r->len > MEM_SIZE || fnv(g_mem + r->lin, r->len) != r->hash) {
            fprintf(stderr, "gen: the program differs from the one the blocks were made from (%05x); interpreter only\n", r->lin);
            return;
        }
    }
    g_armed = 1;
}

static int gen_hook(Cpu *c, uint64_t until)
{
    GenFn *tab, f;
    uint16_t cur;
    int ran = 0;
    if (!g_armed) {
        if (!g_checked && c->s[CS] == g_gen_entry_cs && c->ip == g_gen_entry_ip) arm();
        if (g_checked) watch(c);
        return 0;
    }
    watch(c);
    cur = c->s[CS];
    if ((tab = seg_table(cur)) == NULL || (f = tab[c->ip]) == NULL) return 0;
    for (;;) {
        if (!f(c)) break;
        ran = 1;
        g_gen_blocks_run++;
        if (c->irq_pending || c->stop || c->halted || c->icount >= until) break;
        if (c->s[CS] != cur) {
            cur = c->s[CS];
            if ((tab = seg_table(cur)) == NULL) break;
        }
        if ((f = tab[c->ip]) == NULL) break;
        watch(c);
    }
    return ran;
}

int gen_armed(void)
{
    return g_armed;
}

void gen_install(int on)
{
    unsigned i;
    for (i = 0; i < g_nsegs; i++) memset(g_segs[i].tab, 0, 65536u * sizeof(GenFn));
    for (i = 0; i < g_gen_block_count; i++) {
        GenFn *tab = seg_table(g_gen_blocks[i].cs);
        if (tab == NULL) {
            if (g_nsegs == MAX_SEGS) continue;
            tab = (GenFn *)calloc(65536u, sizeof(GenFn));
            if (tab == NULL) continue;
            g_segs[g_nsegs].cs = g_gen_blocks[i].cs;
            g_segs[g_nsegs].tab = tab;
            g_nsegs++;
        }
        tab[g_gen_blocks[i].ip] = g_gen_blocks[i].fn;
    }
    g_nwatch = 0;
    memset(g_watch_ip, 0, sizeof g_watch_ip);
    g_on = on;
    g_armed = 0;
    g_checked = 0;
    g_dump = getenv("UGH_DUMPIMG");
    g_native_hook = gen_hook;          /* (also for the watched addresses and the entry check) */
}
