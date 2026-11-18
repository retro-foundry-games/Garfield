#include <SDL3/SDL.h>
#include "sdl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

Hardware hw;
uint16_t hardware_palette[16];

/* ---- memory image <-> emulator RAM ---- */

/* the multi-byte members, by address (see GUEST_MEMBERS in game.h) */

#define MEMBER_ENTRY(addr, member, elem) \
    { #member, (addr), (elem), (uint32_t)(sizeof(((StMem *)0)->member) / (elem)) },
const MemField mem_fields[] = { GUEST_MEMBERS(MEMBER_ENTRY) };
const unsigned mem_field_count = sizeof mem_fields / sizeof mem_fields[0];

static void copy_swapped(uint8_t *dst, const uint8_t *src, unsigned size, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
        for (unsigned b = 0; b < size; b++)
            dst[i * size + b] = src[i * size + size - 1 - b];
}

/* Raw bytes are copied as-is; only declared multi-byte fields are swapped. */
static void convert(uint8_t *dst, const uint8_t *src)
{
    memcpy(dst, src, ST_RAM_SIZE);
    for (unsigned i = 0; i < mem_field_count; i++) {
        const MemField *f = &mem_fields[i];
        if (f->size > 1)
            copy_swapped(dst + f->addr, src + f->addr, f->size, f->count);
    }
}

/* The pointer members: the original stores addresses, the port pointers into `mem` (0 stays 0). */
#define POINTER_ENTRY(addr, member) { (addr), (uint32_t)(sizeof(((StMem *)0)->member) / 4) },
static const struct { uint32_t addr, count; } pointer_members[] = { POINTER_MEMBERS(POINTER_ENTRY) };

int mem_is_pointer(uint32_t address)
{
    address &= 0xFFFFFF;
    for (unsigned i = 0; i < sizeof pointer_members / sizeof *pointer_members; i++)
        if (address >= pointer_members[i].addr && address < pointer_members[i].addr + 4 * pointer_members[i].count)
            return 1;
    return 0;
}

static uint32_t be32_at(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put_be32_at(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* A pointer member holds a pointer into `mem` or, as `Entity.table` does for some things, plain data: what is below 1 MiB
 * is an address, anything else is left as it is. */
uint32_t mem_pointer_of_address(uint32_t address)
{
    return address && address < ST_RAM_SIZE ? (uint32_t)(uintptr_t)((uint8_t *)&mem + address) : address;
}

uint32_t mem_address_of_pointer(uint32_t pointer)
{
    uint32_t offset = pointer - (uint32_t)(uintptr_t)&mem;
    return pointer && offset < ST_RAM_SIZE ? offset : pointer;
}

void mem_from_st(StMem *dst, const uint8_t *st_ram)
{
    convert((uint8_t *)dst, st_ram);
    for (unsigned i = 0; i < sizeof pointer_members / sizeof *pointer_members; i++) {
        uint32_t *word = (uint32_t *)((uint8_t *)dst + pointer_members[i].addr);
        for (uint32_t n = 0; n < pointer_members[i].count; n++) {
            uint32_t v = be32_at(st_ram + pointer_members[i].addr + 4 * n);
            word[n] = v && v < ST_RAM_SIZE ? (uint32_t)(uintptr_t)((uint8_t *)dst + v) : v;
        }
    }
}

void mem_to_st(uint8_t *st_ram, const StMem *src)
{
    convert(st_ram, (const uint8_t *)src);
    for (unsigned i = 0; i < sizeof pointer_members / sizeof *pointer_members; i++) {
        const uint32_t *word = (const uint32_t *)((const uint8_t *)src + pointer_members[i].addr);
        for (uint32_t n = 0; n < pointer_members[i].count; n++) {
            uint32_t p = word[n];
            uint32_t offset = p - (uint32_t)(uintptr_t)src;
            put_be32_at(st_ram + pointer_members[i].addr + 4 * n, p && offset < ST_RAM_SIZE ? offset : p);
        }
    }
}


/* ---- ST display ---- */

static uint32_t expand(uint16_t c)
{
    uint32_t r = (c >> 8) & 7, g = (c >> 4) & 7, b = c & 7;
    r = r << 5 | r << 2 | r >> 1;
    g = g << 5 | g << 2 | g >> 1;
    b = b << 5 | b << 2 | b >> 1;
    return 0xFF000000u | b << 16 | g << 8 | r;
}

void game_render(uint32_t *rgba)
{
    uint32_t pal[16];
    for (int i = 0; i < 16; i++)
        pal[i] = expand(hardware_palette[i]);
    for (int y = 0; y < ST_H; y++) {
        for (int g = 0; g < ST_W / 16; g++) {
            const uint16_t *pl = &mem.screen.planes[y * 80 + g * 4];
            for (int x = 0; x < 16; x++) {
                int s = 15 - x;
                int c = (pl[0] >> s & 1) | (pl[1] >> s & 1) << 1 | (pl[2] >> s & 1) << 2 | (pl[3] >> s & 1) << 3;
                rgba[y * ST_W + g * 16 + x] = pal[c];
            }
        }
    }
}

void hw_push_key(uint32_t key)
{
    if (hw.key_head - hw.key_tail < 16)
        hw.keys[hw.key_head++ % 16] = key;
}

uint32_t hw_pop_key(void)
{
    return hw.key_head == hw.key_tail ? 0 : hw.keys[hw.key_tail++ % 16];
}


/* ---- running the program ---- */

GameHooks game_hooks;

#define BASEPAGE     0x10000u
#define SCREEN       (ST_RAM_SIZE - 0x8000u)
#define STACK_TOP    (SCREEN - 0x100u)
#define KBDVECS      0x600u
#define JOYVEC       (KBDVECS + 0x18u)
#define JOY_PACKET   0x640u
#define STUB_RTE_VBL 0x500u  /* the old VBL vector: TOS's own handler, which only returns */
#define STUB_RTE     0x502u
#define STUB_RTS     0x504u
#define STUB_JOY_RET 0x506u

static char data_dir[512];
static uint32_t frames;

uint32_t game_frames(void) { return frames; }
void game_quit(void)
{
    if (game_hooks.quit) {
        game_hooks.quit();
        return;
    }
    SDL_Log("exit after %u frames, player x=%u y=%u, room %u", game_frames(), mem.text.entities[PLAYER].x, mem.text.entities[PLAYER].y,
            mem.vars.room);
    SDL_Quit();
    exit(0);
}

void game_unported(uint32_t address)
{
    if (game_hooks.unported)
        game_hooks.unported(address);
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* The program image as TOS would leave it: relocated code, basepage, and the stubs the original's vectors point at. */
int game_load(const char *dir)
{
    snprintf(data_dir, sizeof data_dir, "%s", dir);
    char path[600];
    snprintf(path, sizeof path, "%s/GARFIELD.PRG", data_dir);
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *prg = malloc(len), *ram = calloc(1, ST_RAM_SIZE);
    if (fread(prg, 1, len, f) != (size_t)len || prg[0] != 0x60 || prg[1] != 0x1A) { fclose(f); free(prg); free(ram); return -1; }
    fclose(f);
    uint32_t tsize = be32(prg + 2), dsize = be32(prg + 6), bsize = be32(prg + 10), ssize = be32(prg + 14);
    memcpy(ram + ADDR_TEXT, prg + 28, tsize + dsize);
    size_t off = 28 + tsize + dsize + ssize;
    if (off < (size_t)len) {
        uint32_t pos = be32(prg + off);
        off += 4;
        while (pos) {
            put_be32(ram + ADDR_TEXT + pos, be32(ram + ADDR_TEXT + pos) + ADDR_TEXT);
            for (;;) {
                uint8_t b = prg[off++];
                if (b == 0) { pos = 0; break; }
                if (b == 1) pos += 254;
                else { pos += b; break; }
            }
        }
    }
    uint8_t *bp = ram + BASEPAGE;  /* lowtpa, hitpa, tbase, tlen, dbase, dlen, bbase, blen */
    put_be32(bp, BASEPAGE); put_be32(bp + 4, ST_RAM_SIZE - 0x8000); put_be32(bp + 8, ADDR_TEXT);
    put_be32(bp + 12, tsize); put_be32(bp + 16, ADDR_TEXT + tsize); put_be32(bp + 20, dsize);
    put_be32(bp + 24, ADDR_TEXT + tsize + dsize); put_be32(bp + 28, bsize);
    free(prg);

    put_be32(ram + 0x70, STUB_RTE_VBL);
    ram[STUB_RTE_VBL] = 0x4E; ram[STUB_RTE_VBL + 1] = 0x73;
    ram[STUB_RTE] = 0x4E; ram[STUB_RTE + 1] = 0x73;
    ram[STUB_RTS] = 0x4E; ram[STUB_RTS + 1] = 0x75;
    static const uint8_t joy_ret[6] = { 0x4C, 0xDF, 0x7F, 0xFF, 0x4E, 0x73 };
    memcpy(ram + STUB_JOY_RET, joy_ret, sizeof joy_ret);
    for (int i = 0; i < 8; i++) put_be32(ram + KBDVECS + i * 4, STUB_RTS);
    put_be32(ram + STACK_TOP + 4, BASEPAGE);
    mem_from_st(&mem, ram);
    free(ram);
    return 0;
}

int game_run(const char *dir)
{
    if (game_load(dir) < 0)
        return -1;
    program_entry();
    return 0;
}

/* ---- the SDL host ---- */

/* Console keys; the arrow keys and Ctrl belong to the joystick and never reach the queue. */
static void push_console_key(SDL_Keycode key, SDL_Scancode sc)
{
    uint32_t st = 0, ascii = 0;
    switch (key) {
    case SDLK_RETURN: st = SCAN_RETURN; ascii = ASCII_RETURN; break;
    case SDLK_SPACE:  st = SCAN_SPACE; ascii = ASCII_SPACE; break;
    case SDLK_BACKSPACE: st = SCAN_BACKSPACE; ascii = ASCII_BACKSPACE; break;
    case SDLK_TAB:    st = SCAN_TAB; ascii = ASCII_TAB; break;
    case SDLK_F1:     st = SCAN_F1; break;
    case SDLK_F2:     st = SCAN_F2; break;
    default:
        if (key >= 'a' && key <= 'z')
            ascii = (uint32_t)key - 32;
        else if (key >= '0' && key <= '9')
            ascii = (uint32_t)key;
        else
            return;
        (void)sc;
    }
    hw_push_key(KEY_OF(st, ascii));
}

/* GARFIELD_INPUT="frame:joystick,frame:joystick,..." (joystick bits as the hardware's) replaces the keyboard, so the
 * front end can be driven without a person: it plays the way a joystick would. */
static uint8_t scripted_joystick(uint32_t frame, bool *scripted)
{
    static uint32_t at[256];
    static uint8_t value[256];
    static int count = -1;
    if (count < 0) {
        count = 0;
        const char *text = getenv("GARFIELD_INPUT");
        for (const char *p = text; p && *p && count < 256; count++) {
            char *end;
            at[count] = (uint32_t)strtoul(p, &end, 10);
            if (*end != ':')
                break;
            value[count] = (uint8_t)strtoul(end + 1, &end, 10);
            p = *end == ',' ? end + 1 : end;
        }
    }
    *scripted = count > 0;
    uint8_t joystick = 0;
    for (int i = 0; i < count && at[i] <= frame; i++)
        joystick = value[i];
    return joystick;
}

static uint8_t poll_joystick(void)
{
    bool scripted;
    uint8_t joystick = scripted_joystick(game_frames(), &scripted);
    if (scripted)
        return joystick;
    const bool *k = SDL_GetKeyboardState(NULL);
    return (k[SDL_SCANCODE_UP] ? JOY_UP : 0) | (k[SDL_SCANCODE_DOWN] ? JOY_DOWN : 0) |
           (k[SDL_SCANCODE_LEFT] ? JOY_LEFT : 0) | (k[SDL_SCANCODE_RIGHT] ? JOY_RIGHT : 0) |
           (k[SDL_SCANCODE_LCTRL] || k[SDL_SCANCODE_RCTRL] ? JOY_FIRE : 0);
}

#define SCALE 3
#define FRAME_NS (1000000000ull / 50)

static SDL_Renderer *ren;
static SDL_Texture *tex;
static uint64_t frame_start;

/* Called by the game at every vertical blank: show the screen, keep 50 Hz, take input. */
static volatile bool waiting_for_key;   /* the sound engine is stopped with the game: the chip would hold its last tone */

static void on_frame(void)
{
    static uint32_t pixels[ST_W * ST_H];
    game_render(pixels);
    SDL_UpdateTexture(tex, NULL, pixels, ST_W * 4);
    SDL_RenderClear(ren);
    SDL_RenderTexture(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);
    uint64_t dt = SDL_GetTicksNS() - frame_start;
    if (dt < FRAME_NS)
        SDL_DelayNS(FRAME_NS - dt);
#ifdef __EMSCRIPTEN__
    else
        emscripten_sleep(0);                    /* the browser draws and reads input only while the program is suspended */
#endif
    frame_start = SDL_GetTicksNS();
    for (SDL_Event e; SDL_PollEvent(&e);) {
        if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE))
            game_quit();
        else if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat)
            push_console_key(e.key.key, e.key.scancode);
    }
    hw.joy1 = poll_joystick();
    static uint16_t room_seen = 0xFFFF;
    if (getenv("GARFIELD_INPUT") && mem.vars.room != room_seen)
        SDL_Log("frame %u: room %u", game_frames(), room_seen = mem.vars.room);
}

static void start_audio(void);

int sdl_run(const char *data_dir)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO))
        return SDL_Log("SDL_Init: %s", SDL_GetError()), 1;
    SDL_Window *win;
    if (!SDL_CreateWindowAndRenderer("Garfield", ST_W * SCALE, ST_H * SCALE, SDL_WINDOW_RESIZABLE, &win, &ren))
        return SDL_Log("window: %s", SDL_GetError()), 1;
    SDL_SetRenderLogicalPresentation(ren, ST_W, ST_H, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, ST_W, ST_H);
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);

    start_audio();
    frame_start = SDL_GetTicksNS();
    game_hooks.frame = on_frame;
    int rc = game_run(data_dir);
    SDL_Quit();
    return rc < 0;
}

