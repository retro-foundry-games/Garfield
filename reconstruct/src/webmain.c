/* The browser build's entry: the page has put the disk image at argv[1]; its files are taken from it, then the game runs. */
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <SDL3/SDL.h>
#include <emscripten.h>
#include "extract_files.h"
#include "sdl.h"

/* Escape: the page reloads to its launcher, since the program's state cannot be run twice. */
static void on_quit(void)
{
    SDL_Quit();
    EM_ASM(if (Module.onQuit) Module.onQuit(););
    exit(0);
}

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
    game_hooks.quit = on_quit;
    return sdl_run("/extracted");
}
