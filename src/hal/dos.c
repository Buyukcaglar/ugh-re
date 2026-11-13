/* dos.c - the DOS and BIOS services the program calls (int 10h, 12h, 16h, 1Ah, 21h, 2Fh, 67h).
 * Files are read from the embedded pack when there is one (pack.c), otherwise from the game
 * folder (host_game_dir); names are matched without regard to case.  Files the program writes
 * (the high-score table UGH!.HI) go to the save folder (host_save_dir), which is also searched
 * first when a file is opened. */
#include "hal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#ifdef _WIN32
#include <io.h>
#else
#include <dirent.h>
#endif

#define MAX_FILES 16
typedef struct DFile {        /* an open file: on disk (f) or in the pack (mem) */
    FILE *f;
    const uint8_t *mem;
    size_t len, pos;
} DFile;
static DFile *g_files[MAX_FILES];
static DFile g_slots[MAX_FILES];

static void dfile_close(DFile *d)
{
    if (d->f != NULL) fclose(d->f);
    memset(d, 0, sizeof *d);
}
static size_t dfile_read(DFile *d, uint8_t *buf, size_t n)
{
    if (d->f != NULL) return fread(buf, 1, n, d->f);
    if (d->pos >= d->len) return 0;
    if (n > d->len - d->pos) n = d->len - d->pos;
    memcpy(buf, d->mem + d->pos, n);
    d->pos += n;
    return n;
}
/* -1 on error, else the new position */
static long dfile_seek(DFile *d, long off, int whence)
{
    long base;
    if (d->f != NULL) return fseek(d->f, off, whence) != 0 ? -1 : ftell(d->f);
    base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (long)d->pos : (long)d->len;
    if (base + off < 0) return -1;
    d->pos = (size_t)(base + off);
    return (long)d->pos;
}
static int g_trace;

static void set_cf(Cpu *c, int on)
{
    /* the trap runs inside the stub: the caller's flags are at SS:SP+4 */
    uint16_t f = rws(c->s[SS], (uint16_t)(c->r[SP] + 4u));
    f = (uint16_t)(on ? (f | F_CF) : (f & ~F_CF));
    wws(c->s[SS], (uint16_t)(c->r[SP] + 4u), f);
}
static void set_zf(Cpu *c, int on)
{
    uint16_t f = rws(c->s[SS], (uint16_t)(c->r[SP] + 4u));
    f = (uint16_t)(on ? (f | F_ZF) : (f & ~F_ZF));
    wws(c->s[SS], (uint16_t)(c->r[SP] + 4u), f);
}

void dos_init(void)
{
    int i;
    for (i = 0; i < MAX_FILES; i++) {
        if (g_files[i] != NULL) dfile_close(g_files[i]);
        g_files[i] = NULL;
    }
    g_trace = getenv("UGH_DOSLOG") != NULL;
}

/* the file in the game folder whose name matches `name` (8.3, any case; a path's last part) */
static const char *base_name(const char *name)
{
    const char *base = name, *p;
    for (p = name; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':') base = p + 1;
    return base;
}

static FILE *open_in_dir(const char *dir, const char *name, const char *mode)
{
    char path[1024];
    const char *base = base_name(name);
    FILE *f;
    if (dir == NULL || *dir == 0) return NULL;
    (void)snprintf(path, sizeof path, "%s/%s", dir, base);
    f = fopen(path, mode);
    if (f != NULL) return f;
#ifndef _WIN32
    {
        DIR *d = opendir(dir);
        struct dirent *e;
        if (d == NULL) return NULL;
        while ((e = readdir(d)) != NULL) {
            const char *a = e->d_name, *b = base;
            while (*a && *b && toupper((unsigned char)*a) == toupper((unsigned char)*b)) { a++; b++; }
            if (*a == 0 && *b == 0) {
                (void)snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
                closedir(d);
                return fopen(path, mode);
            }
        }
        closedir(d);
    }
#endif
    return NULL;
}


/* ---- memory: the arena from the PSP to the top, in blocks the program owns ----
 * Blocks are kept sorted by segment; each has a size in paragraphs and a one-paragraph header
 * below it (as a DOS memory control block).  The program's own block starts at the PSP and
 * fills the memory at the start, as DOS gives it. */
#define MAX_BLOCKS 32
static struct { uint16_t seg, paras; } g_blk[MAX_BLOCKS];
static int g_nblk;
static uint16_t g_mem_top;

