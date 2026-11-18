/* Dev entry point (replaces main.c in the dev build): embeds Lua and exposes the port's memory, the reference emulators and comparison
   helpers to scripts, with an optional control socket for driving a live instance.

   devmain [--image game.stx] [--socket path] [script.lua [args...]]

   Lua API (first argument "ref" or "hyb" selects an emulator; "port" is the C port's own memory):
     emu.run(w) emu.vsync(w) emu.frames(w) emu.pc(w) emu.regs(w) emu.joy(w,v) emu.key(w,k)
     emu.read(w,addr,n) emu.rd8/16/32(w,addr) emu.save(w) emu.restore(w,id) emu.free(id)
     emu.watch(w,lo,hi,fn(pc,addr,size,value)) emu.unwatch(w) emu.backtrace(w) emu.trace(w,fn|nil)
     port.run(dir) port.on_sync(fn(id)) port.frames() port.stop() port.joy(v) port.key(k) port.rd8/16/32(addr) port.wr8/16/32(addr,v) port.field(name[,i])
     port.save() port.restore(id) port.sync(w)
     compare.ram(a,b[,mask_lo,mask_hi]) compare.screen(a,b)
     fields.describe(addr)  hybrid.install(w)  hybrid.stats()  quit([code])  control.poll(ms)
   Over the control socket each line is a Lua chunk (expressions print their value); "<<" starts a multi-line
   chunk ended by ">>". Replies end with a line containing a single ".". */
#include <errno.h>
#include <poll.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include "emu.h"
#include "game.h"
#include "sdl.h"
#include "guestmem.h"
#include "hybrid.h"

#define RAM_BYTES 0x100000u
#define MAX_STATES 512

static Emu *emus[2];
static lua_State *L;
static int quit_requested, quit_code;

/* ---- helpers ---- */

static int which_index(lua_State *l, int arg)
{
    const char *n = luaL_checkstring(l, arg);
    if (!strcmp(n, "ref")) return 0;
    if (!strcmp(n, "hyb")) return 1;
    return luaL_error(l, "unknown machine '%s' (expected ref or hyb)", n);
}

static Emu *getemu(lua_State *l, int arg) { return emus[which_index(l, arg)]; }

/* Big-endian RAM image of a source: "ref", "hyb" or "port". */
static void source_ram(lua_State *l, int arg, uint8_t *buf)
{
    const char *n = luaL_checkstring(l, arg);
    if (!strcmp(n, "port")) mem_to_st(buf, &mem);
    else emu_read(getemu(l, arg), 0, buf, RAM_BYTES);
}

static const MemField *field_at(uint32_t addr)
{
    for (unsigned i = 0; i < mem_field_count; i++) {
        const MemField *f = &mem_fields[i];
        if (addr >= f->addr && addr < f->addr + (uint32_t)f->size * f->count) return f;
    }
    return NULL;
}

static void describe(uint32_t addr, char *out, size_t n)
{
    const MemField *f = field_at(addr);
    if (f) snprintf(out, n, "%s+%u", f->name, addr - f->addr);
    else snprintf(out, n, "?");
}

/* ---- emu.* ---- */

static int l_emu_run(lua_State *l) { lua_pushboolean(l, emu_run_to_vsync(getemu(l, 1)) == 0); return 1; }
static int l_emu_vsync(lua_State *l) { emu_vsync(getemu(l, 1)); return 0; }
static int l_emu_frames(lua_State *l) { lua_pushinteger(l, emu_frames(getemu(l, 1))); return 1; }
static int l_emu_pc(lua_State *l) { lua_pushinteger(l, emu_pc(getemu(l, 1))); return 1; }
static int l_emu_joy(lua_State *l) { emu_set_joy1(getemu(l, 1), (uint8_t)luaL_checkinteger(l, 2)); return 0; }
static int l_emu_key(lua_State *l) { emu_push_key(getemu(l, 1), (uint32_t)luaL_checkinteger(l, 2)); return 0; }

static int l_emu_read(lua_State *l)
{
    lua_Integer n = luaL_checkinteger(l, 3);
    if (n < 0 || n > (lua_Integer)RAM_BYTES) return luaL_error(l, "bad length");
    uint8_t *buf = malloc((size_t)n + 1);
    emu_read(getemu(l, 1), (uint32_t)luaL_checkinteger(l, 2), buf, (size_t)n);
    lua_pushlstring(l, (char *)buf, (size_t)n);
    free(buf);
    return 1;
}

