/* extract <image.stx> <out dir>: writes every file on the disk, flattened by name. */
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "disk.h"

static int dump(Disk *d, uint16_t cluster, const char *out)
{
    DirEntry ents[128];
    int n = disk_readdir(d, cluster, ents, 128), count = 0;
    for (int i = 0; i < n; i++) {
        if (ents[i].name[0] == '.') continue;
        if (ents[i].attr & 0x10) { count += dump(d, ents[i].cluster, out); continue; }
        size_t size;
        uint8_t *data = disk_read_file(d, ents[i].name, &size);
        char path[1024];
        snprintf(path, sizeof path, "%s/%.12s", out, ents[i].name);
        FILE *f = fopen(path, "wb");
        if (!f || !data) { perror(path); return -1; }
        fwrite(data, 1, size, f);
        fclose(f);
        free(data);
        printf("%s (%zu bytes)\n", ents[i].name, size);
        count++;
    }
    return count;
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s image.stx outdir\n", argv[0]); return 2; }
    StxImage *img = stx_load(argv[1]);
    Disk *d = img ? disk_open(img) : NULL;
    if (!d) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    mkdir(argv[2], 0755);
    return dump(d, 0, argv[2]) < 0;
}
