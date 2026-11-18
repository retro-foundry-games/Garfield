#pragma once
#include "stx.h"

/* FAT12 volume on top of a Pasti image. */
typedef struct {
    const StxImage *img;
    unsigned bps, spc, reserved, nfats, ndirs, nsectors, spf, spt, sides;
    unsigned root_start, data_start;
    uint8_t *fat;
} Disk;

typedef struct {
    char name[13];  /* "NAME.EXT" */
    uint8_t attr;   /* 0x10 = directory */
    uint16_t cluster;
    uint32_t size;
} DirEntry;

Disk *disk_open(const StxImage *img);
void disk_close(Disk *d);
/* Entries of a directory (cluster 0 = root). Returns the count written to out. */
int disk_readdir(Disk *d, uint16_t cluster, DirEntry *out, int max);
/* Reads a file by path ("AUTO/GARFIELD.PRG") or, without a slash, by name anywhere on the disk. */
uint8_t *disk_read_file(Disk *d, const char *path, size_t *size);