/* ---- what the original gets from TOS and the hardware ---- */

typedef struct { uint8_t *data; size_t size, pos; } OpenFile;

static OpenFile open_files[16];
static uint32_t heap_next = ADDR_HEAP;
static int supervisor;
static uint8_t ym_registers[16];
static uint8_t joy_sent;

uint32_t tos_malloc(uint32_t size)
{
    uint32_t limit = SCREEN - 0x1000;
    if (size == 0xFFFFFFFFu)
        return limit - heap_next;
    uint32_t address = heap_next;
    heap_next = (heap_next + size + 1) & ~1u;
    return heap_next <= limit ? address : 0;
}

uint16_t tos_open(const char *name)
{
    char path[700], clean[64];
    size_t n = 0;
    for (; name[n] && (unsigned char)name[n] >= ' ' && n < sizeof clean - 1; n++)   /* names end at a control character */
        clean[n] = (char)(name[n] >= 'a' && name[n] <= 'z' ? name[n] - 32 : name[n]);
    clean[n] = 0;
    snprintf(path, sizeof path, "%s/%s", data_dir, clean);
    FILE *f = fopen(path, "rb");
    if (!f)
        return (uint16_t)-33;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    for (int h = 6; h < 16; h++) {
        if (open_files[h].data)
            continue;
        open_files[h].data = malloc(size);
        open_files[h].size = (size_t)size;
        open_files[h].pos = 0;
        if (fread(open_files[h].data, 1, size, f) != (size_t)size) {
            fclose(f);
            return (uint16_t)-1;
        }
        fclose(f);
        return (uint16_t)h;
    }
    fclose(f);
    return (uint16_t)-35;
}