void dos_mem_init(uint16_t psp, uint16_t top)
{
    g_nblk = 1;
    g_blk[0].seg = psp;
    g_blk[0].paras = (uint16_t)(top - psp);
    g_mem_top = top;
}
static int blk_find(uint16_t seg)
{
    int i;
    for (i = 0; i < g_nblk; i++)
        if (g_blk[i].seg == seg) return i;
    return -1;
}
static void mem_alloc(Cpu *c)
{
    uint16_t want = c->r[BX];
    uint32_t best = 0, gap, start, end;
    int i;
    for (i = 1; i <= g_nblk; i++) {
        start = (uint32_t)g_blk[i - 1].seg + g_blk[i - 1].paras + 1u;   /* after the header */
        end = i < g_nblk ? (uint32_t)g_blk[i].seg - 1u : g_mem_top;
        gap = end > start ? end - start : 0u;
        if (gap >= want && g_nblk < MAX_BLOCKS) {
            memmove(&g_blk[i + 1], &g_blk[i], (size_t)(g_nblk - i) * sizeof g_blk[0]);
            g_blk[i].seg = (uint16_t)start;
            g_blk[i].paras = want;
            g_nblk++;
            c->r[AX] = (uint16_t)start;
            set_cf(c, 0);
            return;
        }
        if (gap > best) best = gap;
    }
    c->r[AX] = 8;
    c->r[BX] = (uint16_t)best;
    set_cf(c, 1);
}
static void mem_free(Cpu *c)
{
    int i = blk_find(c->s[ES]);
    if (i <= 0) { c->r[AX] = 9; set_cf(c, 1); return; }
    memmove(&g_blk[i], &g_blk[i + 1], (size_t)(g_nblk - i - 1) * sizeof g_blk[0]);
    g_nblk--;
    set_cf(c, 0);
}
static void mem_resize(Cpu *c)
{
    int i = blk_find(c->s[ES]);
    uint32_t end, room;
    if (i < 0) { c->r[AX] = 9; set_cf(c, 1); return; }
    end = i + 1 < g_nblk ? (uint32_t)g_blk[i + 1].seg - 1u : g_mem_top;
    room = end - g_blk[i].seg;
    if (c->r[BX] > room) {
        c->r[AX] = 8;
        c->r[BX] = (uint16_t)room;
        set_cf(c, 1);
        return;
    }
    g_blk[i].paras = c->r[BX];
    set_cf(c, 0);
}

static void read_asciiz(uint16_t seg, uint16_t off, char *buf, size_t n)
{
    size_t i;
    for (i = 0; i + 1 < n; i++) {
        buf[i] = (char)rbs(seg, (uint16_t)(off + i));
        if (buf[i] == 0) break;
    }
    buf[i] = 0;
}