static int emu_rd(lua_State *l, int bytes)
{
    uint8_t b[4];
    emu_read(getemu(l, 1), (uint32_t)luaL_checkinteger(l, 2), b, (size_t)bytes);
    uint32_t v = 0;
    for (int i = 0; i < bytes; i++) v = v << 8 | b[i];
    lua_pushinteger(l, v);
    return 1;
}
static int l_emu_rd8(lua_State *l) { return emu_rd(l, 1); }
static int l_emu_rd16(lua_State *l) { return emu_rd(l, 2); }
static int l_emu_rd32(lua_State *l) { return emu_rd(l, 4); }

/* emu.wr8/16/32(name, address, value): the value goes to the emulator's memory, big endian. */
static int emu_wr(lua_State *l, int size)
{
    uint32_t value = (uint32_t)luaL_checkinteger(l, 3);
    uint8_t bytes[4];
    for (int i = 0; i < size; i++)
        bytes[i] = value >> (8 * (size - 1 - i));
    emu_write(getemu(l, 1), (uint32_t)luaL_checkinteger(l, 2), bytes, size);
    return 0;
}
static int l_emu_wr8(lua_State *l) { return emu_wr(l, 1); }
static int l_emu_wr16(lua_State *l) { return emu_wr(l, 2); }
static int l_emu_wr32(lua_State *l) { return emu_wr(l, 4); }

static int l_emu_regs(lua_State *l)
{
    uint32_t d[8], a[8];
    Emu *e = getemu(l, 1);
    emu_get_regs(e, d, a);
    lua_newtable(l);
    for (int i = 0; i < 8; i++) {
        lua_pushinteger(l, d[i]); lua_setfield(l, -2, (char[]){ 'd', (char)('0' + i), 0 });
        lua_pushinteger(l, a[i]); lua_setfield(l, -2, (char[]){ 'a', (char)('0' + i), 0 });
    }
    lua_pushinteger(l, emu_get_sr(e)); lua_setfield(l, -2, "sr");
    lua_pushinteger(l, emu_pc(e)); lua_setfield(l, -2, "pc");
    return 1;
}

static EmuState *emu_states[MAX_STATES];
static void *port_states[MAX_STATES];

static int l_emu_save(lua_State *l)
{
    for (int i = 0; i < MAX_STATES; i++)
        if (!emu_states[i]) { emu_states[i] = emu_save(getemu(l, 1)); lua_pushinteger(l, i); return 1; }
    return luaL_error(l, "too many savestates");
}

static int l_emu_restore(lua_State *l)
{
    lua_Integer id = luaL_checkinteger(l, 2);
    if (id < 0 || id >= MAX_STATES || !emu_states[id]) return luaL_error(l, "no such emulator state");
    emu_restore(getemu(l, 1), emu_states[id]);
    return 0;
}

static int l_emu_free(lua_State *l)
{
    lua_Integer id = luaL_checkinteger(l, 1);
    if (id >= 0 && id < MAX_STATES) { emu_state_free(emu_states[id]); emu_states[id] = NULL; }
    return 0;
}

static int watch_ref[2] = { LUA_NOREF, LUA_NOREF };
static int trace_ref[2] = { LUA_NOREF, LUA_NOREF };

static int emu_slot(Emu *e) { return e == emus[0] ? 0 : 1; }

static void on_watch(Emu *e, uint32_t pc, uint32_t addr, int size, uint32_t value, void *user)
{
    (void)user;
    lua_rawgeti(L, LUA_REGISTRYINDEX, watch_ref[emu_slot(e)]);
    lua_pushinteger(L, pc); lua_pushinteger(L, addr); lua_pushinteger(L, size); lua_pushinteger(L, value);
    if (lua_pcall(L, 4, 0, 0)) { fprintf(stderr, "watch callback: %s\n", lua_tostring(L, -1)); lua_pop(L, 1); }
}

static int l_emu_watch(lua_State *l)
{
    int w = which_index(l, 1);
    luaL_checktype(l, 4, LUA_TFUNCTION);
    luaL_unref(l, LUA_REGISTRYINDEX, watch_ref[w]);
    lua_pushvalue(l, 4);
    watch_ref[w] = luaL_ref(l, LUA_REGISTRYINDEX);
    emu_watch_write(emus[w], (uint32_t)luaL_checkinteger(l, 2), (uint32_t)luaL_checkinteger(l, 3), on_watch, NULL);
    return 0;
}