void tos_read(uint16_t handle, uint16_t *dest, uint32_t bytes)
{
    if (handle >= 16 || !open_files[handle].data)
        return;
    OpenFile *f = &open_files[handle];
    size_t n = f->size - f->pos < bytes ? f->size - f->pos : bytes;
    for (size_t i = 0; i + 1 < n; i += 2)
        dest[i / 2] = (uint16_t)(f->data[f->pos + i] << 8 | f->data[f->pos + i + 1]);
    f->pos += n;
}

uint32_t tos_raw_input(void)
{
    return hw_pop_key();
}

uint32_t tos_wait_key(void)
{
    waiting_for_key = true;
    uint32_t key;
    while (!(key = hw_pop_key()))
        on_frame();
    waiting_for_key = false;
    return key;
}

void tos_close(uint16_t handle)
{
    if (handle < 16) {
        free(open_files[handle].data);
        open_files[handle].data = NULL;
    }
}

uint32_t tos_physbase(void) { return SCREEN; }
void tos_setscreen(void) {}
void tos_enter_supervisor(void) { supervisor = 1; }

void ikbd_command(uint8_t command)
{
    if (command == 0x16)  /* joystick interrogate */
        hw.joy_request = 1;
}

void ym_write(uint8_t reg, uint8_t value) { ym_registers[reg & 15] = value; }
uint8_t ym_read(uint8_t reg) { return ym_registers[reg & 15]; }

