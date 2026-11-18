#include "emu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unicorn/unicorn.h>
#include "disk.h"

#define RAM_SIZE   0x100000u
#define IO_BASE    0xFF8000u
#define IO_SIZE    0x8000u
#define BASEPAGE   0x10000u
#define SCREEN     (RAM_SIZE - 0x8000u)
#define STACK_TOP  (SCREEN - 0x100u)
#define KBDVECS    0x600u
#define JOYVEC     (KBDVECS + 0x18u)
#define JOY_PACKET 0x640u
#define STUB_RTE_VBL 0x500u  /* stands in for TOS's own VBL handler */
#define STUB_RTE     0x502u
#define STUB_RTS     0x504u
#define STUB_JOY_RET 0x506u  /* movem.l (a7)+,d0-d7/a0-a6 ; rte: TOS saves every register around joyvec */
#define TEXT_BASE  (BASEPAGE + 0x100u)

#define PROTECTION_CHECK (TEXT_BASE + 0x4A4E) /* FDC-based copy protection; returns d0 = 0 when it passes */
static const uint32_t vsync_waits[] = { TEXT_BASE + 0x79A, TEXT_BASE + 0x71F4, TEXT_BASE + 0x2B4 };  /* busy-waits and the main loop top */

typedef struct { uint8_t *data; size_t size, pos; } File;

struct Emu {
    uc_engine *uc;
    StxImage *image;
    Disk *disk;
    uint32_t entry, heap, logbase, physbase;
    File handles[16];
    uint32_t frames;
    int halted, vsync_pending, skip_wait, joy_request;
    uint8_t joy1, joy_sent;
    uint32_t keys[16];
    unsigned key_head, key_tail;
    int verbose;
    uint8_t *ram, *io;  /* host backing; mapped at every 16 MB alias to mimic the 68000's 24-bit bus */
    uc_hook watch_hook, trace_hook;
    int watch_active, trace_active;
};

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t rd16(const uint8_t *p) { return p[0] << 8 | p[1]; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void wr16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }

static uint32_t reg(Emu *e, int r) { uint32_t v = 0; uc_reg_read(e->uc, r, &v); return v; }
static void setreg(Emu *e, int r, uint32_t v) { uc_reg_write(e->uc, r, &v); }

void emu_read(Emu *e, uint32_t addr, void *buf, size_t n) { uc_mem_read(e->uc, addr, buf, n); }
void emu_write(Emu *e, uint32_t addr, const void *buf, size_t n) { uc_mem_write(e->uc, addr, buf, n); }
uint32_t emu_pc(Emu *e) { return reg(e, UC_M68K_REG_PC); }
uint32_t emu_frames(const Emu *e) { return e->frames; }
void emu_set_joy1(Emu *e, uint8_t s) { e->joy1 = s; }

static uint32_t mem32(Emu *e, uint32_t a) { uint8_t b[4]; emu_read(e, a, b, 4); return rd32(b); }
static void put32(Emu *e, uint32_t a, uint32_t v) { uint8_t b[4]; wr32(b, v); emu_write(e, a, b, 4); }

static uint32_t arg32(Emu *e, uint32_t off) { return mem32(e, reg(e, UC_M68K_REG_A7) + off); }
static uint16_t arg16(Emu *e, uint32_t off)
{
    uint8_t b[2];
    emu_read(e, reg(e, UC_M68K_REG_A7) + off, b, 2);
    return rd16(b);
}