static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static void int21(Cpu *c)
{
    uint8_t ah = (uint8_t)(c->r[AX] >> 8);
    if (g_trace) fprintf(stderr, "int21 ax=%04x bx=%04x cx=%04x dx=%04x\n", c->r[AX], c->r[BX], c->r[CX], c->r[DX]);
    switch (ah) {
    case 0x09: {                            /* print a '$' string */
        size_t i;
        for (i = 0; i + 1 < sizeof g_hal_exit_msg; i++) {
            char ch = (char)rbs(c->s[DS], (uint16_t)(c->r[DX] + i));
            if (ch == '$') break;
            g_hal_exit_msg[i] = ch;
        }
        g_hal_exit_msg[i] = 0;
        fprintf(stderr, "DOS: %s\n", g_hal_exit_msg);
        return;
    }
    case 0x0B: r8set(c, 0, 0); return;
    case 0x19: r8set(c, 0, 2); return;      /* drive C: */
    case 0x1A: return;
    case 0x25:
        wb((uint32_t)(c->r[AX] & 0xFFu) * 4u, (uint8_t)c->r[DX]);
        wb((uint32_t)(c->r[AX] & 0xFFu) * 4u + 1u, (uint8_t)(c->r[DX] >> 8));
        wb((uint32_t)(c->r[AX] & 0xFFu) * 4u + 2u, (uint8_t)c->s[DS]);
        wb((uint32_t)(c->r[AX] & 0xFFu) * 4u + 3u, (uint8_t)(c->s[DS] >> 8));
        return;
    case 0x2A: {
        time_t t = time(NULL);
        struct tm *tm = localtime(&t);
        c->r[CX] = (uint16_t)(tm->tm_year + 1900);
        c->r[DX] = (uint16_t)(((tm->tm_mon + 1) << 8) | tm->tm_mday);
        r8set(c, 0, (uint8_t)tm->tm_wday);
        return;
    }
    case 0x2C: {
        time_t t = time(NULL);
        struct tm *tm = localtime(&t);
        c->r[CX] = (uint16_t)((tm->tm_hour << 8) | tm->tm_min);
        c->r[DX] = (uint16_t)(tm->tm_sec << 8);
        return;
    }
    case 0x30: c->r[AX] = 0x0005u; c->r[BX] = 0; c->r[CX] = 0; return;
    case 0x35:
        c->r[BX] = rw((uint32_t)(c->r[AX] & 0xFFu) * 4u);
        c->s[ES] = rw((uint32_t)(c->r[AX] & 0xFFu) * 4u + 2u);
        return;
    case 0x3D: {
        char name[128];
        int h;
        read_asciiz(c->s[DS], c->r[DX], name, sizeof name);
        for (h = 0; h < MAX_FILES && g_files[h] != NULL; h++) {}
        if (h == MAX_FILES) { c->r[AX] = 4; set_cf(c, 1); return; }
        {
            DFile *d = &g_slots[h];
            memset(d, 0, sizeof *d);
            /* the save folder (the program's own writes), then the pack, then the game folder */
            d->f = open_in_dir(host_save_dir(), name, (c->r[AX] & 3u) == 0 ? "rb" : "r+b");
            if (d->f == NULL) d->mem = hal_pack_find(base_name(name), &d->len);
            if (d->f == NULL && d->mem == NULL) d->f = open_in_dir(host_game_dir(), name, (c->r[AX] & 3u) == 0 ? "rb" : "r+b");
            if (g_trace) fprintf(stderr, "open %s -> %s\n", name, d->mem ? "pack" : d->f ? "ok" : "missing");
            if (d->mem == NULL && d->f == NULL) { c->r[AX] = 2; set_cf(c, 1); return; }
            g_files[h] = d;
        }
        c->r[AX] = (uint16_t)(h + 5);
        set_cf(c, 0);
        return;
    }
    case 0x3C: {                             /* create: in the save folder */
        char name[128];
        int h;
        read_asciiz(c->s[DS], c->r[DX], name, sizeof name);
        for (h = 0; h < MAX_FILES && g_files[h] != NULL; h++) {}
        if (h == MAX_FILES) { c->r[AX] = 4; set_cf(c, 1); return; }
        {
            DFile *d = &g_slots[h];
            memset(d, 0, sizeof *d);
            d->f = open_in_dir(host_save_dir(), name, "w+b");
            if (d->f == NULL) d->f = open_in_dir(host_game_dir(), name, "w+b");
            if (g_trace) fprintf(stderr, "create %s -> %s\n", name, d->f ? "ok" : "failed");
            if (d->f == NULL) { c->r[AX] = 5; set_cf(c, 1); return; }
            g_files[h] = d;
        }
        c->r[AX] = (uint16_t)(h + 5);
        set_cf(c, 0);
        return;
    }
    case 0x3E: {
        int h = c->r[BX] - 5;
        if (h >= 0 && h < MAX_FILES && g_files[h] != NULL) {
            dfile_close(g_files[h]);
            g_files[h] = NULL;
            set_cf(c, 0);
        } else { c->r[AX] = 6; set_cf(c, 1); }
        return;
    }
    case 0x3F: {
        int h = c->r[BX] - 5;
        static uint8_t buf[65536];
        size_t n, i;
        if (h < 0 || h >= MAX_FILES || g_files[h] == NULL) { c->r[AX] = 6; set_cf(c, 1); return; }
        n = dfile_read(g_files[h], buf, c->r[CX]);
        for (i = 0; i < n; i++) wb(lin(c->s[DS], c->r[DX]) + (uint32_t)i, buf[i]);
        c->r[AX] = (uint16_t)n;
        set_cf(c, 0);
        return;
    }
    case 0x40: {
        uint16_t i;
        int h = c->r[BX] - 5;
        if (c->r[BX] == 1 || c->r[BX] == 2) {
            for (i = 0; i < c->r[CX]; i++) fputc(rbs(c->s[DS], (uint16_t)(c->r[DX] + i)), stderr);
        } else if (h >= 0 && h < MAX_FILES && g_files[h] != NULL && g_files[h]->f != NULL) {
            static uint8_t buf[65536];
            for (i = 0; i < c->r[CX]; i++) buf[i] = rbs(c->s[DS], (uint16_t)(c->r[DX] + i));
            c->r[AX] = (uint16_t)fwrite(buf, 1, c->r[CX], g_files[h]->f);
            fflush(g_files[h]->f);
            set_cf(c, 0);
            return;
        } else if (h >= 0) {
            c->r[AX] = 5;
            set_cf(c, 1);
            return;
        }
        c->r[AX] = c->r[CX];
        set_cf(c, 0);
        return;
    }
    case 0x42: {
        int h = c->r[BX] - 5;
        long off = (long)(((uint32_t)c->r[CX] << 16) | c->r[DX]);
        long pos;
        int whence = (c->r[AX] & 0xFFu) == 0 ? SEEK_SET : (c->r[AX] & 0xFFu) == 1 ? SEEK_CUR : SEEK_END;
        if (h < 0 || h >= MAX_FILES || g_files[h] == NULL) { c->r[AX] = 6; set_cf(c, 1); return; }
        if ((c->r[AX] & 0xFFu) != 0) off = (long)(int32_t)off;
        pos = dfile_seek(g_files[h], off, whence);
        if (pos < 0) { c->r[AX] = 25; set_cf(c, 1); return; }
        c->r[AX] = (uint16_t)pos;
        c->r[DX] = (uint16_t)((uint32_t)pos >> 16);
        set_cf(c, 0);
        return;
    }
    case 0x44:
        c->r[DX] = 0x0080u;
        set_cf(c, 0);
        return;
    case 0x48: mem_alloc(c); return;
    case 0x49: mem_free(c); return;
    case 0x4A: mem_resize(c); return;
    case 0x0E: r8set(c, 0, 26); return;    /* select drive: 26 drives */
    case 0x47:                               /* current directory: the root */
        wbs(c->s[DS], c->r[SI], 0);
        set_cf(c, 0);
        return;
    case 0x3B: set_cf(c, 0); return;         /* change directory */
    case 0x43: c->r[CX] = 0x20; set_cf(c, 0); return;
    case 0x41: {                             /* delete: only in the save folder */
        char name[128], path[1024];
        read_asciiz(c->s[DS], c->r[DX], name, sizeof name);
        (void)snprintf(path, sizeof path, "%s/%s", host_save_dir(), base_name(name));
        if (remove(path) != 0) { c->r[AX] = 2; set_cf(c, 1); } else set_cf(c, 0);
        return;
    }
    case 0x4C: case 0x00:
        hal_program_exit();
        return;
    default:
        fprintf(stderr, "int 21h ah=%02x not handled (at %04x:%04x)\n", ah, c->op_cs, c->op_ip);
        set_cf(c, 1);
        return;
    }
}