/* ---- the sound chip ---- */

/* The YM2149 of the ST: three square waves and a noise, mixed and given a volume each. The game sets its registers
 * (ym_write) and this makes the samples from them. The chip is clocked at 2 MHz and divides by 16. */
#define YM_CLOCK 2000000.0
#define SAMPLE_RATE 44100

static float ym_level(unsigned volume)
{
    static const float levels[16] = { 0.0f, 0.0047f, 0.0067f, 0.0095f, 0.0135f, 0.0191f, 0.0270f, 0.0382f,
                                      0.0540f, 0.0763f, 0.1079f, 0.1526f, 0.2158f, 0.3052f, 0.4316f, 0.6103f };
    return levels[volume & 15];
}

static void ym_render(int16_t *out, int count)
{
    static double phase[3], noise_phase;
    static uint32_t lfsr = 1;
    static int noise_bit = 1;
    uint8_t reg[16];
    memcpy(reg, ym_registers, sizeof reg);
    double step[3];
    for (int c = 0; c < 3; c++) {
        unsigned period = (unsigned)(reg[2 * c + 1] & 0x0F) << 8 | reg[2 * c];
        step[c] = period ? YM_CLOCK / 16.0 / period / SAMPLE_RATE : 0.0;
    }
    unsigned noise_period = reg[6] & 0x1F;
    double noise_step = YM_CLOCK / 16.0 / (noise_period ? noise_period : 1) / SAMPLE_RATE;
    for (int i = 0; i < count; i++) {
        noise_phase += noise_step;
        while (noise_phase >= 1.0) {
            noise_phase -= 1.0;
            noise_bit = (int)(lfsr & 1);
            lfsr = (lfsr >> 1) | (((lfsr ^ (lfsr >> 3)) & 1) << 16);
        }
        float mix = 0.0f;
        for (int c = 0; c < 3; c++) {
            phase[c] += step[c];
            if (phase[c] >= 1.0)
                phase[c] -= (int)phase[c];
            int tone = step[c] == 0.0 || phase[c] < 0.5;
            int tone_on = !(reg[7] >> c & 1), noise_on = !(reg[7] >> (c + 3) & 1);
            int bit = (tone || !tone_on) && (noise_bit || !noise_on);
            mix += ym_level(reg[8 + c]) * (bit ? 1.0f : -1.0f);
        }
        out[i] = (int16_t)(mix * 0.33f * 32767.0f);
    }
}