static int l_emu_unwatch(lua_State *l)
{
    int w = which_index(l, 1);
    emu_watch_clear(emus[w]);
    luaL_unref(l, LUA_REGISTRYINDEX, watch_ref[w]);
    watch_ref[w] = LUA_NOREF;
    return 0;
}

static void on_trace(uint32_t pc, void *user)
{
    int w = (int)(intptr_t)user;
    lua_rawgeti(L, LUA_REGISTRYINDEX, trace_ref[w]);
    lua_pushinteger(L, pc);
    if (lua_pcall(L, 1, 0, 0)) { fprintf(stderr, "trace callback: %s\n", lua_tostring(L, -1)); lua_pop(L, 1); }
}

static int l_emu_trace(lua_State *l)
{
    int w = which_index(l, 1);
    luaL_unref(l, LUA_REGISTRYINDEX, trace_ref[w]);
    trace_ref[w] = LUA_NOREF;
    if (lua_isnoneornil(l, 2)) {
        emu_trace_clear(emus[w]);
        return 0;
    }
    luaL_checktype(l, 2, LUA_TFUNCTION);
    lua_pushvalue(l, 2);
    trace_ref[w] = luaL_ref(l, LUA_REGISTRYINDEX);
    emu_trace(emus[w], on_trace, (void *)(intptr_t)w);
    return 0;
}

static int l_emu_backtrace(lua_State *l)
{
    uint32_t ret[16], tgt[16];
    int n = emu_backtrace(getemu(l, 1), ret, tgt, 16);
    lua_newtable(l);
    for (int i = 0; i < n; i++) {
        lua_newtable(l);
        lua_pushinteger(l, ret[i]); lua_setfield(l, -2, "ret");
        lua_pushinteger(l, tgt[i]); lua_setfield(l, -2, "target");
        lua_rawseti(l, -2, i + 1);
    }
    return 1;
}

/* ---- port.* ---- */

/* The port runs inside port.run(dir); the script gets control at every sync point through port.on_sync(fn(id)). */
static int sync_ref = LUA_NOREF;
static int resume_requested;
static jmp_buf quit_env, resume_env;
static int running;

static void on_port_quit(void)
{
    if (running)   /* outside port.run (a unit check of one routine) there is nothing to leave */
        longjmp(quit_env, 1);
}

static void on_port_sync(int id)
{
    if (sync_ref == LUA_NOREF) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, sync_ref);
    lua_pushinteger(L, id);
    if (lua_pcall(L, 1, 0, 0)) {
        fprintf(stderr, "on_sync: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        game_quit();
    }
    if (resume_requested) {   /* the script restored a state: the program goes on from the top of its main loop */
        resume_requested = 0;
        longjmp(resume_env, 1);
    }
}

static int l_port_resume(lua_State *l) { (void)l; resume_requested = 1; return 0; }

static int l_port_on_sync(lua_State *l)
{
    luaL_unref(l, LUA_REGISTRYINDEX, sync_ref);
    sync_ref = LUA_NOREF;
    if (!lua_isnoneornil(l, 1)) {
        luaL_checktype(l, 1, LUA_TFUNCTION);
        lua_pushvalue(l, 1);
        sync_ref = luaL_ref(l, LUA_REGISTRYINDEX);
    }
    game_hooks.sync = on_port_sync;
    return 0;
}

/* port.on_unported(fn(address)): the port was sent to something that is not code; fn may restore savestates and return true
 * to have the port carry on from the top of its main loop. */
static int unported_ref = LUA_NOREF;