static void int10(Cpu *c)
{
    uint8_t ah = (uint8_t)(c->r[AX] >> 8), al = (uint8_t)c->r[AX];
    switch (ah) {
    case 0x00: vga_set_mode(al & 0x7F); g_mem[0x449] = (uint8_t)(al & 0x7F); return;
    case 0x0F: c->r[AX] = (uint16_t)((80u << 8) | g_mem[0x449]); c->r[BX] &= 0x00FFu; return;
    case 0x10:
        switch (al) {
        case 0x00: (void)vga_in(0x3DA); vga_out(0x3C0, (uint8_t)(c->r[BX] & 0x1F)); vga_out(0x3C0, (uint8_t)(c->r[BX] >> 8)); vga_out(0x3C0, 0x20); return;
        case 0x01: (void)vga_in(0x3DA); vga_out(0x3C0, 0x11); vga_out(0x3C0, (uint8_t)(c->r[BX] >> 8)); vga_out(0x3C0, 0x20); return;
        case 0x02: {
            int i;
            (void)vga_in(0x3DA);
            for (i = 0; i < 16; i++) {
                vga_out(0x3C0, (uint8_t)i);
                vga_out(0x3C0, rbs(c->s[ES], (uint16_t)(c->r[DX] + i)));
            }
            vga_out(0x3C0, 0x11);
            vga_out(0x3C0, rbs(c->s[ES], (uint16_t)(c->r[DX] + 16)));
            vga_out(0x3C0, 0x20);
            return;
        }
        case 0x10:
            vga_out(0x3C8, (uint8_t)c->r[BX]);
            vga_out(0x3C9, (uint8_t)(c->r[DX] >> 8));
            vga_out(0x3C9, (uint8_t)(c->r[CX] >> 8));
            vga_out(0x3C9, (uint8_t)c->r[CX]);
            return;
        case 0x12: {
            uint16_t i;
            vga_out(0x3C8, (uint8_t)c->r[BX]);
            for (i = 0; i < c->r[CX] * 3u; i++) vga_out(0x3C9, rbs(c->s[ES], (uint16_t)(c->r[DX] + i)));
            return;
        }
        case 0x15:
            vga_out(0x3C7, (uint8_t)c->r[BX]);
            c->r[DX] = (uint16_t)((c->r[DX] & 0xFFu) | (vga_in(0x3C9) << 8));
            c->r[CX] = (uint16_t)(vga_in(0x3C9) << 8);
            c->r[CX] |= vga_in(0x3C9);
            return;
        case 0x17: {
            uint16_t i;
            vga_out(0x3C7, (uint8_t)c->r[BX]);
            for (i = 0; i < c->r[CX] * 3u; i++) wbs(c->s[ES], (uint16_t)(c->r[DX] + i), vga_in(0x3C9));
            return;
        }
        default: return;
        }
    case 0x11: case 0x01: case 0x02: case 0x05: case 0x06: case 0x07: case 0x09: case 0x0A: case 0x0E: return;
    case 0x03: c->r[CX] = 0; c->r[DX] = 0; return;
    case 0x08: c->r[AX] = 0x0720u; return;
    case 0x12: if ((c->r[BX] & 0xFFu) == 0x10u) { c->r[BX] = 0x0003u; c->r[CX] = 0x0009u; } return;
    case 0x1A: if (al == 0) { c->r[AX] = 0x001Au; c->r[BX] = 0x0008u; } return;
    default: return;
    }
}

