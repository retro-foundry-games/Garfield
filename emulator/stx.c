#include "stx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

StxImage *stx_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    StxImage *img = calloc(1, sizeof *img);
    img->raw = malloc(len);
    img->raw_size = (size_t)len;
    if (fread(img->raw, 1, len, f) != (size_t)len || len < 16 || memcmp(img->raw, "RSY\0", 4)) {
        fclose(f);
        stx_free(img);
        return NULL;
    }
    fclose(f);
    const uint8_t *raw = img->raw;
    img->ntracks = raw[10];
    size_t pos = 16;
    for (unsigned t = 0; t < img->ntracks; t++) {
        uint32_t rec_size = le32(raw + pos), fuzzy = le32(raw + pos + 4);
        unsigned nsec = le16(raw + pos + 8), tno = raw[pos + 14];
        StxTrack *tr = &img->tracks[tno >> 7][tno & 0x7F];
        tr->flags = le16(raw + pos + 10);
        tr->track_length = le16(raw + pos + 12);
        size_t p = pos + 16;
        if (tr->flags & 1) {  /* sector descriptors follow the track header */
            tr->nsectors = nsec;
            tr->sectors = calloc(nsec, sizeof *tr->sectors);
            size_t data_base = p + 16 * nsec + fuzzy;
            for (unsigned i = 0; i < nsec; i++) {
                const uint8_t *d = raw + p + 16 * i;
                StxSector *s = &tr->sectors[i];
                uint32_t off = le32(d);
                s->bit_position = le16(d + 4);
                s->track = d[8]; s->side = d[9]; s->number = d[10]; s->size_code = d[11];
                s->crc = le16(d + 12);
                s->fdc_status = d[14];
                s->size = 128u << (s->size_code & 3);
                s->data = data_base + off + s->size <= img->raw_size ? raw + data_base + off : NULL;
            }
        }
        pos += rec_size;
    }
    return img;
}

void stx_free(StxImage *img)
{
    if (!img) return;
    for (int s = 0; s < 2; s++)
        for (int t = 0; t < 128; t++)
            free(img->tracks[s][t].sectors);
    free(img->raw);
    free(img);
}

const uint8_t *stx_sector(const StxImage *img, unsigned track, unsigned side, unsigned number, uint32_t *size)
{
    if (track >= 128 || side >= 2) return NULL;
    const StxTrack *tr = &img->tracks[side][track];
    for (unsigned i = 0; i < tr->nsectors; i++) {
        const StxSector *s = &tr->sectors[i];
        if (s->number == number && s->track == track) {
            if (size) *size = s->size;
            return s->data;
        }
    }
    return NULL;
}