static int on_port_unported(uint32_t address)
{
    if (unported_ref == LUA_NOREF) return 0;
    lua_rawgeti(L, LUA_REGISTRYINDEX, unported_ref);
    lua_pushinteger(L, address);
    if (lua_pcall(L, 1, 1, 0)) {
        fprintf(stderr, "on_unported: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return 0;
    }
    int resumed = lua_toboolean(L, -1);
    lua_pop(L, 1);
    if (resumed && running)
        longjmp(resume_env, 1);
    return resumed;
}

static int l_port_on_unported(lua_State *l)
{
    luaL_unref(l, LUA_REGISTRYINDEX, unported_ref);
    unported_ref = LUA_NOREF;
    if (!lua_isnoneornil(l, 1)) {
        luaL_checktype(l, 1, LUA_TFUNCTION);
        lua_pushvalue(l, 1);
        unported_ref = luaL_ref(l, LUA_REGISTRYINDEX);
    }
    game_hooks.unported = on_port_unported;
    return 0;
}

/* The program runs until it ends or game_quit(); a restored state is carried on from the top of its main loop. */
static int l_port_run(lua_State *l)
{
    if (game_load(luaL_optstring(l, 1, "extracted")) < 0) {
        lua_pushinteger(l, -1);
        return 1;
    }
    game_hooks.quit = on_port_quit;
    running = 1;
    if (!setjmp(quit_env)) {
        if (setjmp(resume_env))
            play_game();
        else
            program_entry();
    }
    running = 0;
    lua_pushinteger(l, 0);
    return 1;
}
/* port.call(name): runs one routine of the port that takes no arguments, on the port's current memory. */
static void call_room_state(void) { run_room_state(mem.vars.state_routine); }

static const struct { const char *name; void (*fn)(void); } callable[] = {
    { "room_state", call_room_state },
    { "draw_sprite_queue", draw_sprite_queue }, { "present_back_buffer", present_back_buffer },
    { "reset_game_state", reset_game_state }, { "place_player", place_player },
    { "copy_object_templates", copy_object_templates }, { "mark_entities", mark_entities },
    { "build_block_map", build_block_map }, { "draw_room", draw_room }, { "place_room_objects", place_room_objects },
    { "draw_status_panel", draw_status_panel }, { "update_message", update_message },
    { "update_clock", update_clock }, { "update_score_display", update_score_display },
    { "draw_fatigue_bar", draw_fatigue_bar }, { "copy_screen_to_back_buffer", copy_screen_to_back_buffer },
    { "scatter_end_pictures", scatter_end_pictures }, { "update_companion", update_companion },
    { "update_second_companion", update_second_companion }, { "fatigue_up", fatigue_up }, { "check_room_exit", check_room_exit }, { "update_first_entity", update_first_entity },
    { "shake_object", shake_object }, { "move_walking_object", move_walking_object }, { "drop_objects", drop_objects },
    { "move_thrown_object", move_thrown_object }, { "fall_asleep", fall_asleep }, { "restart_sound_if_done", restart_sound_if_done }, { "draw_room_sprites", draw_room_sprites },
};

/* port.call_entity(name, n): a routine of an entity, the n'th of the entities. */
static int l_port_call_entity(lua_State *l)
{
    const char *name = luaL_checkstring(l, 1);
    lua_Integer n = luaL_checkinteger(l, 2);
    if (n < 0 || n >= 24)
        return luaL_error(l, "no such entity");
    if (!strcmp(name, "update_entity")) { update_entity(&mem.text.entities[n]); return 0; }
    return luaL_error(l, "no entity routine '%s'", name);
}

static int l_port_call(lua_State *l)
{
    const char *name = luaL_checkstring(l, 1);
    for (unsigned i = 0; i < sizeof callable / sizeof *callable; i++)
        if (!strcmp(callable[i].name, name)) { callable[i].fn(); return 0; }
    return luaL_error(l, "no callable routine '%s'", name);
}
static int l_port_frames(lua_State *l) { lua_pushinteger(l, game_frames()); return 1; }
static int l_port_quit(lua_State *l) { (void)l; game_quit(); return 0; }
static int l_port_joy(lua_State *l) { hw.joy1 = (uint8_t)luaL_checkinteger(l, 1); return 0; }
static int l_port_key(lua_State *l) { hw_push_key((uint32_t)luaL_checkinteger(l, 1)); return 0; }
static int l_port_rd8(lua_State *l) { lua_pushinteger(l, guest_rd8((uint32_t)luaL_checkinteger(l, 1))); return 1; }
static int l_port_rd16(lua_State *l) { lua_pushinteger(l, guest_rd16((uint32_t)luaL_checkinteger(l, 1))); return 1; }
static int l_port_rd32(lua_State *l) { lua_pushinteger(l, guest_rd32((uint32_t)luaL_checkinteger(l, 1))); return 1; }
static int l_port_wr8(lua_State *l) { guest_wr8((uint32_t)luaL_checkinteger(l, 1), (uint8_t)luaL_checkinteger(l, 2)); return 0; }
static int l_port_wr16(lua_State *l) { guest_wr16((uint32_t)luaL_checkinteger(l, 1), (uint16_t)luaL_checkinteger(l, 2)); return 0; }
static int l_port_wr32(lua_State *l) { guest_wr32((uint32_t)luaL_checkinteger(l, 1), (uint32_t)luaL_checkinteger(l, 2)); return 0; }

static const MemField *find_field(lua_State *l, const char *name)
{
    for (unsigned i = 0; i < mem_field_count; i++)
        if (!strcmp(mem_fields[i].name, name)) return &mem_fields[i];
    luaL_error(l, "no field named '%s'", name);
    return NULL;
}

static int l_port_field(lua_State *l)
{
    const MemField *f = find_field(l, luaL_checkstring(l, 1));
    lua_Integer i = luaL_optinteger(l, 2, 0);
    if (i < 0 || (uint32_t)i >= f->count) return luaL_error(l, "index out of range");
    const uint8_t *p = (const uint8_t *)&mem + f->addr + (size_t)i * f->size;
    lua_pushinteger(l, f->size == 1 ? *p : f->size == 2 ? *(const uint16_t *)p : *(const uint32_t *)p);
    return 1;
}

typedef struct { StMem mem; Hardware hw; uint16_t palette[16]; } PortState;

static int l_port_save(lua_State *l)
{
    for (int i = 0; i < MAX_STATES; i++)
        if (!port_states[i]) {
            PortState *s = malloc(sizeof *s);
            memcpy(&s->mem, &mem, sizeof mem);
            memcpy(&s->hw, &hw, sizeof hw);
            memcpy(s->palette, hardware_palette, sizeof hardware_palette);
            port_states[i] = s;
            lua_pushinteger(l, i);
            return 1;
        }
    return luaL_error(l, "too many savestates");
}

static int l_port_restore(lua_State *l)
{
    lua_Integer id = luaL_checkinteger(l, 1);
    if (id < 0 || id >= MAX_STATES || !port_states[id]) return luaL_error(l, "no such port state");
    PortState *s = port_states[id];
    memcpy(&mem, &s->mem, sizeof mem);
    memcpy(&hw, &s->hw, sizeof hw);
    memcpy(hardware_palette, s->palette, sizeof hardware_palette);
    return 0;
}

/* Overwrites the port's memory with an emulator's RAM, so the port can be run from a reference savestate. */
static int l_port_sync(lua_State *l)
{
    static uint8_t buf[RAM_BYTES];
    uint8_t pal[32];
    Emu *e = getemu(l, 1);
    emu_read(e, 0, buf, RAM_BYTES);
    mem_from_st(&mem, buf);
    emu_read(e, 0xFF8240, pal, sizeof pal);
    for (int i = 0; i < 16; i++) hardware_palette[i] = (uint16_t)(pal[i * 2] << 8 | pal[i * 2 + 1]);
    return 0;
}

/* ---- compare.* / fields.* / hybrid.* ---- */

static int l_compare_ram(lua_State *l)
{
    static uint8_t a[RAM_BYTES], b[RAM_BYTES];
    source_ram(l, 1, a);
    source_ram(l, 2, b);
    /* masked regions (dead stack, code not reconstructed yet) count as equal: either lo, hi or a list of {lo, hi} */
    if (lua_istable(l, 3)) {
        for (int i = 1;; i++) {
            lua_rawgeti(l, 3, i);
            if (lua_isnil(l, -1)) { lua_pop(l, 1); break; }
            lua_rawgeti(l, -1, 1);
            lua_rawgeti(l, -2, 2);
            uint32_t lo = (uint32_t)lua_tointeger(l, -2), hi = (uint32_t)lua_tointeger(l, -1);
            lua_pop(l, 3);
            if (hi > lo && hi <= RAM_BYTES) memcpy(b + lo, a + lo, hi - lo);
        }
    } else {
        uint32_t mlo = (uint32_t)luaL_optinteger(l, 3, 0), mhi = (uint32_t)luaL_optinteger(l, 4, 0);
        if (mhi > mlo) memcpy(b + mlo, a + mlo, mhi - mlo);
    }
    lua_newtable(l);
    int n = 0;
    for (uint32_t i = 0; i < RAM_BYTES && n < 32; i++) {
        if (a[i] == b[i]) continue;
        uint32_t j = i;
        while (j < RAM_BYTES && a[j] != b[j]) j++;
        char d[96];
        describe(i, d, sizeof d);
        lua_newtable(l);
        lua_pushinteger(l, i); lua_setfield(l, -2, "lo");
        lua_pushinteger(l, j - 1); lua_setfield(l, -2, "hi");
        lua_pushstring(l, d); lua_setfield(l, -2, "field");
        lua_pushinteger(l, a[i]); lua_setfield(l, -2, "a");
        lua_pushinteger(l, b[i]); lua_setfield(l, -2, "b");
        lua_rawseti(l, -2, ++n);
        i = j;
    }
    return 1;
}

static int l_compare_screen(lua_State *l)
{
    static uint8_t a[RAM_BYTES], b[RAM_BYTES];
    source_ram(l, 1, a);
    source_ram(l, 2, b);
    int diff = 0, first = -1;
    for (int i = 0; i < ST_W * ST_H / 2; i++)
        if (a[ADDR_SCREEN + i] != b[ADDR_SCREEN + i]) { if (first < 0) first = i; diff++; }
    lua_pushinteger(l, diff);
    lua_pushinteger(l, first < 0 ? -1 : (lua_Integer)ADDR_SCREEN + first);
    return 2;
}

/* The port's palette against the emulator's colour registers: how many of the 16 colours differ, and the first. */
static int l_compare_palette(lua_State *l)
{
    uint8_t pal[32];
    emu_read(getemu(l, 1), 0xFF8240, pal, sizeof pal);
    int diff = 0, first = -1;
    for (int i = 0; i < 16; i++)
        if (hardware_palette[i] != (uint16_t)(pal[i * 2] << 8 | pal[i * 2 + 1])) { if (first < 0) first = i; diff++; }
    lua_pushinteger(l, diff);
    lua_pushinteger(l, first);
    return 2;
}

/* compare.render(ref): the picture the front end draws from the port (game_render) against one worked out here from the
 * reference's screen memory and colour registers: the number of pixels that differ. */
static int l_compare_render(lua_State *l)
{
    static uint32_t drawn[ST_W * ST_H];
    static uint8_t screen[ST_W * ST_H / 2], pal[32];
    Emu *e = getemu(l, 1);
    game_render(drawn);
    emu_read(e, ADDR_SCREEN, screen, sizeof screen);
    emu_read(e, 0xFF8240, pal, sizeof pal);
    uint32_t colour[16];
    for (int i = 0; i < 16; i++) {
        unsigned c = (unsigned)(pal[2 * i] << 8 | pal[2 * i + 1]);
        unsigned r = (c >> 8) & 7, g = (c >> 4) & 7, b = c & 7;
        r = r << 5 | r << 2 | r >> 1; g = g << 5 | g << 2 | g >> 1; b = b << 5 | b << 2 | b >> 1;   /* 3 bits to 8, as the front end does */
        colour[i] = 0xFF000000u | b << 16 | g << 8 | r;
    }
    int diff = 0;
    for (int y = 0; y < ST_H; y++)
        for (int x = 0; x < ST_W; x++) {
            const uint8_t *group = &screen[y * 160 + (x / 16) * 8];
            int bit = 15 - x % 16, index = 0;
            for (int plane = 0; plane < 4; plane++) {
                unsigned word = (unsigned)(group[2 * plane] << 8 | group[2 * plane + 1]);
                index |= (int)(word >> bit & 1) << plane;
            }
            if (drawn[y * ST_W + x] != colour[index]) diff++;
        }
    lua_pushinteger(l, diff);
    return 1;
}

static int l_fields_describe(lua_State *l)
{
    char d[96];
    describe((uint32_t)luaL_checkinteger(l, 1), d, sizeof d);
    lua_pushstring(l, d);
    return 1;
}

static int l_hybrid_install(lua_State *l) { hybrid_install(getemu(l, 1)); return 0; }

static int l_hybrid_stats(lua_State *l)
{
    static const char *names[] = { "joy_handler", "joy_decode", "present", "draw_queue", "scroll_rows" };
    lua_newtable(l);
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++) {
        lua_newtable(l);
        lua_pushinteger(l, hybrid_lifted_calls[i]); lua_setfield(l, -2, "calls");
        lua_pushinteger(l, hybrid_fallbacks[i]); lua_setfield(l, -2, "fallbacks");
        lua_setfield(l, -2, names[i]);
    }
    return 1;
}