static int load_prg(Emu *e, const uint8_t *prg, size_t len)
{
    if (rd16(prg) != 0x601A)
        return -1;
    uint32_t tsize = rd32(prg + 2), dsize = rd32(prg + 6), ssize = rd32(prg + 14);
    size_t body = tsize + dsize;
    uint8_t *img = malloc(body);
    memcpy(img, prg + 28, body);
    size_t off = 28 + body + ssize;
    if (off < len) {
        uint32_t pos = rd32(prg + off);
        off += 4;
        while (pos) {
            wr32(img + pos, rd32(img + pos) + TEXT_BASE);
            for (;;) {
                uint8_t b = prg[off++];
                if (b == 0) { pos = 0; break; }
                if (b == 1) pos += 254;
                else { pos += b; break; }
            }
        }
    }
    emu_write(e, TEXT_BASE, img, body);
    free(img);
    e->entry = TEXT_BASE;
    e->heap = (TEXT_BASE + (uint32_t)len + 0x10000 + 0xFF) & ~0xFFu;
    uint8_t bp[32] = {0};  /* GEMDOS basepage: lowtpa, hitpa, tbase, tlen, dbase, dlen, bbase, blen */
    uint32_t dsz = rd32(prg + 6), bsz = rd32(prg + 10);
    wr32(bp, BASEPAGE); wr32(bp + 4, RAM_SIZE - 0x8000); wr32(bp + 8, TEXT_BASE);
    wr32(bp + 12, tsize); wr32(bp + 16, TEXT_BASE + tsize); wr32(bp + 20, dsz);
    wr32(bp + 24, TEXT_BASE + tsize + dsz); wr32(bp + 28, bsz);
    emu_write(e, BASEPAGE, bp, sizeof bp);
    return 0;
}

static void read_cstr(Emu *e, uint32_t addr, char *out, size_t max)
{
    size_t i = 0;
    for (; i + 1 < max; i++) {
        uint8_t c;
        emu_read(e, addr + i, &c, 1);
        if (c < ' ') break;  /* GEMDOS ends names at control characters */
        out[i] = c;
    }
    out[i] = 0;
}

static uint32_t trap_gemdos(Emu *e, unsigned func)
{
    switch (func) {
    case 0x4A: return 0;
    case 0x20: {
        uint32_t sr = reg(e, UC_M68K_REG_SR);
        if (arg32(e, 2) == 0 && !(sr & 0x2000)) {
            uint32_t sp = reg(e, UC_M68K_REG_A7);
            setreg(e, UC_M68K_REG_SR, sr | 0x2000);
            setreg(e, UC_M68K_REG_A7, sp);
            return sp;
        }
        setreg(e, UC_M68K_REG_SR, sr & ~0x2000u);
        return 0;
    }
    case 0x48: case 0x44: {
        uint32_t size = arg32(e, 2), limit = SCREEN - 0x1000;
        if (size == 0xFFFFFFFFu) return limit - e->heap;
        uint32_t addr = e->heap;
        e->heap = (e->heap + size + 1) & ~1u;
        return e->heap <= limit ? addr : 0;
    }
    case 0x49: case 0x19: case 0x3B: case 0x1A: case 0x09: return 0;
    case 0x02: return 0;
    case 0x0B: return e->key_head != e->key_tail ? 0xFFFFFFFFu : 0;
    case 0x01: case 0x08: case 0x06: {
        if (func == 0x06 && arg16(e, 2) != 0xFF)
            return 0;  /* raw output */
        if (e->key_head == e->key_tail)
            return 0;
        return e->keys[e->key_tail++ % 16];
    }
    case 0x0E: return 1;
    case 0x3D: {
        char raw[256], name[64];
        read_cstr(e, arg32(e, 2), raw, sizeof raw);
        const char *base = raw;
        for (const char *p = raw; *p; p++)
            if (*p == '\\' || *p == '/') base = p + 1;
        size_t n = 0;
        for (; base[n] && n < sizeof name - 1; n++) name[n] = (char)((base[n] >= 'a' && base[n] <= 'z') ? base[n] - 32 : base[n]);
        name[n] = 0;
        size_t sz;
        uint8_t *data = disk_read_file(e->disk, name, &sz);
        if (e->verbose) fprintf(stderr, "  Fopen '%s' exists=%d\n", name, data != NULL);
        if (!data) return (uint32_t)-33;
        for (int h = 6; h < 16; h++) {
            if (e->handles[h].data) continue;
            e->handles[h] = (File){ data, sz, 0 };
            return (uint32_t)h;
        }
        free(data);
        return (uint32_t)-35;
    }
    case 0x3E: {
        unsigned h = arg16(e, 2);
        if (h < 16) { free(e->handles[h].data); e->handles[h].data = NULL; }
        return 0;
    }
    case 0x3F: {
        unsigned h = arg16(e, 2);
        uint32_t count = arg32(e, 4), buf = arg32(e, 8);
        if (h >= 16 || !e->handles[h].data) return (uint32_t)-37;
        File *f = &e->handles[h];
        size_t n = f->size - f->pos < count ? f->size - f->pos : count;
        emu_write(e, buf, f->data + f->pos, n);
        f->pos += n;
        return (uint32_t)n;
    }
    case 0x42: {
        int32_t off = (int32_t)arg32(e, 2);
        unsigned h = arg16(e, 6), mode = arg16(e, 8);
        if (h >= 16 || !e->handles[h].data) return (uint32_t)-37;
        File *f = &e->handles[h];
        f->pos = (mode == 0 ? 0 : mode == 1 ? f->pos : f->size) + off;
        return (uint32_t)f->pos;
    }
    case 0x00: case 0x4C:
        e->halted = 1;
        uc_emu_stop(e->uc);
        return 0;
    }
    fprintf(stderr, "unhandled GEMDOS %#x\n", func);
    e->halted = 1;
    uc_emu_stop(e->uc);
    return 0;
}