void dos_trap(Cpu *c, uint8_t n)
{
    switch (n) {
    case 0x10: int10(c); return;
    case 0x11: c->r[AX] = rw(0x410); return;
    case 0x12: c->r[AX] = 640; return;
    case 0x15: r8set(c, 4, 0x86); set_cf(c, 1); return;
    case 0x16: {                             /* the BIOS key buffer (filled by the BIOS int 9) */
        uint16_t head = rw(0x41A), tail = rw(0x41C);
        uint8_t ah = (uint8_t)(c->r[AX] >> 8);
        if (ah == 0x01 || ah == 0x11) {
            set_zf(c, head == tail);
            if (head != tail) c->r[AX] = rw(0x400u + head);
        } else if (ah == 0x00 || ah == 0x10) {
            if (head == tail) { c->r[AX] = 0; return; }   /* (would wait; the game asks with 01h first) */
            c->r[AX] = rw(0x400u + head);
            if (g_trace) fprintf(stderr, "int16: key %04x\n", c->r[AX]);
            head = (uint16_t)(head + 2u >= 0x3Eu ? 0x1Eu : head + 2u);
            g_mem[0x41A] = (uint8_t)head;
            g_mem[0x41B] = 0;
        } else if (ah == 0x02 || ah == 0x12) {
            c->r[AX] = (uint16_t)((c->r[AX] & 0xFF00u) | g_mem[0x417]);
        }
        return;
    }
    case 0x1A: {
        time_t t = time(NULL);
        struct tm *tm = localtime(&t);
        switch (c->r[AX] >> 8) {
        case 0x00:
            c->r[CX] = rw(0x46E);
            c->r[DX] = rw(0x46C);
            r8set(c, 0, 0);
            return;
        case 0x02:
            c->r[CX] = (uint16_t)((bcd(tm->tm_hour) << 8) | bcd(tm->tm_min));
            c->r[DX] = (uint16_t)(bcd(tm->tm_sec) << 8);
            set_cf(c, 0);
            return;
        case 0x04:
            c->r[CX] = (uint16_t)((bcd((tm->tm_year + 1900) / 100) << 8) | bcd(tm->tm_year % 100));
            c->r[DX] = (uint16_t)((bcd(tm->tm_mon + 1) << 8) | bcd(tm->tm_mday));
            set_cf(c, 0);
            return;
        default: return;
        }
    }
    case 0x20: hal_program_exit(); return;
    case 0x21: int21(c); return;
    case 0x2F: if (c->r[AX] == 0x4300u) r8set(c, 0, 0); return;   /* no XMS */
    case 0x33: c->r[AX] = 0; return;                                  /* no mouse */
    case 0x67: r8set(c, 4, 0x80); return;                             /* no EMS */
    case 0x00:
        cpu_fatal("Divide error at %04X:%04X", rw(lin(c->s[SS], c->r[SP])) ? c->op_cs : 0, c->op_ip);
        return;
    case 0x06:
        cpu_fatal("Invalid opcode at %04X:%04X", rws(c->s[SS], (uint16_t)(c->r[SP] + 2)), rws(c->s[SS], c->r[SP]));
        return;
    case 0x1B: case 0x1C: case 0x23: case 0x24: case 0x28: case 0x05: case 0x01: case 0x03: case 0x04:
        return;
    default:
        fprintf(stderr, "int %02xh ax=%04x not handled\n", n, c->r[AX]);
        return;
    }
}
