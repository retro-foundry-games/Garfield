#pragma once
#include "emu.h"

/* Transitional runtime: the original 68k code runs on the emulator core, and every routine lifted into C
   replaces its original. Shrinks as more of the game is lifted. */

/* Registers every lifted routine on e. */
void hybrid_install(Emu *e);
extern unsigned hybrid_lifted_calls[8], hybrid_fallbacks[8];

/* Standalone use by the port: owns an emulator, mirrors screen and palette into `mem` and `hw`. */
int hybrid_init(const char *stx_path);
int hybrid_frame(void);  /* runs to the next sync point and delivers the interrupt; 0 ok, -1 halted */
