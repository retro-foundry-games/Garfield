#include "hybrid.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game.h"
#include "sdl.h"

#define TEXT_BASE 0x10100u
#define RAM_BYTES 0x100000u

unsigned hybrid_lifted_calls[8], hybrid_fallbacks[8];
static uint8_t ram_buf[RAM_BYTES], ram_after[RAM_BYTES];

/* An adapter calls a routine of the port with the arguments the original passed in registers and puts the
   original's register and flag side effects back, so the emulator carries on exactly as it would have. It
   returns 0 to let the original code run instead. */
typedef struct { uint32_t d[8], a[8], sr; } Regs;
typedef int (*Adapter)(Regs *r);

#define CCR_X 0x10
#define CCR_N 0x08
#define CCR_Z 0x04
#define CCR_V 0x02
#define CCR_C 0x01
#define ORIGINAL_TEXT(off) (TEXT_BASE + (off))

static int call_port(Emu *e, Adapter fn, int slot)
{
    Regs r;
    emu_read(e, 0, ram_buf, RAM_BYTES);
    mem_from_st(&mem, ram_buf);
    emu_get_regs(e, r.d, r.a);
    r.sr = emu_get_sr(e);
    if (!fn(&r)) {
        hybrid_fallbacks[slot]++;
        return 0;
    }
    hybrid_lifted_calls[slot]++;
    mem_to_st(ram_after, &mem);
    for (uint32_t i = 0; i < RAM_BYTES;) {  /* only what the routine changed goes back */
        if (ram_after[i] == ram_buf[i]) { i++; continue; }
        uint32_t j = i;
        while (j < RAM_BYTES && ram_after[j] != ram_buf[j]) j++;
        emu_write(e, i, ram_after + i, j - i);
        i = j;
    }
    emu_set_regs(e, r.d, r.a);
    emu_set_sr(e, r.sr);
    return 1;
}

/* $06FE: a0 = the packet */
static int adapt_ikbd_joystick_event(Regs *r)
{
    ikbd_joystick_event(ST_PTR(const uint8_t, r->a[0]));
    return 1;
}

/* $0716 */
static int adapt_joystick_direction(Regs *r)
{
    int led = mem.vars.progress_flags & 0x40;
    uint8_t s = mem.text.joy_state;
    if (!led && mem.text.joy_dir_table[(s & 0xF) | ((s >> 3) & 0x10)] == 0 &&
        (uint16_t)(mem.vars.joy_idle + 1) == 0x80 && !(mem.vars.mode_flags & 8))
        return 0;  /* the idle timeout starts routines that are not reconstructed */
    uint8_t dir = joystick_direction();
    if (led) {
        r->d[0] = dir == 5 ? 5 : (r->d[0] & 0xFFFF0000u) | 4;
        r->d[7] = (r->d[7] & 0xFFFF0000u) | mem.text.entities[PLAYER].x;
    } else {
        r->d[1] = (r->d[1] & ~0xFFu) | (s & 0xF);
        r->d[0] = dir;
        r->a[0] = ORIGINAL_TEXT(0x7304);
    }
    return 1;
}

/* $6310 */
static int adapt_draw_sprite_queue(Regs *r)
{
    unsigned n = mem.vars.queue_count;
    if (n) {
        const struct DrawEntry *first = (const struct DrawEntry *)((const uint8_t *)&mem.vars + mem.vars.queue_base);
        r->a[0] = first->dest + 32 * 0x30;  /* the last entry drawn is the first of the list */
        r->a[1] = first->source + 32 * ((first->dest >> 31) ? 24 : 16);
        r->d[0] = 0;
        r->d[1] = 0xFFFF;
        r->d[2] = 0x30;
    }
    draw_sprite_queue();
    r->sr = (r->sr & ~0x0Fu) | CCR_Z;  /* the loop ends on tst.w d0 with d0 = 0 */
    return 1;
}

/* $62A4: d5 = bits to shift, a2 = the rows, X = the bit coming in */
static int adapt_shift_sprite_right(Regs *r)
{
    if ((r->d[5] & 0xFFFF) > 7)
        return 0;  /* the original would jump past its table */
    unsigned bits = r->d[5] & 7;
    uint16_t *rows = ST_PTR(uint16_t, r->a[2]);
    unsigned out = shift_sprite_right(rows, bits, (r->sr & CCR_X) != 0);
    uint16_t last = rows[127 * 3 + 2];
    r->d[0] = 0xFFFF;
    r->d[2] = 0;
    if (bits == 0)
        r->sr = (r->sr & ~0x0Fu) | CCR_Z;  /* the flags are the last moveq's */
    else
        r->sr = (r->sr & ~0x1Fu) | (out ? CCR_X | CCR_C : 0) | (last & 0x8000 ? CCR_N : 0) | (last == 0 ? CCR_Z : 0);
    return 1;
}

/* $6E7C */
static int adapt_present_back_buffer(Regs *r)
{
    uint32_t src = mem.vars.back_buf + 0x600 + 4 + 127 * 8 + 128 * 40;
    present_back_buffer();
    r->d[0] = (r->d[0] & 0xFFFF0000u) | 0xFFFF;
    r->d[1] = 8;
    r->a[0] = mem.vars.screen_ptr + 128 * 160;
    r->a[1] = src;
    r->a[2] = src + 0x1E00;
    r->a[3] = src + 2 * 0x1E00;
    r->a[4] = src + 3 * 0x1E00;
    return 1;
}

#define ADAPT(name, slot) \
    static int hyb_##name(Emu *e, void *u) { (void)u; return call_port(e, adapt_##name, slot); }
ADAPT(ikbd_joystick_event, 0)
ADAPT(joystick_direction, 1)
ADAPT(present_back_buffer, 2)
ADAPT(draw_sprite_queue, 3)
ADAPT(shift_sprite_right, 4)

/* HYB_SKIP=<slots>, e.g. "34": leaves those routines to the original code (bisecting a mismatch). */
void hybrid_install(Emu *e)
{
    const char *skip = getenv("HYB_SKIP") ? getenv("HYB_SKIP") : "";
    static const struct { uint32_t addr; EmuRoutineFn fn; } routines[] = {
        { 0x6FE, hyb_ikbd_joystick_event }, { 0x716, hyb_joystick_direction }, { 0x6E7C, hyb_present_back_buffer },
        { 0x6310, hyb_draw_sprite_queue }, { 0x62A4, hyb_shift_sprite_right } };
    for (int i = 0; i < 5; i++)
        if (!strchr(skip, '0' + i))
            emu_replace(e, TEXT_BASE + routines[i].addr, routines[i].fn, NULL);
}

static Emu *machine;

int hybrid_init(const char *stx_path)
{
    machine = emu_new(stx_path);
    if (!machine)
        return -1;
    hybrid_install(machine);
    return 0;
}

int hybrid_frame(void)
{
    if (emu_run_to_vsync(machine) < 0)
        return -1;
    /* The port's view of the machine: all of RAM as typed memory, plus the hardware palette. */
    emu_read(machine, 0, ram_buf, RAM_BYTES);
    mem_from_st(&mem, ram_buf);
    uint8_t pal[32];
    emu_read(machine, 0xFF8240, pal, sizeof pal);
    for (int i = 0; i < 16; i++)
        hardware_palette[i] = (uint16_t)(pal[i * 2] << 8 | pal[i * 2 + 1]);
    emu_set_joy1(machine, hw.joy1);
    for (uint32_t k; (k = hw_pop_key()) != 0;)
        emu_push_key(machine, k);
    emu_vsync(machine);
    return 0;
}