static uint32_t trap_xbios(Emu *e, unsigned func)
{
    switch (func) {
    case 5: {
        uint32_t log = arg32(e, 2), phys = arg32(e, 6);
        if (log != 0xFFFFFFFFu) e->logbase = log;
        if (phys != 0xFFFFFFFFu) e->physbase = phys;
        return 0;
    }
    case 2: return e->physbase;
    case 3: return e->logbase;
    case 4: case 6: case 37: return 0;
    case 34: return KBDVECS;
    }
    fprintf(stderr, "unhandled XBIOS %u\n", func);
    e->halted = 1;
    uc_emu_stop(e->uc);
    return 0;
}

static void on_intr(uc_engine *uc, uint32_t intno, void *user)
{
    Emu *e = user;
    uint32_t pc = reg(e, UC_M68K_REG_PC);
    if (intno == 33 || intno == 46 || intno == 45) {
        unsigned func = arg16(e, 0);
        if (getenv("TRAPLOG")) fprintf(stderr, "trap %u func %#x a7=%08x sr=%04x pc=%06x d0=%08x a6=%08x\n", intno, func, reg(e, UC_M68K_REG_A7), reg(e, UC_M68K_REG_SR), pc, reg(e, UC_M68K_REG_D0), reg(e, UC_M68K_REG_A6));
        uint32_t ret = intno == 33 ? trap_gemdos(e, func) : intno == 46 ? trap_xbios(e, func) : 0;
        setreg(e, UC_M68K_REG_D0, ret);
        setreg(e, UC_M68K_REG_PC, pc + 2);
        return;
    }
    if (intno == 256) { /* RTE */
        uint32_t sp = reg(e, UC_M68K_REG_A7);
        uint8_t fr[6];
        emu_read(e, sp, fr, 6);
        setreg(e, UC_M68K_REG_A7, sp + 6);
        setreg(e, UC_M68K_REG_SR, rd16(fr));
        setreg(e, UC_M68K_REG_PC, rd32(fr + 2));
        return;
    }
    fprintf(stderr, "INTR %u pc=%06x\n", intno, pc);
    e->halted = 1;
    uc_emu_stop(uc);
}

static void on_io(uc_engine *uc, uc_mem_type type, uint64_t addr, int size, int64_t value, void *user)
{
    Emu *e = user;
    (void)uc; (void)size;
    if (type == UC_MEM_WRITE && (addr & 0xFFFFFF) == 0xFFFC02 && (value & 0xFF) == 0x16)
        e->joy_request = 1;
}

