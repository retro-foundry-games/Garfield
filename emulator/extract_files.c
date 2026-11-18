#include <stdio.h>
#include <stdlib.h>
#include "extract_files.h"

int extract_files(Disk *d, uint16_t cluster, const char *out)
{
    DirEntry ents[128];
    int n = disk_readdir(d, cluster, ents, 128), count = 0;
    for (int i = 0; i < n; i++) {
        if (ents[i].name[0] == '.') continue;
        if (ents[i].attr & 0x10) {
            int inner = extract_files(d, ents[i].cluster, out);
            if (inner < 0) return -1;
            count += inner;
            continue;
        }
        size_t size;
        uint8_t *data = disk_read_file(d, ents[i].name, &size);
        char path[1024];
        snprintf(path, sizeof path, "%s/%.12s", out, ents[i].name);
        FILE *f = fopen(path, "wb");
        if (!f || !data) { perror(path); free(data); return -1; }
        fwrite(data, 1, size, f);
        fclose(f);
        free(data);
        printf("%s (%zu bytes)\n", ents[i].name, size);
        count++;
    }
    return count;
}
