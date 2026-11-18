#pragma once
#include <stddef.h>
#include <stdint.h>

/* Pasti (.stx) disk image. */
typedef struct {
    uint8_t track, side, number, size_code, fdc_status;
    uint16_t crc, bit_position;
    uint32_t size;
    const uint8_t *data;
} StxSector;

typedef struct {
    uint16_t flags, track_length;
    unsigned nsectors;
    StxSector *sectors;
} StxTrack;

typedef struct {
    uint8_t *raw;
    size_t raw_size;
    unsigned ntracks;
    StxTrack tracks[2][128];  /* [side][track] */
} StxImage;

StxImage *stx_load(const char *path);
void stx_free(StxImage *img);
/* Sector contents (512 bytes for the standard format), or NULL when the track has no such sector. */
const uint8_t *stx_sector(const StxImage *img, unsigned track, unsigned side, unsigned number, uint32_t *size);