static bool on_unmapped(uc_engine *uc, uc_mem_type type, uint64_t addr, int size, int64_t value, void *user)
{
    Emu *e = user;
    (void)uc; (void)value;
    fprintf(stderr, "UNMAPPED type=%d addr=%08llx size=%d pc=%06x\n", type, (unsigned long long)addr, size, reg(e, UC_M68K_REG_PC));
    return false;
}

static void on_protection(uc_engine *uc, uint64_t addr, uint32_t size, void *user)
{
    Emu *e = user;
    (void)uc; (void)addr; (void)size;
    uint32_t sp = reg(e, UC_M68K_REG_A7), ret = mem32(e, sp);
    setreg(e, UC_M68K_REG_D0, 0);
    setreg(e, UC_M68K_REG_A7, sp + 4);
    setreg(e, UC_M68K_REG_PC, ret);
}

static void on_wait(uc_engine *uc, uint64_t addr, uint32_t size, void *user)
{
    Emu *e = user;
    (void)addr; (void)size;
    if (e->skip_wait) { e->skip_wait = 0; return; }
    e->vsync_pending = 1;
    uc_emu_stop(uc);
}

Emu *emu_new(const char *stx_path)
{
    StxImage *image = stx_load(stx_path);
    Disk *disk = image ? disk_open(image) : NULL;
    if (!disk) { fprintf(stderr, "cannot read disk image %s\n", stx_path); return NULL; }
    size_t len;
    uint8_t *prg = disk_read_file(disk, "AUTO/GARFIELD.PRG", &len);
    if (!prg) { fprintf(stderr, "no AUTO/GARFIELD.PRG on %s\n", stx_path); return NULL; }

    Emu *e = calloc(1, sizeof *e);
    e->image = image;
    e->disk = disk;
    uc_open(UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, &e->uc);
    uc_ctl_set_cpu_model(e->uc, UC_CPU_M68K_M68000);
    e->ram = calloc(1, RAM_SIZE);
    e->io = calloc(1, IO_SIZE);
    for (uint64_t hi = 0; hi < 256; hi++) {
        uc_mem_map_ptr(e->uc, hi << 24, RAM_SIZE, UC_PROT_ALL, e->ram);
        uc_mem_map_ptr(e->uc, (hi << 24) | IO_BASE, IO_SIZE, UC_PROT_ALL, e->io);
    }
    e->logbase = e->physbase = SCREEN;
    if (load_prg(e, prg, len) < 0) { free(prg); return NULL; }
    free(prg);

    uint8_t two[2];
    wr16(two, 0x4E73); emu_write(e, STUB_RTE_VBL, two, 2); emu_write(e, STUB_RTE, two, 2);
    wr16(two, 0x4E75); emu_write(e, STUB_RTS, two, 2);
    static const uint8_t joy_ret[6] = { 0x4C, 0xDF, 0x7F, 0xFF, 0x4E, 0x73 };
    emu_write(e, STUB_JOY_RET, joy_ret, sizeof joy_ret);
    put32(e, 0x70, STUB_RTE_VBL);
    for (int i = 0; i < 8; i++) put32(e, KBDVECS + i * 4, STUB_RTS);
    put32(e, STACK_TOP + 4, BASEPAGE);
    setreg(e, UC_M68K_REG_SR, 0);  /* leave supervisor mode first: it swaps the stack pointer banks */
    setreg(e, UC_M68K_REG_A7, STACK_TOP);
    setreg(e, UC_M68K_REG_PC, e->entry);

    uc_hook h;
    uc_hook_add(e->uc, &h, UC_HOOK_INTR, on_intr, e, 1, 0);
    for (uint64_t hi = 0; hi < 256; hi++)
        uc_hook_add(e->uc, &h, UC_HOOK_MEM_WRITE, on_io, e, (hi << 24) | IO_BASE, (hi << 24) | (IO_BASE + IO_SIZE - 1));
    uc_hook_add(e->uc, &h, UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED | UC_HOOK_MEM_FETCH_UNMAPPED,
                on_unmapped, e, 1, 0);
    uc_hook_add(e->uc, &h, UC_HOOK_CODE, on_protection, e, PROTECTION_CHECK, PROTECTION_CHECK);
    for (size_t i = 0; i < sizeof vsync_waits / sizeof *vsync_waits; i++)
        uc_hook_add(e->uc, &h, UC_HOOK_CODE, on_wait, e, vsync_waits[i], vsync_waits[i]);
    return e;
}