static int l_quit(lua_State *l)
{
    quit_requested = 1;
    quit_code = (int)luaL_optinteger(l, 1, 0);
    return 0;
}

/* ---- control socket ---- */

static int listen_fd = -1, client_fd = -1;
static char rbuf[1 << 16];
static size_t rlen;
static char *out_buf;
static size_t out_len, out_cap;
static int capture;

static void out_append(const char *s, size_t n)
{
    if (out_len + n + 1 > out_cap) out_buf = realloc(out_buf, out_cap = (out_len + n + 1) * 2);
    memcpy(out_buf + out_len, s, n);
    out_len += n;
}

static int l_print(lua_State *l)
{
    int n = lua_gettop(l);
    for (int i = 1; i <= n; i++) {
        size_t len;
        const char *s = luaL_tolstring(l, i, &len);
        if (capture) { if (i > 1) out_append("\t", 1); out_append(s, len); }
        else { if (i > 1) fputc('\t', stdout); fwrite(s, 1, len, stdout); }
        lua_pop(l, 1);
    }
    if (capture) out_append("\n", 1); else fputc('\n', stdout);
    return 0;
}

/* Runs a chunk; an expression prints its value like a REPL. */
static void run_chunk(const char *code)
{
    char *as_expr = NULL;
    if (asprintf(&as_expr, "return %s", code) < 0) return;
    int top = lua_gettop(L);
    int rc = luaL_loadstring(L, as_expr);
    if (rc != LUA_OK) { lua_pop(L, 1); rc = luaL_loadstring(L, code); }
    free(as_expr);
    if (rc == LUA_OK) rc = lua_pcall(L, 0, LUA_MULTRET, 0);
    if (rc != LUA_OK) {
        const char *m = lua_tostring(L, -1);
        out_append("error: ", 7);
        out_append(m ? m : "?", m ? strlen(m) : 1);
        out_append("\n", 1);
    } else {
        for (int i = top + 1; i <= lua_gettop(L); i++) {
            size_t len;
            const char *s = luaL_tolstring(L, i, &len);
            out_append(s, len);
            out_append("\n", 1);
            lua_pop(L, 1);
        }
    }
    lua_settop(L, top);
}

