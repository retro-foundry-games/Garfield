#pragma once
/* The platform around the game: hardware state, input, the screen, loading the program image, the run loop.
 * Nothing here is a routine of the game. */
#include "game.h"

#define ST_W 320
#define ST_H 200

/* Convert between the emulator's big-endian RAM image and the native StMem. */
void mem_from_st(StMem *dst, const uint8_t *st_ram);
void mem_to_st(uint8_t *st_ram, const StMem *src);

typedef struct {
    volatile uint32_t vbl_count;
    uint8_t joy1;          /* IKBD joystick 1: bit0 up, bit1 down, bit2 left, bit3 right, bit7 fire */
    uint32_t keys[16];
    unsigned key_head, key_tail;
    int joy_request;       /* set when the game sends the IKBD joystick interrogate command */
} Hardware;

#define JOY_UP    0x01
#define JOY_DOWN  0x02
#define JOY_LEFT  0x04
#define JOY_RIGHT 0x08
#define JOY_FIRE  0x80

extern Hardware hw;

/* Console keyboard queue; entries are (ST scancode << 16) | ASCII. Arrow keys, Ctrl and Shift are the joystick. */
void hw_push_key(uint32_t key);
uint32_t hw_pop_key(void);   /* 0 when empty */

/* The program runs inside game_run(); the host gets control at the hooks. */
typedef struct {
    void (*sync)(int id);   /* the program reached a point where the original takes interrupts (before they run) */
    void (*frame)(void);    /* a vertical blank was delivered: show the screen, pace, read input */
    void (*quit)(void);     /* the program is to end: the host may take it (and return), otherwise the process exits */
    int (*unported)(uint32_t address);   /* the program was sent to something that is not code; nonzero: the host has restored a
                                            state and the program carries on from the top of its main loop */
} GameHooks;
extern GameHooks game_hooks;
int game_load(const char *data_dir);  /* the program's image into memory */
int game_run(const char *data_dir);   /* game_load, then the program from its start; the process ends in game_quit() */
void game_quit(void);
void game_unported(uint32_t address);   /* offers such a jump to the host; returns when it did not take it */
uint32_t game_frames(void);
void game_render(uint32_t *rgba);

/* Opens the window and runs the game in it until it ends or the window closes. */
int sdl_run(const char *data_dir);