void emu_free(Emu *e)
{
    for (int i = 0; i < 16; i++) free(e->handles[i].data);
    uc_close(e->uc);
    disk_close(e->disk);
    stx_free(e->image);
    free(e->ram);
    free(e->io);
    free(e);
}

int emu_run_to_vsync(Emu *e)
{
    for (int chunks = 0; !e->halted && !e->vsync_pending; chunks++) {
        if (chunks >= 3 && chunks < 8)
            fprintf(stderr, "  chunk %d pc=text+%05x sr=%04x a7=%08x queue count=%04x base=%08x\n", chunks, reg(e, UC_M68K_REG_PC) - TEXT_BASE, reg(e, UC_M68K_REG_SR), reg(e, UC_M68K_REG_A7), mem32(e, 0x440f4 + 0x15a) >> 16, mem32(e, 0x440f4 + 0x14a));
        if (chunks == 20) {
            fprintf(stderr, "no vsync point for 40M instructions, spinning at pc=text+%05x\n", reg(e, UC_M68K_REG_PC) - TEXT_BASE);
            e->halted = 1;
            break;
        }
        uc_err err = uc_emu_start(e->uc, reg(e, UC_M68K_REG_PC), 0, 0, 2000000);
        if (err != UC_ERR_OK) {
            fprintf(stderr, "uc error %s pc=%06x\n", uc_strerror(err), reg(e, UC_M68K_REG_PC));
            e->halted = 1;
        }
    }
    return e->halted ? -1 : 0;
}

static int joy_interrupt(Emu *e)
{
    uint32_t sr = reg(e, UC_M68K_REG_SR), handler = mem32(e, JOYVEC);
    if (!(e->joy_request || e->joy1 != e->joy_sent) || !(sr & 0x2000) || ((sr >> 8) & 7) >= 6 || handler == STUB_RTS)
        return 0;
    e->joy_request = 0;
    e->joy_sent = e->joy1;
    uint8_t pkt[3] = { 0xFD, 0, e->joy1 };
    emu_write(e, JOY_PACKET, pkt, 3);
    uint32_t sp = reg(e, UC_M68K_REG_A7) - 6 - 15 * 4 - 4;
    uint8_t fr[6 + 15 * 4 + 4];
    uint8_t *saved = fr + 4;  /* ascending: return stub, d0-d7, a0-a6, then the rte frame */
    wr32(fr, STUB_JOY_RET);
    for (int i = 0; i < 8; i++) wr32(saved + i * 4, reg(e, UC_M68K_REG_D0 + i));
    for (int i = 0; i < 7; i++) wr32(saved + 32 + i * 4, reg(e, UC_M68K_REG_A0 + i));
    wr16(saved + 60, (uint16_t)sr); wr32(saved + 62, reg(e, UC_M68K_REG_PC));
    emu_write(e, sp, fr, sizeof fr);
    setreg(e, UC_M68K_REG_A7, sp);
    setreg(e, UC_M68K_REG_A0, JOY_PACKET);
    setreg(e, UC_M68K_REG_SR, (sr & ~0x700u) | 0x2600);
    setreg(e, UC_M68K_REG_PC, handler);
    return 1;
}

static void vbl(Emu *e)
{
    uint32_t sr = reg(e, UC_M68K_REG_SR), handler = mem32(e, 0x70);
    if (!(sr & 0x2000) || ((sr >> 8) & 7) >= 4 || !handler)
        return;
    uint32_t sp = reg(e, UC_M68K_REG_A7) - 6;
    uint8_t fr[6];
    wr16(fr, (uint16_t)sr); wr32(fr + 2, reg(e, UC_M68K_REG_PC));
    emu_write(e, sp, fr, 6);
    setreg(e, UC_M68K_REG_A7, sp);
    setreg(e, UC_M68K_REG_SR, (sr & ~0x700u) | 0x2400);
    setreg(e, UC_M68K_REG_PC, handler);
    e->frames++;
    e->skip_wait = 1;
    put32(e, 0x466, mem32(e, 0x466) + 1);
}

