#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct Emu Emu;

/* Boots GARFIELD.PRG from the disk image; GEMDOS file calls read from the same image. */
Emu *emu_new(const char *stx_path);
void emu_free(Emu *e);

/* Runs the program until it reaches a vsync wait point. Returns 0, or -1 if it halted or faulted. */
int emu_run_to_vsync(Emu *e);
/* Delivers the pending interrupt (IKBD joystick packet, else VBL) at the vsync point. */
void emu_vsync(Emu *e);

void emu_read(Emu *e, uint32_t addr, void *buf, size_t n);
void emu_write(Emu *e, uint32_t addr, const void *buf, size_t n);
uint32_t emu_pc(Emu *e);
uint32_t emu_frames(const Emu *e);
void emu_set_joy1(Emu *e, uint8_t state);

void emu_get_regs(Emu *e, uint32_t d[8], uint32_t a[8]);
void emu_set_regs(Emu *e, const uint32_t d[8], const uint32_t a[8]);

/* Replaces the routine at `addr`. fn returns 1 when it handled the call (the emulator then performs the
   rts), or 0 to let the original code run. */
typedef int (*EmuRoutineFn)(Emu *e, void *user);
void emu_replace(Emu *e, uint32_t addr, EmuRoutineFn fn, void *user);

/* Complete machine state, for rewinding without replaying from the start. */
typedef struct EmuState EmuState;
EmuState *emu_save(Emu *e);
void emu_restore(Emu *e, const EmuState *s);
void emu_state_free(EmuState *s);

/* Calls fn for every write overlapping [lo, hi]; one watch at a time. */
typedef void (*EmuWatchFn)(Emu *e, uint32_t pc, uint32_t addr, int size, uint32_t value, void *user);
void emu_watch_write(Emu *e, uint32_t lo, uint32_t hi, EmuWatchFn fn, void *user);
void emu_watch_clear(Emu *e);

/* Return addresses on the stack that follow a bsr/jsr, with the call target in targets[]. */
int emu_backtrace(Emu *e, uint32_t *returns, uint32_t *targets, int max);

/* Calls fn(pc) before every instruction until emu_trace_clear. */
typedef void (*EmuTraceFn)(uint32_t pc, void *user);
void emu_trace(Emu *e, EmuTraceFn fn, void *user);
void emu_trace_clear(Emu *e);

/* Console keyboard (GEMDOS Crawio/Cconis/Cconin): value is (ST scancode << 16) | ASCII. */
void emu_push_key(Emu *e, uint32_t key);

uint32_t emu_get_sr(Emu *e);
void emu_set_sr(Emu *e, uint32_t sr);

uint32_t emu_physbase(Emu *e);