static void reply(void)
{
    out_append(".\n", 2);
    for (size_t off = 0; off < out_len;) {
        ssize_t w = write(client_fd, out_buf + off, out_len - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
    out_len = 0;
}

static void handle_input(void)
{
    static int multi;
    static char *chunk;
    static size_t chunk_len;
    char *nl;
    while ((nl = memchr(rbuf, '\n', rlen))) {
        *nl = 0;
        if (nl > rbuf && nl[-1] == '\r') nl[-1] = 0;
        if (!multi && !strcmp(rbuf, "<<")) { multi = 1; chunk_len = 0; }
        else if (multi && !strcmp(rbuf, ">>")) {
            multi = 0;
            chunk = realloc(chunk, chunk_len + 1);
            chunk[chunk_len] = 0;
            capture = 1; run_chunk(chunk); capture = 0;
            reply();
        } else if (multi) {
            size_t n = strlen(rbuf);
            chunk = realloc(chunk, chunk_len + n + 2);
            memcpy(chunk + chunk_len, rbuf, n);
            chunk[chunk_len += n] = '\n';
            chunk_len++;
        } else {
            capture = 1; run_chunk(rbuf); capture = 0;
            reply();
        }
        size_t used = (size_t)(nl - rbuf) + 1;
        memmove(rbuf, rbuf + used, rlen - used);
        rlen -= used;
    }
}

static int control_listen(const char *path)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof addr.sun_path) {
        fprintf(stderr, "socket path too long (max %zu bytes): %s\n", sizeof addr.sun_path - 1, path);
        return -1;
    }
    strcpy(addr.sun_path, path);
    unlink(path);
    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0 || bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(listen_fd, 1) < 0) {
        perror(path);
        return -1;
    }
    return 0;
}