void emu_vsync(Emu *e)
{
    e->vsync_pending = 0;
    if (!joy_interrupt(e))
        vbl(e);
}

void emu_get_regs(Emu *e, uint32_t d[8], uint32_t a[8])
{
    for (int i = 0; i < 8; i++) {
        d[i] = reg(e, UC_M68K_REG_D0 + i);
        a[i] = reg(e, UC_M68K_REG_A0 + i);
    }
}

void emu_set_regs(Emu *e, const uint32_t d[8], const uint32_t a[8])
{
    for (int i = 0; i < 8; i++) {
        setreg(e, UC_M68K_REG_D0 + i, d[i]);
        setreg(e, UC_M68K_REG_A0 + i, a[i]);
    }
}

typedef struct { Emu *e; EmuRoutineFn fn; void *user; } Replacement;

static void on_replace(uc_engine *uc, uint64_t addr, uint32_t size, void *user)
{
    Replacement *r = user;
    (void)uc; (void)addr; (void)size;
    if (!r->fn(r->e, r->user))
        return;
    uint32_t sp = reg(r->e, UC_M68K_REG_A7);
    setreg(r->e, UC_M68K_REG_PC, mem32(r->e, sp));
    setreg(r->e, UC_M68K_REG_A7, sp + 4);
}

void emu_replace(Emu *e, uint32_t addr, EmuRoutineFn fn, void *user)
{
    Replacement *r = malloc(sizeof *r);
    *r = (Replacement){ e, fn, user };
    uc_hook h;
    uc_hook_add(e->uc, &h, UC_HOOK_CODE, on_replace, r, addr, addr);
}

struct EmuState {
    uc_context *ctx;
    uint8_t *ram;
    uint32_t heap, logbase, physbase, frames;
    int halted, vsync_pending, skip_wait, joy_request;
    uint8_t joy1, joy_sent;
    uint8_t palette[32];                 /* the colour registers at $FF8240 */
    File handles[16];
};

EmuState *emu_save(Emu *e)
{
    EmuState *s = calloc(1, sizeof *s);
    uc_context_alloc(e->uc, &s->ctx);
    uc_context_save(e->uc, s->ctx);
    s->ram = malloc(RAM_SIZE);
    emu_read(e, 0, s->ram, RAM_SIZE);
    emu_read(e, 0xFF8240, s->palette, sizeof s->palette);
    s->heap = e->heap; s->logbase = e->logbase; s->physbase = e->physbase; s->frames = e->frames;
    s->halted = e->halted; s->vsync_pending = e->vsync_pending; s->skip_wait = e->skip_wait;
    s->joy_request = e->joy_request; s->joy1 = e->joy1; s->joy_sent = e->joy_sent;
    for (int i = 0; i < 16; i++) {
        s->handles[i] = e->handles[i];
        if (e->handles[i].data) {
            s->handles[i].data = malloc(e->handles[i].size);
            memcpy(s->handles[i].data, e->handles[i].data, e->handles[i].size);
        }
    }
    return s;
}

void emu_restore(Emu *e, const EmuState *s)
{
    uc_context_restore(e->uc, s->ctx);
    emu_write(e, 0, s->ram, RAM_SIZE);
    emu_write(e, 0xFF8240, s->palette, sizeof s->palette);
    e->heap = s->heap; e->logbase = s->logbase; e->physbase = s->physbase; e->frames = s->frames;
    e->halted = s->halted; e->vsync_pending = s->vsync_pending; e->skip_wait = s->skip_wait;
    e->joy_request = s->joy_request; e->joy1 = s->joy1; e->joy_sent = s->joy_sent;
    for (int i = 0; i < 16; i++) {
        free(e->handles[i].data);
        e->handles[i] = s->handles[i];
        if (s->handles[i].data) {
            e->handles[i].data = malloc(s->handles[i].size);
            memcpy(e->handles[i].data, s->handles[i].data, s->handles[i].size);
        }
    }
}

