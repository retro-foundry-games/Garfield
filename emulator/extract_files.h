#pragma once
#include "disk.h"

/* Writes every file of the directory at `cluster` (0 = root) and below, flattened by name, into `out`.
 * Returns the number of files, or -1 on error. */
int extract_files(Disk *d, uint16_t cluster, const char *out);
