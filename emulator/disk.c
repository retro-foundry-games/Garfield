#include "disk.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static const uint8_t *sector(const Disk *d, unsigned lsn)
{
    static const uint8_t zero[512];
    unsigned per_cyl = d->spt * d->sides, trk = lsn / per_cyl, rem = lsn % per_cyl;
    const uint8_t *s = stx_sector(d->img, trk, rem / d->spt, rem % d->spt + 1, NULL);
    return s ? s : zero;
}

Disk *disk_open(const StxImage *img)
{
    uint32_t size;
    const uint8_t *boot = stx_sector(img, 0, 0, 1, &size);
    if (!boot) return NULL;
    Disk *d = calloc(1, sizeof *d);
    d->img = img;
    d->bps = le16(boot + 11); d->spc = boot[13]; d->reserved = le16(boot + 14); d->nfats = boot[16];
    d->ndirs = le16(boot + 17); d->nsectors = le16(boot + 19); d->spf = le16(boot + 22);
    d->spt = le16(boot + 24); d->sides = le16(boot + 26);
    if (d->bps != 512 || !d->spc || !d->spt || !d->sides) { free(d); return NULL; }
    d->root_start = d->reserved + d->nfats * d->spf;
    d->data_start = d->root_start + (d->ndirs * 32 + d->bps - 1) / d->bps;
    d->fat = malloc(d->spf * d->bps);
    for (unsigned i = 0; i < d->spf; i++)
        memcpy(d->fat + i * d->bps, sector(d, d->reserved + i), d->bps);
    return d;
}

void disk_close(Disk *d)
{
    if (!d) return;
    free(d->fat);
    free(d);
}

static unsigned next_cluster(const Disk *d, unsigned c)
{
    unsigned o = c * 3 / 2, v = d->fat[o] | d->fat[o + 1] << 8;
    return c & 1 ? v >> 4 : v & 0xFFF;
}

static uint8_t *read_chain(Disk *d, unsigned c, size_t *out_size)
{
    size_t cap = 0, n = 0;
    uint8_t *buf = NULL;
    unsigned guard = 0;
    while (c >= 2 && c < 0xFF0 && guard++ < 4096) {
        buf = realloc(buf, cap += d->spc * d->bps);
        for (unsigned i = 0; i < d->spc; i++, n += d->bps)
            memcpy(buf + n, sector(d, d->data_start + (c - 2) * d->spc + i), d->bps);
        c = next_cluster(d, c);
    }
    *out_size = n;
    return buf;
}

int disk_readdir(Disk *d, uint16_t cluster, DirEntry *out, int max)
{
    uint8_t *raw;
    size_t len;
    if (cluster == 0) {
        len = d->ndirs * 32;
        raw = malloc(len);
        for (size_t o = 0; o < len; o += d->bps)
            memcpy(raw + o, sector(d, d->root_start + o / d->bps), d->bps);
    } else {
        raw = read_chain(d, cluster, &len);
    }
    int n = 0;
    for (size_t o = 0; o + 32 <= len && n < max; o += 32) {
        const uint8_t *e = raw + o;
        if (e[0] == 0) break;
        if (e[0] == 0xE5 || (e[11] & 0x08)) continue;
        DirEntry *de = &out[n++];
        int k = 0;
        for (int i = 0; i < 8 && e[i] != ' '; i++) de->name[k++] = (char)e[i];
        if (e[8] != ' ') {
            de->name[k++] = '.';
            for (int i = 8; i < 11 && e[i] != ' '; i++) de->name[k++] = (char)e[i];
        }
        de->name[k] = 0;
        de->attr = e[11];
        de->cluster = le16(e + 26);
        de->size = le32(e + 28);
    }
    free(raw);
    return n;
}

static int same_name(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return 0;
    return *a == *b;
}

static int find_in(Disk *d, uint16_t dir, const char *path, DirEntry *found)
{
    DirEntry ents[128];
    int n = disk_readdir(d, dir, ents, 128);
    const char *slash = strpbrk(path, "/\\");
    size_t len = slash ? (size_t)(slash - path) : strlen(path);
    for (int i = 0; i < n; i++) {
        if (ents[i].name[0] == '.') continue;
        if (slash && (ents[i].attr & 0x10) && strlen(ents[i].name) == len && !strncasecmp(ents[i].name, path, len))
            return find_in(d, ents[i].cluster, slash + 1, found);
        if (!slash && !(ents[i].attr & 0x10) && same_name(ents[i].name, path)) { *found = ents[i]; return 1; }
    }
    return 0;
}

static int find_anywhere(Disk *d, uint16_t dir, const char *name, DirEntry *found)
{
    DirEntry ents[128];
    int n = disk_readdir(d, dir, ents, 128);
    for (int i = 0; i < n; i++)
        if (!(ents[i].attr & 0x10) && same_name(ents[i].name, name)) { *found = ents[i]; return 1; }
    for (int i = 0; i < n; i++)
        if ((ents[i].attr & 0x10) && ents[i].name[0] != '.' && find_anywhere(d, ents[i].cluster, name, found)) return 1;
    return 0;
}

uint8_t *disk_read_file(Disk *d, const char *path, size_t *size)
{
    DirEntry e;
    int ok = strpbrk(path, "/\\") ? find_in(d, 0, path, &e) : find_anywhere(d, 0, path, &e);
    if (!ok) return NULL;
    size_t len;
    uint8_t *buf = read_chain(d, e.cluster, &len);
    *size = e.size < len ? e.size : len;
    return buf;
}