static void SDLCALL audio_callback(void *user, SDL_AudioStream *stream, int additional, int total)
{
    (void)user; (void)total;
    int16_t samples[1024];
    while (additional > 0) {
        int count = additional / 2 < 1024 ? additional / 2 : 1024;
        if (count <= 0)
            break;
        ym_render(samples, count);
        if (waiting_for_key || getenv("GARFIELD_MUTE"))
            memset(samples, 0, count * 2);
        SDL_PutAudioStreamData(stream, samples, count * 2);
        additional -= count * 2;
    }
}

static void start_audio(void)
{
    SDL_AudioSpec spec = { SDL_AUDIO_S16, 1, SAMPLE_RATE };
    SDL_AudioStream *stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_callback, NULL);
    if (!stream) {
        SDL_Log("no sound: %s", SDL_GetError());
        return;
    }
    SDL_ResumeAudioStreamDevice(stream);
}
uint8_t tos_mfp_gpip(void) { return 0; }
uint8_t tos_video_sync(void) { return 0; }

/* The original takes its interrupts where it waits for them. One arrival takes one interrupt: the keyboard
 * chip's joystick packet when there is news, else the vertical blank. */
int sync_arrive(int id)
{
    if (game_hooks.sync)
        game_hooks.sync(id);
    if (!supervisor)
        return 0;
    uint32_t joy_handler = mem.sys.kbd_vectors[6];
    if ((hw.joy_request || hw.joy1 != joy_sent) && joy_handler == ADDR_TEXT + 0x6FE) {
        hw.joy_request = 0;
        joy_sent = hw.joy1;
        mem.sys.joy_packet[0] = 0xFD;
        mem.sys.joy_packet[1] = 0;
        mem.sys.joy_packet[2] = joy_sent;
        ikbd_joystick_event(mem.sys.joy_packet);
        return 1;
    }
    if (mem.sys.vbl_vector == ADDR_TEXT + 0x680) {
        frames++;
        mem.sys.frclock++;
        vbl_handler();
        if (game_hooks.frame)
            game_hooks.frame();
    }
    return 0;
}
