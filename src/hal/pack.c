/* pack.c - game files held in memory (a pack made by tools/mkpak.py), for builds that carry the
 * game inside the program.  When a pack is set, it is searched first, by name, without regard
 * to case; files not in it come from the game folder.
 *
 * Pack layout (little-endian): "P2PK", u32 count, then count entries of
 * { char name[16] (NUL-padded), u32 offset, u32 length }, then the data. */
#include "hal.h"
#include <string.h>
#include <ctype.h>

#define MAX_PACK 256

static struct { char name[16]; const uint8_t *data; uint32_t len; } g_pack[MAX_PACK];
static unsigned g_pack_n;

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int hal_set_pack(const uint8_t *pak, size_t len)
{
    uint32_t n, i;
    g_pack_n = 0;
    if (pak == NULL || len < 8 || memcmp(pak, "P2PK", 4) != 0) return 0;
    n = le32(pak + 4);
    if (n > MAX_PACK || 8u + (size_t)n * 24u > len) return 0;
    for (i = 0; i < n; i++) {
        const uint8_t *e = pak + 8 + (size_t)i * 24u;
        uint32_t off = le32(e + 16), l = le32(e + 20);
        if ((size_t)off > len || (size_t)l > len - off) { g_pack_n = 0; return 0; }
        memcpy(g_pack[i].name, e, 16);
        g_pack[i].name[15] = 0;
        g_pack[i].data = pak + off;
        g_pack[i].len = l;
    }
    g_pack_n = n;
    return 1;
}

int hal_have_pack(void)
{
    return g_pack_n > 0;
}

const uint8_t *hal_pack_find(const char *name, size_t *len)
{
    const char *base = name, *p;
    unsigned i;
    for (p = name; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':') base = p + 1;
    for (i = 0; i < g_pack_n; i++) {
        const char *a = g_pack[i].name, *b = base;
        while (*a && *b && toupper((unsigned char)*a) == toupper((unsigned char)*b)) { a++; b++; }
        if (*a == 0 && *b == 0) { *len = g_pack[i].len; return g_pack[i].data; }
    }
    return NULL;
}
