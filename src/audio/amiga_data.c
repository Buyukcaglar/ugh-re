/* amiga_data.c - see amiga_data.h.  The AmigaDOS file system: 512-byte blocks, the root block at
 * the disk's middle (880 on a DD floppy), hash tables of 72 entries in the root and directory
 * blocks, header blocks with up to 72 data block pointers each (stored backwards) and extension
 * blocks for more; OFS data blocks carry a 24-byte header, FFS ones are plain data. */
#include "amiga_data.h"
#include "amiga_audio.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define BS 512u

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static const uint8_t *blk(const uint8_t *d, size_t len, uint32_t n)
{
    if ((size_t)n * BS + BS > len) return NULL;
    return d + (size_t)n * BS;
}

static int name_eq(const uint8_t *b, const char *name)
{
    unsigned n = b[BS - 80], i;
    if (n > 30 || strlen(name) != n) return 0;
    for (i = 0; i < n; i++)
        if (toupper(b[BS - 79 + i]) != toupper((unsigned char)name[i])) return 0;
    return 1;
}

static uint8_t *read_file(const uint8_t *d, size_t len, uint32_t hdr, int ffs, size_t *out_len)
{
    const uint8_t *b = blk(d, len, hdr);
    uint32_t size, got = 0;
    uint8_t *out;
    int guard = 0;
    if (b == NULL) return NULL;
    size = be32(b + BS - 188);
    if (size > 4u * 1024u * 1024u || (out = (uint8_t *)malloc(size ? size : 1u)) == NULL) return NULL;
    while (b != NULL && got < size && guard++ < 4096) {
        uint32_t cnt = be32(b + 8), i;
        if (cnt > 72) cnt = 72;
        for (i = 0; i < cnt && got < size; i++) {
            const uint8_t *db = blk(d, len, be32(b + 24 + 4 * (71 - i)));
            uint32_t n;
            if (db == NULL) { free(out); return NULL; }
            if (ffs) { n = BS; db = db; }
            else { n = be32(db + 12); if (n > BS - 24) n = BS - 24; db += 24; }
            if (n > size - got) n = size - got;
            memcpy(out + got, db, n);
            got += n;
        }
        b = be32(b + BS - 8) ? blk(d, len, be32(b + BS - 8)) : NULL;
    }
    if (got != size) { free(out); return NULL; }
    *out_len = size;
    return out;
}

static uint8_t *find_in(const uint8_t *d, size_t len, uint32_t dir, const char *name, int ffs, size_t *out_len, int depth)
{
    const uint8_t *b = blk(d, len, dir);
    unsigned i;
    if (b == NULL || depth > 8) return NULL;
    for (i = 0; i < 72; i++) {
        uint32_t h = be32(b + 24 + 4 * i);
        int guard = 0;
        while (h != 0 && guard++ < 1000) {
            const uint8_t *e = blk(d, len, h);
            int32_t sec;
            if (e == NULL) break;
            sec = (int32_t)be32(e + BS - 4);
            if (sec == 2) {
                uint8_t *r = find_in(d, len, h, name, ffs, out_len, depth + 1);
                if (r != NULL) return r;
            } else if (sec == -3 && name_eq(e, name)) {
                return read_file(d, len, h, ffs, out_len);
            }
            h = be32(e + BS - 16);
        }
    }
    return NULL;
}

uint8_t *adf_read_file(const uint8_t *adf, size_t len, const char *name, size_t *out_len)
{
    if (adf == NULL || len < 1760u * BS || memcmp(adf, "DOS", 3) != 0) return NULL;
    return find_in(adf, len, (uint32_t)(len / BS / 2u), name, adf[3] & 1, out_len, 0);
}

int amiga_data_load_adf(const uint8_t *adf, size_t len)
{
    size_t mn = 0, sn = 0;
    uint8_t *m = adf_read_file(adf, len, "mdat.UGH!-All_Sounds.o", &mn);
    uint8_t *s = adf_read_file(adf, len, "smpl.UGH!-All_Sounds", &sn);
    int ok = m != NULL && s != NULL && amiga_audio_load(m, mn, s, sn);
    free(m);
    free(s);
    return ok;
}
