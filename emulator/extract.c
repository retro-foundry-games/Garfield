/* extract <image.stx> <out dir>: writes every file on the disk, flattened by name. */
#include <stdio.h>
#include <sys/stat.h>
#include "extract_files.h"

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s image.stx outdir\n", argv[0]); return 2; }
    StxImage *img = stx_load(argv[1]);
    Disk *d = img ? disk_open(img) : NULL;
    if (!d) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    mkdir(argv[2], 0755);
    return extract_files(d, 0, argv[2]) < 0;
}
