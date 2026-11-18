#include <SDL3/SDL_main.h>
#include "sdl.h"

int main(int argc, char **argv)
{
    return sdl_run(argc > 1 ? argv[1] : "extracted");
}