void emu_state_free(EmuState *s)
{
    if (!s) return;
    uc_context_free(s->ctx);
    for (int i = 0; i < 16; i++) free(s->handles[i].data);
    free(s->ram);
    free(s);
}

typedef struct { Emu *e; EmuWatchFn fn; void *user; } Watch;
static Watch watch_slot;

static void on_watch(uc_engine *uc, uc_mem_type type, uint64_t addr, int size, int64_t value, void *user)
{
    Watch *w = user;
    (void)uc; (void)type;
    w->fn(w->e, reg(w->e, UC_M68K_REG_PC), (uint32_t)addr, size, (uint32_t)value, w->user);
}

void emu_watch_write(Emu *e, uint32_t lo, uint32_t hi, EmuWatchFn fn, void *user)
{
    emu_watch_clear(e);
    watch_slot = (Watch){ e, fn, user };
    uc_hook_add(e->uc, &e->watch_hook, UC_HOOK_MEM_WRITE, on_watch, &watch_slot, lo, hi);
    uc_ctl_flush_tb(e->uc);  /* blocks translated before the hook would not report their writes */
    e->watch_active = 1;
}

void emu_watch_clear(Emu *e)
{
    if (e->watch_active)
        uc_hook_del(e->uc, e->watch_hook);
    e->watch_active = 0;
}

int emu_backtrace(Emu *e, uint32_t *returns, uint32_t *targets, int max)
{
    uint32_t sp = reg(e, UC_M68K_REG_A7), text_end = TEXT_BASE + 0x40000;
    int n = 0;
    for (uint32_t off = 0; off < 0x200 && n < max; off += 2) {
        uint32_t ret = mem32(e, sp + off);
        if (ret < TEXT_BASE || ret >= text_end || (ret & 1))
            continue;
        uint8_t code[6];
        emu_read(e, ret - 6, code, 6);
        uint32_t target = 0;
        if (rd16(code) == 0x4EB9)
            target = rd32(code + 2);
        else if (rd16(code + 2) == 0x6100)
            target = ret - 4 + 2 + (int16_t)rd16(code + 4);
        else if (code[4] == 0x61 && code[5] != 0 && code[5] != 0xFF)
            target = ret - 2 + 2 + (int8_t)code[5];
        else
            continue;
        returns[n] = ret;
        targets[n++] = target;
    }
    return n;
}

typedef struct { EmuTraceFn fn; void *user; } TraceCtx;
static TraceCtx trace_slot;

static void on_trace(uc_engine *uc, uint64_t addr, uint32_t size, void *user)
{
    TraceCtx *t = user;
    (void)uc; (void)size;
    t->fn((uint32_t)addr, t->user);
}

void emu_trace(Emu *e, EmuTraceFn fn, void *user)
{
    emu_trace_clear(e);
    trace_slot = (TraceCtx){ fn, user };
    uc_hook_add(e->uc, &e->trace_hook, UC_HOOK_CODE, on_trace, &trace_slot, 1, 0);
    uc_ctl_flush_tb(e->uc);
    e->trace_active = 1;
}

void emu_trace_clear(Emu *e)
{
    if (e->trace_active)
        uc_hook_del(e->uc, e->trace_hook);
    e->trace_active = 0;
}

void emu_push_key(Emu *e, uint32_t key)
{
    if (e->key_head - e->key_tail < 16)
        e->keys[e->key_head++ % 16] = key;
}

uint32_t emu_get_sr(Emu *e) { return reg(e, UC_M68K_REG_SR); }
void emu_set_sr(Emu *e, uint32_t sr) { setreg(e, UC_M68K_REG_SR, sr); }

uint32_t emu_physbase(Emu *e) { return e->physbase; }