static void control_poll(int timeout_ms)
{
    if (listen_fd < 0) return;
    struct pollfd p[2] = { { listen_fd, POLLIN, 0 }, { client_fd, POLLIN, 0 } };
    if (poll(p, client_fd >= 0 ? 2 : 1, timeout_ms) <= 0) return;
    if (p[0].revents & POLLIN) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd >= 0) { if (client_fd >= 0) close(client_fd); client_fd = fd; rlen = 0; }
    }
    if (client_fd >= 0 && (p[1].revents & (POLLIN | POLLHUP))) {
        ssize_t n = read(client_fd, rbuf + rlen, sizeof rbuf - rlen - 1);
        if (n <= 0) { close(client_fd); client_fd = -1; rlen = 0; return; }
        rlen += (size_t)n;
        handle_input();
    }
}

static int l_control_poll(lua_State *l) { control_poll((int)luaL_optinteger(l, 1, 0)); return 0; }

/* ---- setup ---- */

#define FN(name) { #name, l_##name }
static void reg(const char *table, const luaL_Reg *fns)
{
    lua_getglobal(L, table);
    if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_newtable(L); lua_pushvalue(L, -1); lua_setglobal(L, table); }
    luaL_setfuncs(L, fns, 0);
    lua_pop(L, 1);
}

