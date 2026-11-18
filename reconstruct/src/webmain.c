/* The browser build's entry: the page has put the disk image at argv[1]; its files are taken from it, then the game runs. */
#include <stdio.h>
#include <sys/stat.h>
#include "extract_files.h"
#include "sdl.h"

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    StxImage *image = stx_load(argv[1]);
    Disk *disk = image ? disk_open(image) : NULL;
    if (!disk) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    mkdir("/extracted", 0755);
    if (extract_files(disk, 0, "/extracted") <= 0)
        return 1;
    return sdl_run("/extracted");
}
