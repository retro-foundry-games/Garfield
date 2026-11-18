/* Where the original jumps through a table to something that is not a routine of the game (room 80's record, command
 * numbers past the end of a table): the host may take it (game_unported), otherwise the program stops. */
#include <stdio.h>
#include "game.h"
#include "sdl.h"


void unknown_sound_command(uint32_t address)
{
    game_unported(address);
    fprintf(stderr, "the sound sequencer command $%02X is not a command of the game\n", address);
    game_quit();
}


void unknown_room_routine(uint32_t address)
{
    game_unported(address);
    fprintf(stderr, "the room routine at $%06X is not a routine of the game (room %u, came from %u, state routine $%06X)\n", address, mem.vars.room,
            mem.vars.previous_room, mem.vars.state_routine);
    game_quit();
}

void unknown_entity_command(unsigned command)
{
    game_unported(command);
    fprintf(stderr, "animation command %u is not a command of the game\n", command);
    game_quit();
}