static void open_api(void)
{
    static const luaL_Reg emu_fns[] = {
        { "run", l_emu_run }, { "vsync", l_emu_vsync }, { "frames", l_emu_frames }, { "pc", l_emu_pc },
        { "regs", l_emu_regs }, { "joy", l_emu_joy }, { "key", l_emu_key }, { "read", l_emu_read },
        { "wr8", l_emu_wr8 }, { "wr16", l_emu_wr16 }, { "wr32", l_emu_wr32 }, { "rd8", l_emu_rd8 }, { "rd16", l_emu_rd16 }, { "rd32", l_emu_rd32 }, { "save", l_emu_save },
        { "restore", l_emu_restore }, { "free", l_emu_free }, { "watch", l_emu_watch },
        { "unwatch", l_emu_unwatch }, { "trace", l_emu_trace }, { "backtrace", l_emu_backtrace }, { NULL, NULL } };
    static const luaL_Reg port_fns[] = {
        { "call", l_port_call }, { "call_entity", l_port_call_entity },
        { "run", l_port_run }, { "on_sync", l_port_on_sync }, { "resume", l_port_resume }, { "on_unported", l_port_on_unported }, { "frames", l_port_frames }, { "stop", l_port_quit }, { "joy", l_port_joy }, { "key", l_port_key }, { "rd8", l_port_rd8 }, { "rd16", l_port_rd16 },
        { "rd32", l_port_rd32 }, { "wr8", l_port_wr8 }, { "wr16", l_port_wr16 }, { "wr32", l_port_wr32 },
        { "field", l_port_field }, { "save", l_port_save }, { "restore", l_port_restore },
        { "sync", l_port_sync }, { NULL, NULL } };
    static const luaL_Reg compare_fns[] = { { "ram", l_compare_ram }, { "screen", l_compare_screen }, { "palette", l_compare_palette }, { "render", l_compare_render }, { NULL, NULL } };
    static const luaL_Reg fields_fns[] = { { "describe", l_fields_describe }, { NULL, NULL } };
    static const luaL_Reg hybrid_fns[] = { { "install", l_hybrid_install }, { "stats", l_hybrid_stats }, { NULL, NULL } };
    static const luaL_Reg control_fns[] = { { "poll", l_control_poll }, { NULL, NULL } };
    reg("emu", emu_fns); reg("port", port_fns); reg("compare", compare_fns);
    reg("fields", fields_fns); reg("hybrid", hybrid_fns); reg("control", control_fns);
    lua_pushcfunction(L, l_quit); lua_setglobal(L, "quit");
    lua_pushcfunction(L, l_print); lua_setglobal(L, "print");
}

int main(int argc, char **argv)
{
    const char *image = "Garfield - Big, Fat, Hairy Deal.stx", *socket_path = NULL, *script = NULL;
    int first_arg = argc;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--image") && i + 1 < argc) image = argv[++i];
        else if (!strcmp(argv[i], "--socket") && i + 1 < argc) socket_path = argv[++i];
        else { script = argv[i]; first_arg = i + 1; break; }
    }
    emus[0] = emu_new(image);
    emus[1] = emu_new(image);
    if (!emus[0] || !emus[1]) return 1;
    hybrid_install(emus[1]);

    L = luaL_newstate();
    luaL_openlibs(L);
    open_api();
    lua_newtable(L);
    for (int i = first_arg; i < argc; i++) { lua_pushstring(L, argv[i]); lua_rawseti(L, -2, i - first_arg + 1); }
    lua_setglobal(L, "args");

    int rc = 0;
    if (script && (luaL_loadfile(L, script) || lua_pcall(L, 0, 0, 0))) {
        fprintf(stderr, "%s\n", lua_tostring(L, -1));
        rc = 1;
    }
    if (socket_path && !rc) {
        if (control_listen(socket_path) < 0) {
            rc = 1;
        } else {
            fprintf(stderr, "control socket: %s\n", socket_path);
            while (!quit_requested) control_poll(200);
            unlink(socket_path);
        }
    } else if (!script) {
        fprintf(stderr, "usage: %s [--image game.stx] [--socket path] [script.lua [args...]]\n", argv[0]);
        rc = 2;
    }
    lua_close(L);
    return rc ? rc : quit_code;
}
