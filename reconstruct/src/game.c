#include <string.h>
#include "game.h"

/* The buffers the program keeps its addresses of in variables never move; the port uses them directly. */
#define SCREEN_AT(offset) (&mem.screen.planes[(offset) / 2])             /* the screen, by byte offset */
#define BACK_AT(offset) (&mem.heap.back_buf[(offset) / 2])               /* the back buffer, by byte offset */
#define FONT (mem.text.graphics)                                         /* $8874 */
#define SPRITES (mem.text.graphics + 0x2D80 / 2)                         /* the sprites follow the font */
/* Where the next saved background goes: the two lists of them are allocated one after the other, and the cursor moves on
 * through them. */
#define BACKGROUNDS_HERE (mem.heap.backgrounds_a + (mem.vars.backgrounds_now - mem.vars.backgrounds_a) / 2)
#define SPRITE_AT(number) (SPRITES + (uint32_t)(number) * 0x200 / 2)     /* one of 32 rows, 512 bytes */



StMem mem;

/* ---- the routines of the original ---- */

/* $06FE: the keyboard chip reports the joystick: three bytes (header, joystick 0, joystick 1). */
void ikbd_joystick_event(const uint8_t *packet)
{
    mem.text.joy_hdr = packet[0];
    mem.text.joy_state = packet[2];
}

/* $0716: joystick bits (up, down, left, right in the low four, fire in bit 7) to the direction code in
 * joy_dir. While progress_flags bit 6 is set the player is led to the left edge instead. Returns the code. */
uint8_t joystick_direction(void)
{
    uint8_t dir;
    if (mem.vars.progress_flags & 0x40) {
        dir = 4;
        if (mem.text.entities[PLAYER].x < 0x40) {
            mem.vars.progress_flags &= ~0x40;
            dir = 5;
        }
        mem.vars.joy_dir = dir;
        mem.vars.joy_idle = 0;
        return dir;
    }
    uint8_t s = mem.text.joy_state;
    dir = mem.text.joy_dir_table[(s & 0xF) | ((s >> 3) & 0x10)];
    mem.vars.joy_dir = dir;
    if (dir) {
        mem.vars.joy_idle = 0;
    } else if (++mem.vars.joy_idle == 0x80) {
        mem.vars.joy_idle = 0;
        if (!(mem.vars.mode_flags & 8)) {
            post_message(0x2E);
            start_bubble(9, 0x28);
        }
    }
    return dir;
}

/* $6310: every queued entry is a sprite of 32 rows for the back buffer, drawn last in first out. The planes of
 * the back buffer are 0x1E00 bytes apart and its rows 48 bytes (24 words); a masked sprite brings 6 bytes
 * a plane a row, the others 4. The entries are at queue_base from the variables ($16E, or $25E for the second
 * list). */
void draw_sprite_queue(void)
{
    const struct DrawEntry *queue = (const struct DrawEntry *)((const uint8_t *)&mem.vars + mem.vars.queue_base);
    while (mem.vars.queue_count) {
        const struct DrawEntry *e = &queue[--mem.vars.queue_count];
        uint16_t *dest = ST_PTR(uint16_t, e->dest);
        const uint16_t *src = ST_PTR(const uint16_t, e->source);
        int masked = e->dest >> 31;
        for (int row = 0; row < 32; row++, dest += 24) {
            for (int plane = 0; plane < 4; plane++) {
                uint16_t *d = dest + plane * (0x1E00 / 2);
                d[0] = *src++;
                d[1] = *src++;
                if (masked)
                    d[2] = *src++;
            }
        }
    }
}

/* $62A4: shifts the 128 rows of three words (a sprite's four planes of 32 rows) right by `bits`, 0 to 7, as
 * one 48-bit value, with the bit shifted out of a row coming in at the left of the next (through the carry). */
unsigned shift_sprite_right(uint16_t *rows, unsigned bits, unsigned carry)
{
    for (int row = 0; row < 128; row++, rows += 3) {
        for (unsigned i = 0; i < bits; i++) {
            unsigned out = rows[2] & 1;
            rows[2] = (uint16_t)(rows[2] >> 1 | (rows[1] & 1) << 15);
            rows[1] = (uint16_t)(rows[1] >> 1 | (rows[0] & 1) << 15);
            rows[0] = (uint16_t)(rows[0] >> 1 | carry << 15);
            carry = out;
        }
    }
    return carry;
}

/* $6E7C: the back buffer is four planes of rows of 24 words, of which the screen shows 20 from the second
 * word on; the screen wants them interleaved, plane by plane, a group of four words for 16 pixels. */
void present_back_buffer(void)
{
    uint16_t *screen = SCREEN_AT(0);
    const uint16_t *plane[4];
    for (int p = 0; p < 4; p++)
        plane[p] = BACK_AT(0x600) + p * (0x1E00 / 2);
    for (int row = 0; row < 128; row++) {
        for (int p = 0; p < 4; p++)
            plane[p] += row == 0 ? 2 : 4;
        for (int group = 0; group < 20; group++)
            for (int p = 0; p < 4; p++)
                *screen++ = *plane[p]++;
    }
}

/* ---- start-up ---- */

/* The address of an element of a table in the code segment, as the original keeps it in its variables. */
#define TEXT_ADDRESS(offset) (ADDR_TEXT + (offset))

/* $07E0: the colours go to the palette registers; the hardware has three bits a colour. */
void set_palette(const uint16_t colours[16])
{
    for (int i = 0; i < 16; i++)
        hardware_palette[i] = colours[i] & 0x0777;
}

/* $0826 */
void set_black_palette(void)
{
    set_palette(mem.text.palettes[2]);
}

/* $082C: what TOS had in the palette registers is kept to be put back. */
void save_system_palette(void)
{
    for (int i = 0; i < 16; i++)
        mem.text.palettes[1][i] = hardware_palette[i];
}

/* $6FFC: for every byte its bits in the opposite order, in both halves of a word. The loop counts down to
 * and including 0, so it does one more: the entry before the table gets $FFFF. */
void build_bit_reversed(void)
{
    mem.vars.bit_reversed_below = 0xFFFF;
    for (unsigned i = 0; i < 256; i++) {
        unsigned reversed = 0;
        for (int bit = 0; bit < 8; bit++)
            reversed |= (i >> bit & 1) << (7 - bit);
        mem.vars.bit_reversed[i] = (uint16_t)(reversed << 8 | reversed);
    }
}

/* $6FCA: the addresses of the records of two tables of the code segment. The first table is records of a
 * count and then six bytes for each, ended by a negative count; the second is 80 records of (word, long) pairs,
 * each ended by a negative word. */
void build_record_lists(void)
{
    const uint16_t *table = mem.text.records_510a;
    unsigned n = 0;
    while ((int16_t)*table >= 0) {
        mem.vars.picture_lists[n++] = TEXT_ADDRESS(0x510A + 2 * (table - mem.text.records_510a));
        unsigned count = *table;
        table += 1 + count * 3;
    }
    table = mem.text.records_3f58;
    for (int record = 0; record < 80; record++) {
        mem.vars.room_picture_lists[record] = TEXT_ADDRESS(0x3F58 + 2 * (table - mem.text.records_3f58));
        while ((int16_t)*table++ >= 0)
            table += 2;
    }
}

/* $6F36: a file into memory. */
void load_file(const char *name, uint16_t *dest, uint32_t bytes)
{
    mem.vars.file_handle = tos_open(name);
    tos_read(mem.vars.file_handle, dest, bytes);
    tos_close(mem.vars.file_handle);
}

/* $6F76: a Neochrome picture: the 128 byte header and then the 32000 bytes of the screen, both read to the
 * screen. */
void load_picture(const char *name)
{
    mem.vars.file_handle = tos_open(name);
    tos_read(mem.vars.file_handle, SCREEN_AT(0), 0x80);
    tos_read(mem.vars.file_handle, SCREEN_AT(0), 0x7D00);
    tos_close(mem.vars.file_handle);
}

/* $07A4: BACKCHRS.XYZ is the level's data: a 12 byte header whose first two longs are the sizes of the first
 * two of three parts. */
void load_level_data(void)
{
    uint16_t *file = mem.heap.level_data;
    uint32_t base = mem.vars.level_start;
    load_file(mem.text.backchrs_name, file, 0x222E0);
    mem.vars.level_start = base + 12;
    mem.vars.level_second = mem.vars.level_start + ((uint32_t)file[0] << 16 | file[1]);
    mem.vars.level_third = mem.vars.level_second + ((uint32_t)file[2] << 16 | file[3]);
}

/* $06B2: the keyboard chip's joystick handler is replaced by the game's own. */
void install_joystick_handler(void)
{
    mem.vars.kbdvecs = 0x600;                      /* XBIOS Kbdvbase: the table in low memory */
    mem.vars.old_joyvec = mem.sys.kbd_vectors[6];
    mem.sys.kbd_vectors[6] = TEXT_ADDRESS(0x6FE);
}

/* $0650: the vector the handler replaces is kept, and the jmp that ends the handler is patched with it. */
void save_vbl_vector(void)
{
    mem.vars.saved_vbl_vector = mem.sys.vbl_vector;
    mem.text.vbl_old_vector = mem.sys.vbl_vector;
}

/* $0664 */
void install_vbl_handler(void)
{
    mem.sys.vbl_vector = TEXT_ADDRESS(0x680);
}

/* $0790: waits until the vertical blank interrupt has counted once. */
void wait_vbl_tick(void)
{
    uint16_t before = mem.text.vbl_counter;
    do {
        while (sync_arrive(SYNC_TICK))
            ;
    } while (mem.text.vbl_counter == before);
}

/* $71C6: a fade starts from the colours on the screen and takes ten steps. */
void start_fade(void)
{
    for (int i = 0; i < 16; i++)
        mem.text.fade_palette[i] = hardware_palette[i];
    mem.text.fade_toggle = 0;
    mem.text.fade_steps = 9;
}

/* $71EC: waits until the interrupt has taken the fade to its end. */
void wait_fade(void)
{
    for (;;) {
        while (sync_arrive(SYNC_FADE))
            ;
        if (mem.text.fade_steps < 0)
            break;
    }
}

/* $7202: every second vertical blank a fade to black takes a step: the registers get the colours it is at,
 * and every colour loses one step of red, green and blue. */
void fade_out_step(void)
{
    if (mem.text.fade_steps < 0)
        return;
    int skip = mem.text.fade_toggle & 1;
    mem.text.fade_toggle ^= 1;
    if (skip)
        return;
    mem.text.fade_steps--;
    set_palette(mem.text.fade_palette);
    for (int i = 0; i < 16; i++) {
        uint16_t colour = mem.text.fade_palette[i], out = 0;
        if (colour & 0x007) out |= (colour & 0x007) - 0x001;
        if (colour & 0x070) out |= (colour & 0x070) - 0x010;
        if (colour & 0x700) out |= (colour & 0x700) - 0x100;
        mem.text.fade_palette[i] = out;
    }
}

/* $7120: a fade in, likewise every second blank: each colour steps up to the game's palette. A component that
 * has not come up to 0 to 7 yet shows as 0. */
void fade_in_step(void)
{
    if (mem.text.fade_in_steps < 0)
        return;
    int skip = mem.text.fade_in_toggle & 1;
    mem.text.fade_in_toggle ^= 1;
    if (skip)
        return;
    mem.text.fade_in_steps--;
    for (int i = 0; i < 16; i++) {
        uint16_t target = mem.text.palettes[0][i], current = mem.text.fade_palette[i];
        uint16_t out = 0, shown = 0;
        uint8_t blue = current & 0x00F;
        if ((target & 0x00F) != blue) {
            blue = (blue + 1) & 0x0F;
            if (!(blue & 0x08))
                shown = blue;
        }
        uint8_t green = current & 0x0F0;
        if ((target & 0x0F0) != green) {
            green = (green + 0x10) & 0xF0;
            if (!(green & 0x80))
                shown |= green;
        }
        uint16_t red = current & 0xF00;
        if ((target & 0xF00) != red) {
            red = (red + 0x100) & 0xF00;
            if (!(red & 0x800))
                shown |= red;
        }
        out = blue | green | red;
        mem.text.fade_palette[i] = out;
        mem.text.fade_output[i] = shown;
    }
    set_palette(mem.text.fade_output);
}

/* $0680: the vertical blank interrupt. The counter goes round every 25, and the sound and the fades step. It
 * ends by jumping to the vector it replaced, which only returns. */
void vbl_handler(void)
{
    if (++mem.text.vbl_counter == 0x19) {
        mem.text.vbl_counter = 0;
        mem.text.vbl_event = 0xFFFF;
    }
    sound_update();
    fade_out_step();
    fade_in_step();
}

/* ---- the game's state ---- */

/* A long stored as two words of the code segment's data. */
static uint32_t word_pair(const uint16_t *p)
{
    return (uint32_t)p[0] << 16 | p[1];
}

/* $5F46: the picture of an entity's frame is the frame'th long of its table; a table with no picture for the
 * frame gives the first. */
const uint16_t *entity_image(const struct Entity *entity, uint16_t frame)
{
    for (;;) {
        const uint16_t *image = entity->table.pictures[frame];
        if (image)
            return image;
        frame = 0;
    }
}

/* $6DA6 */
void copy_object_templates(void)
{
    for (int i = 0; i < 243; i++)
        mem.text.objects[i] = mem.text.object_templates[i];
}

/* $04F6: the player stands at the right of the room, and a second entity (a shot's) gets its picture. */
void place_player(void)
{
    struct Entity *player = &mem.text.entities[PLAYER];
    player->x = 0xA0;
    player->y = 0x78;
    player->flags = 3;
    player->table = PICTURES(mem.text.picture_tables);
    player->image = entity_image(player, 0);
    struct Entity *shot = &mem.text.entities[PLAYER + 1];
    shot->flags = 3;
    shot->image = TEXT_AT(const uint16_t, 0x848);
    mem.vars.bubble_delay = 0xFFFF;
}

/* $03C8: the state of a new game. */
void reset_game_state(void)
{
    mem.vars.backgrounds_now = mem.vars.backgrounds_a;
    mem.vars.queue_count = 0;
    copy_object_templates();
    mem.vars.player_item = 0xFFFF;
    mem.vars.first_creature_item = 0xFFFF;
    mem.vars.second_creature_item = 3;
    mem.vars.player_item = 0xFFFF;
    mem.vars.previous_room = 1;
    mem.vars.var_120 = 1;
    mem.vars.next_message = 0xFFFF;
    mem.text.vbl_event = 0xFFFF;
    memcpy(mem.text.score_text, "00000", 5);
    memcpy(mem.text.clock_text + 2, ":00:00", 6);
    mem.vars.message_progress = 0;
    mem.vars.message_number = 0;
    mem.vars.ending_countdown = 0;
    mem.vars.time_stage = 0;
    mem.vars.creature_away_ticks = 0;
    mem.vars.display_flags = 9;
    mem.vars.progress_flags = 0;
    mem.vars.event_flags = 0;
    mem.vars.mode_flags = 0;
    mem.vars.control_flags = 0;
    mem.vars.control_flags |= 0x08;
    mem.vars.fatigue = 0;
    mem.vars.hand_over_delay = 2;
    place_player();
    struct Entity *e = &mem.text.entities[5];
    e->x = 0x80;
    e->y = 0x78;
    e->frame = 0x4F;
    e->flags = 1;
    e->table = PICTURES(mem.text.picture_tables + (0x1398 - 0x12E0) / 4);
    e->image = entity_image(e, 0);
    e = &mem.text.entities[6];
    e->x = 0x40;
    e->y = 0x78;
    e->frame = 7;
    e->table = PICTURES(mem.text.picture_tables + (0x133C - 0x12E0) / 4);
    e->image = entity_image(e, 8);
    e = &mem.text.entities[0];
    e->x = 0x80;
    e->y = 0x32;
    e->frame = 0;
    e->flags = 2;
    e->field_8 = 6;
    e->image = 0;
    e->table.cycle = 0;
    mem.text.entities[0].flags &= ~0x0002;
    mem.vars.first_entity_queue_count = 0;
}

/* ---- the room ---- */

/* The data of the original is big-endian: a byte of an array of words is the high or the low half of a word. */
static uint8_t byte_of(const uint16_t *words, uint32_t offset)
{
    uint16_t word = words[offset / 2];
    return offset & 1 ? word & 0xFF : word >> 8;
}

static void set_byte_of(uint16_t *words, uint32_t offset, uint8_t value)
{
    uint16_t *word = &words[offset / 2];
    *word = offset & 1 ? (*word & 0xFF00) | value : (*word & 0x00FF) | value << 8;
}

/* $4A12: the entities of the room are marked as not drawn, but the player, the shot, and the one that belongs
 * to the room. */
void mark_entities(void)
{
    for (int i = 1; i <= 23; i++)
        mem.text.entities[i].flags &= ~0x0002;
    mem.text.entities[8].flags |= 0x0002;
    mem.text.entities[9].flags |= 0x0002;
    if (mem.text.entities[6].frame == mem.vars.room)
        mem.text.entities[6].flags |= 0x0002;
}

/* $4994: a block of the room's map is solid. The map has a bit for each, 48 to a row, 21 rows. */
static void set_block(unsigned x, unsigned y)
{
    mem.vars.block_map[(uint16_t)(y * 6 + (x >> 3))] |= 1 << (7 - (x & 7));
}

/* $494A: the solid blocks of the room are runs in a table of four a room. */
void build_block_map(void)
{
    memset(mem.vars.block_map, 0, sizeof mem.vars.block_map);
    const uint8_t *run = &mem.text.room_blocks[(uint16_t)(mem.vars.room << 4)];
    for (int n = 0; n < 4; n++, run += 4) {
        if (run[0] == 0 && run[1] == 0)
            return;
        for (unsigned i = 0; i <= run[2]; i++)
            set_block(run[0] + i, run[1]);
    }
}

/* $6984: the objects of the room (records of 16 bytes: where, what, and the room) are put in the entities from the
 * eleventh on. A record with bit 15 of its x is skipped, and one with bit 15 of its y ends the list. */
void place_room_objects(void)
{
    struct Entity *entity = &mem.text.entities[11];
    mem.vars.dropped_objects = 0;
    for (unsigned index = 0;; index++) {
        const uint16_t *object = &mem.text.objects[index * 8];
        if (!(object[0] & 0x8000) && object[3] == mem.vars.room) {
            entity->x = object[0];
            entity->y = object[1];
            entity->frame = index;
            entity->flags |= 0x0002;
            entity->field_8 = object[2];
            entity->image = 0;
            entity++;
            mem.vars.dropped_objects++;
        }
        if (object[1] & 0x8000)
            return;
    }
}

/* $486C: the next tile of the room's map goes into the back buffer, 8 pixels wide and 8 rows high. The map is
 * words: a tile number, or, with bit 15, how many more times the next tile follows. */
static void draw_map_tile(const uint16_t **map, uint32_t back_offset, const uint16_t *tiles)
{
    uint32_t tile;
    if ((int16_t)mem.vars.map_run_length < 0) {
        tile = *(*map)++;
        if ((int16_t)tile >= 0)
            goto draw;
        mem.vars.map_run_length = tile & 0x7FFF;
    }
    mem.vars.map_run_length--;
    tile = **map;
draw:
    tile <<= 5;
    for (int k = 0; k < 32; k++)
        set_byte_of(mem.heap.back_buf, back_offset + (k >> 2) * 0x30 + (k & 3) * 0x1E00, byte_of(tiles, tile + k));
}

/* $5F46's neighbour $3406: what a room needs. */
static const struct RoomRecord *room_record(uint16_t room)
{
    if (room < 80)
        return &mem.text.rooms[room];
    /* the 80th record runs on into the block table, which is raw bytes: its words are read as the original does */
    static struct RoomRecord overlapping;
    const uint8_t *bytes = (const uint8_t *)&mem.text.rooms[0] + (size_t)room * sizeof(struct RoomRecord);
    for (int i = 0; i < 10; i++)
        overlapping.w[i] = (uint16_t)(bytes[2 * i] << 8 | bytes[2 * i + 1]);
    return &overlapping;
}

/* $2288: a position is kept in the room: x from 0 to $160, y from 0 to $A0, a negative one being 0. */
void clamp_to_room(uint16_t *x, uint16_t *y)
{
    if (*x & 0x8000)
        *x = 0;
    if (*x >= 0x160)
        *x = 0x160;
    if (*y & 0x8000)
        *y = 0;
    if (*y >= 0xA0)
        *y = 0xA0;
}

/* $225C: where in the back buffer a sprite at (x, y) goes: its address, even, and how many pixels (0 to 15) it is
 * shifted. */
static uint16_t *sprite_address(uint16_t x, uint16_t y, unsigned *shift)
{
    clamp_to_room(&x, &y);
    *shift = x & 0xF;
    return BACK_AT(((x >> 3) + y * 48) & ~1u);
}

/* $637A: a sprite is kept turned the way it is drawn: one that faces left is turned once, every long of it, the
 * bits in the opposite order. */
static void turn_sprite(uint16_t number, uint16_t *sprite, int facing_left)
{
    uint8_t *bits = &mem.text.flipped_sprites[number >> 3];
    uint8_t mask = 1 << (number & 7);
    if (*bits & mask) {
        if (facing_left)
            return;
        *bits &= ~mask;
    } else {
        if (!facing_left)
            return;
        *bits |= mask;
    }
    for (int i = 0; i < 128; i++, sprite += 2) {
        uint16_t high = sprite[0], low = sprite[1];
        #define REVERSED(w) ((uint16_t)((mem.vars.bit_reversed[(w) & 0xFF] & 0xFF) << 8 | (mem.vars.bit_reversed[(w) >> 8] & 0xFF)))
        sprite[0] = REVERSED(low);
        sprite[1] = REVERSED(high);
        #undef REVERSED
    }
}

/* $6066: a sprite is drawn into the back buffer where it goes, with its mask (the pixels any plane has), and what
 * was behind it is saved in `save`, for the draw queue to put back. A sprite not shifted is 4 bytes wide a plane,
 * a shifted one is 6, and is shifted in a scratch area first. */
static void composite_sprite(uint16_t *back, uint16_t *save, const uint16_t *sprite, unsigned shift,
                             struct DrawEntry *entry)
{
    if (shift == 0) {
        mem.vars.backgrounds_now += 0x200;
        for (int row = 0; row < 32; row++, back += 24, sprite += 2) {
            uint32_t mask = ~(word_pair(sprite) | word_pair(sprite + 0x40) | word_pair(sprite + 0x80) |
                              word_pair(sprite + 0xC0));
            for (int plane = 0; plane < 4; plane++) {
                uint16_t *bg = back + plane * 0xF00;
                uint32_t pixels = word_pair(sprite + plane * 0x40);
                save[0] = bg[0];
                save[1] = bg[1];
                save += 2;
                uint32_t result = (word_pair(bg) & mask) | pixels;
                bg[0] = result >> 16;
                bg[1] = (uint16_t)result;
            }
        }
        return;
    }
    entry->dest |= 0x80000000u;
    mem.vars.backgrounds_now += 0x300;
    uint16_t *scratch = mem.vars.sprite_scratch;
    if (shift & 8) {
        /* shifted by a byte first: every four bytes of the sprite go to the middle of six */
        for (int group = 0; group < 128; group++) {
            uint32_t src = group * 4;
            scratch[group * 3 + 0] = byte_of(sprite, src);
            scratch[group * 3 + 1] = byte_of(sprite, src + 1) << 8 | byte_of(sprite, src + 2);
            scratch[group * 3 + 2] = byte_of(sprite, src + 3) << 8;
        }
        shift &= 7;
    } else {
        for (int i = 0; i < 128; i++) {
            scratch[i * 3 + 0] = sprite[i * 2 + 0];
            scratch[i * 3 + 1] = sprite[i * 2 + 1];
            scratch[i * 3 + 2] = 0;
        }
    }
    shift_sprite_right(scratch, shift, 0);
    const uint16_t *rows = scratch;
    for (int row = 0; row < 32; row++, back += 24, rows += 3) {
        uint32_t mask = ~(word_pair(rows) | word_pair(rows + 0x60) | word_pair(rows + 0xC0) | word_pair(rows + 0x120));
        uint16_t mask_word = ~(rows[2] | rows[0x62] | rows[0xC2] | rows[0x122]);
        for (int plane = 0; plane < 4; plane++) {
            uint16_t *bg = back + plane * 0xF00;
            uint32_t pixels = word_pair(rows + plane * 0x60);
            uint16_t pixel_word = rows[plane * 0x60 + 2];
            save[0] = bg[0];
            save[1] = bg[1];
            save[2] = bg[2];
            save += 3;
            uint32_t result = (word_pair(bg) & mask) | pixels;
            bg[0] = result >> 16;
            bg[1] = (uint16_t)result;
            bg[2] = (bg[2] & mask_word) | pixel_word;
        }
    }
}

/* $600C: one picture of an entity's sprite, `dx` and `dy` from it, is drawn, and an entry made for the draw queue to
 * put the background back. */
static void draw_sprite(const struct Placement *entity, const uint16_t *placement, uint16_t number)
{
    uint16_t sprite_number = number & 0xFFF;
    uint16_t *sprite = SPRITE_AT(sprite_number);
    uint16_t dx = placement[0];
    if (entity->flags & 1)
        dx = -dx;
    uint16_t x = entity->x + dx;
    turn_sprite(sprite_number, sprite, entity->flags & 1);
    uint16_t y = entity->y + placement[1];
    unsigned shift;
    uint16_t *address = sprite_address(x, y, &shift);
    struct DrawEntry *queue = (struct DrawEntry *)((uint8_t *)&mem.vars + (int32_t)(int16_t)(mem.vars.queue_count << 3) +
                                                   mem.vars.queue_base);
    queue->dest = ST_ADDRESS(address);
    queue->source = mem.vars.backgrounds_now;
    uint16_t *save = BACKGROUNDS_HERE;
    mem.vars.queue_count++;
    composite_sprite(address, save, sprite, shift, queue);
}

/* $5FD0: the pictures of an entity: the records of its list are three words each, where and which. */
static void draw_entity_pictures(const struct Placement *entity)
{
    uint16_t which = entity->field_8 ? entity->field_8 : mem.vars.time_stage;
    const uint16_t *record = (mem.text.records_510a + (mem.vars.picture_lists[which] - TEXT_ADDRESS(0x510A)) / 2);
    for (unsigned count = *record++; count; count--, record += 3)
        draw_sprite(entity, record, record[2]);
}

/* $463C: the pictures of the room itself, from a list of (x, y, which). */
void draw_room_sprites(void)
{
    const uint16_t *list = (mem.text.records_3f58 + (mem.vars.room_picture_lists[mem.vars.room - 1] - TEXT_ADDRESS(0x3F58)) / 2);
    while ((int16_t)list[0] >= 0) {
        mem.text.sprite_entity.x = list[0];
        mem.text.sprite_entity.y = list[1];
        mem.text.sprite_entity.field_8 = list[2];
        list += 3;
        draw_entity_pictures(PLACEMENT(&mem.text.sprite_entity));
    }
}

/* $6448: a picture of 32 by 32 pixels: four planes of 128 bytes become the screen's, interleaved. */
static void draw_picture(const uint16_t *picture, uint16_t *screen)
{
    for (int row = 0; row < 32; row++, screen += 80)
        for (int group = 0; group < 2; group++)
            for (int plane = 0; plane < 4; plane++)
                screen[group * 4 + plane] = picture[plane * 0x40 + row * 2 + group];
}

/* $193A (with $6966 and $6976): one of the three places of the panel shows the picture of an object, or the
 * picture `blank` if there is none. */
static void draw_panel_place(uint16_t object, uint16_t blank, uint16_t *screen)
{
    uint16_t which;
    if ((int16_t)object < 0) {
        which = blank;
    } else {
        const uint16_t *record = (mem.text.records_510a + (mem.vars.picture_lists[mem.text.objects[object * 8 + 2]] - TEXT_ADDRESS(0x510A)) / 2);
        which = record[3];
    }
    draw_picture(SPRITE_AT(which), screen);
}

/* $18FA */
void draw_status_panel(void)
{
    draw_panel_place(mem.vars.player_item, 0xDF, SCREEN_AT(0x5A08));
    draw_panel_place(mem.vars.first_creature_item, 0x04, SCREEN_AT(0x5A28));
    draw_panel_place(mem.vars.second_creature_item, 0xD9, SCREEN_AT(0x5A48));
}

/* $4546: a room is drawn: its map into the back buffer, the objects and the pictures; then the routines of the
 * room take over. */
void draw_room(void)
{
    mark_entities();
    mem.vars.walk_steps_left = 0xFFFF;
    mem.vars.mode_flags &= ~0x40;
    uint16_t number = mem.vars.room;
    if (number >= 0x3B)
        number = mem.text.room_alias[number - 0x3B];
    int16_t offset = (int16_t)(uint16_t)(number << 2) - 4;
    const uint16_t *tiles = mem.heap.level_data + 12 / 2;           /* after the file's 12 byte header */
    const uint16_t *second = tiles + (mem.vars.level_second - mem.vars.level_start) / 2;
    const uint16_t *map = second + word_pair(second + offset / 2) / 2;
    mem.vars.map_run_length = 0xFFFF;
    uint32_t row_start = 0x604;
    for (int row = 0; row < 16; row++, row_start += 0x180)
        for (int column = 0; column < 40; column++)
            draw_map_tile(&map, row_start + column, tiles);
    mem.vars.level_fourth = mem.vars.level_third + (uint32_t)(mem.vars.room - 1) * 16;
    mem.vars.display_flags |= 0x02;
    build_block_map();
    place_room_objects();
    const struct RoomRecord *record = room_record(mem.vars.room);
    struct Entity *first = &mem.text.entities[0];
    first->frame = 0x13;
    mem.vars.first_entity_queue_count = 0;
    first->flags &= ~0x0002;
    if (record->w[9] & 1)
        first->flags |= 0x0002;
    mem.vars.blocker_x = 0;
    mem.vars.control_flags &= ~0x01;
    draw_room_sprites();
    mem.vars.state_routine = word_pair(&record->w[7]);
    mem.vars.display_flags |= 0x02;
    run_room_init(word_pair(&record->w[5]));
}

/* ---- the entities ---- */

/* $49B4: whether the entity stands on a block: its bit 2 of the flags says so. The blocks are the bits of the room's
 * map; a row of them is 6 bytes. The map is read from the start of the variables, past its end too. */
void test_ground(struct Entity *entity)
{
    uint16_t x = entity->x, y = entity->y & 0xFFF8;
    clamp_to_room(&x, &y);
    uint16_t row = (uint16_t)((uint16_t)(y + 0x10) * 6) >> 3;
    if (y & 7)
        row += 6;
    uint16_t index = (x >> 6) + row;
    unsigned bit = ((x & 0xFF) >> 3 ^ 7) & 7;
    if (((const uint8_t *)&mem.vars)[index] & (1 << bit))
        entity->flags |= 0x0004;
    else
        entity->flags &= ~0x0004;
}

/* $5BA2: an entity moves sideways, the other way if it faces left; where it would leave the room it stays, and its
 * edge says which way it went. */
void move_entity_horizontally(struct Entity *entity, int16_t distance)
{
    uint16_t step = entity->flags & 1 ? -distance : distance;
    entity->x += step;
    if (!(entity->x & 0x8000)) {
        if (entity->x < 0x180)
            return;
        entity->edge = 1;
    } else {
        if (entity->x >= 0xFFE0)
            return;
        entity->edge = 2;
    }
    entity->x -= step;
}

/* $5FBE: now and then (one blank in sixteen) the bubble is a different picture. */
static uint16_t bubble_blink(void)
{
    return (mem.text.vbl_counter & 0xF) == 0 ? 4 : 0;
}

static struct Object *object_record(uint16_t number);
static void run_object_use(uint32_t address);

/* The commands of an entity's animation: a record that begins with a negative word is one. They change the entity
 * and its picture pointer, which goes on to the next record, or somewhere else. Returns nonzero when the entity's
 * update starts over. */
static int run_entity_command(struct Entity *entity, const uint16_t *record, unsigned command)
{
    switch (command) {
    case 1:                                     /* $5F22: go to the record at the address that follows */
        entity->image = (mem.text.frame_data + (word_pair(record + 1) - TEXT_ADDRESS(0x848)) / 2);
        return 1;
    case 2: {                                   /* $5F2A: the record the joystick direction picks, if there is one */
        uint8_t direction = mem.vars.joy_dir;
        if (direction) {
            const uint16_t *image = entity_image(entity, direction);
            if (image) {
                entity->image = image;
                return 1;
            }
        }
        entity->image += 1;
        return 1;
    }
    case 3: {                                   /* $5F7C: the bubble, over the player; it ends this entity's turn */
        if (mem.vars.display_flags & 0x04)
            return 0;
        struct Entity *player = &mem.text.entities[PLAYER];
        entity->x = player->x;
        entity->y = player->y;
        entity->flags = player->flags;
        if ((int16_t)--mem.vars.bubble_delay >= 0) {
            draw_sprite(PLACEMENT(entity), mem.text.bubble_placement, entity->field_8);
            return 0;
        }
        mem.vars.bubble_delay = 4;
        entity->image += 1;
        entity->field_8 = bubble_blink() + 0xD7;
        draw_sprite(PLACEMENT(entity), mem.text.bubble_placement, entity->field_8);
        return 0;
    }
    case 4:                                     /* $5EF0: faces left */
        entity->flags |= 0x0001;
        break;
    case 5:                                     /* $5EF8: faces right */
        entity->flags &= ~0x0001;
        break;
    case 6: {                                   /* $5EC2: jumps when the joystick is (not) as the record says */
        uint8_t difference = mem.vars.joy_dir ^ (record[1] & 0xFF);
        int jump = (record[1] & 0x8000) ? difference != 0 : difference == 0;
        if (jump)
            entity->image = (mem.text.frame_data + (word_pair(record + 2) - TEXT_ADDRESS(0x848)) / 2);
        else
            entity->image += 4;
        return 1;
    }
    case 7:                                     /* $5EA8: jumps when it stands on a block */
        test_ground(entity);
        if (entity->flags & 0x0004)
            entity->image = (mem.text.frame_data + (word_pair(record + 1) - TEXT_ADDRESS(0x848)) / 2);
        else
            entity->image += 3;
        return 1;
    case 8:                                     /* $5EA0 */
        entity->fall = 3;
        break;
    case 9:                                     /* $5F14: an item is used */
        use_item();
        break;
    case 10:                                    /* $5E98 */
        entity->flags ^= 0x0001;
        break;
    case 11:                                    /* $5E90 */
        entity->flags |= 0x0008;
        break;
    case 12:                                    /* $5F00 */
        mem.vars.display_flags &= ~0x04;
        break;
    case 13:                                    /* $5F0C */
        mem.vars.display_flags |= 0x04;
        break;
    case 14:                                    /* $5E2A */
        mem.vars.display_flags |= 0x20;
        break;
    case 15:                                    /* $5E34 */
        mem.vars.display_flags &= ~0x20;
        break;
    case 16:                                    /* $6622: the item that is carried does what it does */
        if (!(mem.vars.player_item & 0x8000)) {
            struct Object *object = object_record(mem.vars.player_item);
            run_object_use(word_pair(object->use));
        }
        break;
    case 17:                                    /* $16C2: a flag of the entity turns it round */
        if (entity->flags & 0x0200) {
            entity->flags &= ~0x0200;
            entity->flags ^= 0x0001;
        }
        break;
    case 18:                                    /* $5DFA: it stops at the thing in its way */
        if (mem.vars.blocker_x == 0 || (uint16_t)(entity->x - mem.vars.blocker_x) >= 0x20)
            break;
        entity->image = entity_image(entity, 0x10);
        entity->x = mem.vars.blocker_x + 0x0C;
        mem.vars.control_flags |= 0x01;
        return 1;
    case 19:                                    /* $5DEE: a point */
        add_score_ten();
        mem.vars.points_collected++;
        break;
    case 20:                                    /* $5F5C: a bubble the record describes */
        start_bubble(record[1], record[2]);
        entity->image += 3;
        return 1;
    case 21:                                    /* $5DDA */
        mem.vars.mode_flags |= 0x02;
        break;
    case 22:                                    /* $5DE4 */
        mem.vars.mode_flags &= ~0x02;
        break;
    case 23: {                                  /* $5D40: it is thrown: the entity goes to the first place of the room's, and what was held lands */
        entity->image = 0;
        struct Entity *copy = &mem.text.entities[1];
        memcpy(copy, entity, offsetof(struct Entity, count));
        memset(entity, 0, offsetof(struct Entity, count));
        struct Object *held = mem.vars.thrown_object;
        uint16_t message = 0x2B;
        held->room = 0x20;
        held->y = 0x90;
        held->x = 0x100;
        if (copy->x >= 0x108 && (uint16_t)(copy->x - 0x108) < 0x10) {
            start_bubble(3, 0x0A);
            add_score_hundred();
            copy->flags &= ~0x0002;
            message = 0x2A;
            if ((mem.text.vbl_counter & 3) == 0) {
                for (int i = 0; i < 4; i++)
                    add_score_hundred();
                held->room = 0x19;
                held->x = 0x50;
                message = 0x47;
            }
        }
        post_message(message);
        return 1;
    }
    case 27: {                                  /* $5E3E: at a door */
        const struct RoomRecord *room = room_record(mem.vars.room);
        uint16_t message = 1;
        if ((int16_t)entity->x >= 0x88 && (int16_t)entity->x < 0xC8) {
            if (!((int16_t)room->w[4] < 0)) {
                if ((room->w[9] & 0xFF) & 0x40)
                    entity->flags |= 0x0008;
                entity->image += 3;
                return 1;
            }
            if (mem.vars.room == 0x4C)
                message = 0x4A;
        }
        post_message(message);
        entity->image = (mem.text.frame_data + (word_pair(record + 1) - TEXT_ADDRESS(0x848)) / 2);
        return 1;
    }
    case 24:                                    /* $5D14: a loop starts at the record that follows */
        entity->count = 0;
        entity->image += 1;
        entity->loop_image = entity->image;
        return 1;
    case 25:                                    /* $5D24: the loop ends after as many runs as the record says */
        if (++entity->count != record[1]) {
            entity->image = entity->loop_image;
            return 1;
        }
        entity->image += 2;
        return 1;
    case 26: {                                  /* $5CEC: now and then it changes to one of two records */
        uint16_t chance = mem.text.vbl_counter & 0xF;
        if (chance == 0)
            break;
        entity->image = chance < 6 ? TEXT_AT(const uint16_t, 0x8AA) : TEXT_AT(const uint16_t, 0x876);
        return 1;
    }
    case 28: {                                  /* $5CA0: the thing it throws, with the picture its turn gives */
        static const uint16_t spin[8] = { 0x1D, 0x1D, 0x1E, 0x1E, 0x1F, 0x1F, 0x1E, 0x1E };
        struct Entity *thrown = &mem.text.entities[10];
        thrown->x = entity->x;
        thrown->y = entity->y;
        thrown->x += (entity->flags & 1) ? 0xFFF2 : 0x18;
        thrown->edge = (thrown->edge + 1) & 7;
        thrown->field_8 = spin[thrown->edge];
        break;
    }
    case 29:                                    /* $5C98: it is out of play */
        entity->flags &= ~0x0002;
        return 1;
    case 30:                                    /* $5C84 */
        if (mem.vars.event_flags & 0x80)
            mem.vars.control_flags |= 0x02;
        break;
    case 31:                                    /* $5C6A: a sound effect, which the record names */
        sound_effects_stop();
        sound_effect(record[1]);
        entity->image += 2;
        return 1;
    default:
        unknown_entity_command(command);
        break;
    }
    entity->image += 1;                         /* $5F06: on to the next record */
    return 1;
}

/* $5ACC: an entity that is in play takes the next record of its animation: it moves by it, and falls when it has
 * nothing to stand on, and its pictures are drawn. */
void update_entity(struct Entity *entity)
{
    for (;;) {
        if (!(entity->flags & 0x0002))
            return;
        if (entity->image == 0) {
            draw_entity_pictures(PLACEMENT(entity));
            return;
        }
        const uint16_t *record = entity->image;
        if (record[0] & 0x8000) {
            if (run_entity_command(entity, record, (uint16_t)-(int16_t)record[0]))
                continue;
            return;
        }
        move_entity_horizontally(entity, (int8_t)record[0]);
        if (record[1] != 0) {
            entity->y -= record[1];
        } else {
            test_ground(entity);
            int grounded = 0;
            if ((entity->y & 7) == 0) {
                uint16_t fall = entity->fall;
                entity->fall = 0;
                if (entity->flags & 0x0004)
                    grounded = 1;
                else
                    entity->fall = fall;
            }
            if (!grounded) {
                uint16_t before = entity->fall;
                if (before != 9) {
                    entity->fall++;
                    if (entity->fall == 1) {
                        entity->image = entity_image(entity, 7);
                        continue;
                    }
                }
                entity->y += mem.text.fall_speeds[before];
                test_ground(entity);
                if (entity->flags & 0x0004) {
                    entity->fall = 0;
                    entity->y &= 0xFFF8;
                }
            }
        }
        entity->field_8 = record[2];
        entity->image += 3;
        if (entity->y >= 0xBE)
            entity->edge = 3;
        draw_entity_pictures(PLACEMENT(entity));
        return;
    }
}

/* $038C: the sound starts again when it is done and the game is on. */
void restart_sound_if_done(void)
{
    if (!(mem.vars.control_flags & 0x04))
        return;
    if (mem.text.tune_playing)
        return;
    mem.vars.control_flags &= ~0x04;
    if (mem.vars.control_flags & 0x08)
        sound_start(0);
}

/* $67C8: the record of an object of the rooms. */
static struct Object *object_record(uint16_t number)
{
    return (struct Object *)&mem.text.objects[(uint16_t)(number * 16) / 2];
}

/* $13B8: the first entity is looked after every 20 frames: what its sprites hid is put back (from the second list
 * of the draw queue), it moves on, and it is drawn again. */
void update_first_entity(void)
{
    struct Entity *entity = &mem.text.entities[0];
    if (++entity->frame != 0x14)
        return;
    entity->frame = 0;
    if (!(entity->flags & 0x0002))
        return;
    mem.vars.queue_base = 0x25E;
    mem.vars.queue_count = mem.vars.first_entity_queue_count;
    mem.vars.backgrounds_now = mem.vars.backgrounds_b;
    draw_sprite_queue();
    if (++entity->x == 0x190)
        entity->x = 0xFF90;
    mem.vars.backgrounds_now = mem.vars.backgrounds_b;
    update_entity(entity);
    mem.vars.first_entity_queue_count = mem.vars.queue_count;
}

/* $6534: while bit 7 of display_flags is set the object shaking_entity points at, if it is in its 8th frame, shakes. */
void shake_object(void)
{
    static const int16_t shake[8] = { 0, -2, -4, -2, 0, 2, 4, 2 };
    if (!(mem.vars.display_flags & 0x80))
        return;
    struct Entity *entity = mem.vars.shaking_entity;
    if (entity->frame != 8)
        return;
    entity->y += shake[mem.vars.shake_phase++ & 7];
}

/* $6572: an object walks along, 16 pixels a step, turning round at the edges of the room and where there is nothing
 * to stand on. */
void move_walking_object(void)
{
    if (mem.vars.walk_steps_left & 0x8000)
        return;
    struct Entity *entity = mem.vars.walking_entity;
    uint16_t *recorded_x = &mem.vars.walking_object->x;
    mem.vars.walk_steps_left--;
    int turn;
    if (entity->flags & 1) {
        entity->x += 0x10;
        turn = entity->x >= 0x140;
    } else {
        entity->x -= 0x10;
        turn = entity->x < 0x20;
    }
    if (turn)
        entity->flags ^= 0x0001;
    *recorded_x = entity->x;
    uint16_t y = entity->y;
    entity->y -= 0x10;
    test_ground(entity);
    entity->y = y;
    if (!(entity->flags & 0x0004))
        entity->flags ^= 0x0001;
}

/* $65CC: the objects of the room (the entities from the 11th on) that are in play and have nothing under them
 * fall 4 pixels, and their record follows. */
void drop_objects(void)
{
    for (int i = 11; i < 21; i++) {
        struct Entity *entity = &mem.text.entities[i];
        if (!(entity->flags & 0x0002) || entity->y >= 0x90)
            continue;
        uint16_t y = entity->y;
        entity->y -= 0x20;
        test_ground(entity);
        entity->y = y;
        if (entity->flags & 0x0004)
            continue;
        entity->y += 4;
        object_record(entity->frame)->y = entity->y;
    }
}

/* $33AA: an object thrown flies on a wave, 4 pixels a frame, until its time is up; it turns where it meets an edge. */
void move_thrown_object(void)
{
    if (!(mem.vars.mode_flags & 0x04))
        return;
    struct Entity *entity = mem.vars.thrown_entity;
    uint16_t *recorded_x = &mem.vars.thrown_object->x;
    uint16_t left = mem.vars.thrown_ticks - 1;
    if (left == 0) {
        mem.vars.mode_flags &= ~0x04;
        return;
    }
    mem.vars.thrown_ticks = left;
    entity->y += (left & 7) == 7 ? (int16_t)mem.text.rooms[0].w[0] : mem.text.wave[left & 7];
    move_entity_horizontally(entity, 4);
    *recorded_x = entity->x;
    if (entity->edge) {
        entity->edge = 0;
        entity->flags ^= 0x0001;
    }
}

/* $17AA: when the clock has run out the player falls asleep for a while: a message, and a bubble over his head, and
 * when the clock has moved on he wakes up. */
void fall_asleep(void)
{
    struct Entity *player = &mem.text.entities[PLAYER];
    struct Entity *bubble = &mem.text.entities[10];
    if (mem.vars.event_flags & 0x20) {
        mem.vars.event_flags &= ~0x20;
        player->image = entity_image(player, 0x11);
        mem.vars.event_flags |= 0x40;
        mem.vars.wake_minute_ones = mem.text.clock_text[4];
        mem.vars.wake_minute_tens = mem.text.clock_text[3];
        mem.vars.wake_minute_tens++;
        post_message(3);
        bubble->flags |= 0x0002;
        bubble->field_8 = 0x1D;
        bubble->image = 0;
        bubble->x = player->x;
        bubble->y = player->y;
        bubble->x += (player->flags & 1) ? 0xFFF2 : 0x18;
        return;
    }
    if (!(mem.vars.event_flags & 0x40))
        return;
    if (!(mem.vars.event_flags & 0x80)) {
        if ((uint8_t)mem.text.clock_text[4] != mem.vars.wake_minute_ones)
            return;
        if ((uint8_t)mem.text.clock_text[3] != mem.vars.wake_minute_tens)
            return;
    }
    mem.vars.event_flags &= ~0x40;
    bubble->image = TEXT_AT(const uint16_t, 0x11D2);
    player->image = TEXT_AT(const uint16_t, 0xDCC);
    mem.vars.fatigue = 0;
    mem.vars.display_flags |= 0x08;
}

/* $7096: the fade in of a room starts below the colours, and takes as many steps as the room's table says. */
void start_fade_in(void)
{
    for (int i = 0; i < 16; i++)
        mem.text.fade_palette[i] = mem.text.palettes[0][i] | 0x888;
    uint16_t steps = 7;
    uint16_t room = mem.vars.room, index = 0;
    int table = 1;
    if (room == 0x4B || room == 0x4F) {
        index = 3;
    } else if (room >= 0x10 && (uint16_t)(room - 0x10) < 9) {
        index = room - 0x10;
    } else if (room < 0x3B || (uint16_t)(room - 0x3B) >= 0x0C) {
        table = 0;
    } else {
        index = 3;
    }
    if (table && mem.vars.player_item != 0x0E)
        steps = mem.text.fade_in_table[index];
    mem.text.fade_in_toggle = steps >> 8;
    mem.text.fade_in_steps = (int8_t)steps;
}

/* $4504: which of the five neighbours of a room (1 to 5) another room is, or 0. */
uint16_t find_neighbor(const struct RoomRecord *room, uint16_t other)
{
    for (int i = 0; i < 5; i++)
        if (room->w[i] == other)
            return i + 1;
    return 0;
}

/* $43DC: a player who has gone out of the room through its door, or over one of its edges, comes into the room that
 * is there: the rooms say which they are next to; he stands at the edge he came in by, and the room is drawn. */
void check_room_exit(void)
{
    mem.vars.display_flags &= ~0x02;
    const struct RoomRecord *record = room_record(mem.vars.room);
    struct Entity *player = &mem.text.entities[PLAYER];
    uint16_t next;
    int through_door = player->flags & 0x08;
    player->flags &= ~0x08;
    if (through_door) {
        next = record->w[4];
        if (next & 0x8000)
            return;
        if ((int16_t)player->x < 0x88 || (int16_t)player->x >= 0xC8)
            return;
        player->image = TEXT_AT(const uint16_t, 0xB80);
    } else if (player->y >= 0xBE) {
        next = record->w[2];
        if (next & 0x8000)
            return;
    } else if (player->x & 0x8000) {
        next = record->w[1];
        if (next & 0x8000)
            return;
    } else if (player->x >= 0x158) {
        next = record->w[0];
        if (next & 0x8000)
            return;
    } else if (player->y < 0x17) {
        next = record->w[3];
        if (next & 0x8000)
            return;
    } else {
        return;
    }
    uint16_t from = mem.vars.room;
    mem.vars.previous_room = from;
    mem.vars.room = next;
    record = room_record(next);
    uint16_t entry = find_neighbor(record, from);
    if ((record->w[9] & 0xFF) & (1 << entry))
        mem.vars.display_flags |= 0x40;
    switch (entry) {
    case 1:
        player->x = 0x150;
        player->flags |= 0x0001;
        break;
    case 2:
        player->x = 4;
        player->flags &= ~0x0001;
        break;
    case 3:
        player->y = 0xB9;
        player->image = entity_image(player, 0x0F);
        break;
    case 4:
        player->y = 0x18;
        break;
    default:
        player->x = 0xB0;
        player->image = entity_image(player, (record->w[9] & 0x40) ? 0x16 : 0x0A);
        break;
    }
    draw_room();
}

/* ---- the companions and the things to carry ---- */

/* $1976, $199A, $19A4: points are added at a place of the score, which carries to the one before it. */
static void add_to_score(unsigned place)
{
    mem.vars.display_flags |= 0x01;
    char *digit = &mem.text.score_text[place];
    for (;;) {
        (*digit)++;
        if (*digit != 0x3A)
            break;
        *digit-- = '0';
    }
}

void add_score_ten(void) { add_to_score(3); }
void add_score_hundred(void) { add_to_score(2); }

/* $6642: the energy is used up: the item goes, a message comes, and the panel is drawn. */
void lose_item(void)
{
    mem.vars.player_item = 0xFFFF;
    post_message((mem.text.vbl_counter & 1) + 0x27);
    mem.vars.fatigue = 0;
    mem.vars.display_flags |= 0x08;
    add_score_ten();
    draw_status_panel();
}

/* $1858: the fatigue goes up by one step; when it is at its maximum the item that was carried is lost. */
void fatigue_up(void)
{
    mem.vars.event_flags |= 0x20;
    if (mem.vars.fatigue < 0x1F) {
        mem.vars.fatigue++;
        mem.vars.display_flags |= 0x08;
        mem.vars.event_flags &= ~0x20;
        return;
    }
    mem.vars.fatigue = 0x1F;
    if (mem.vars.player_item & 0x8000)
        return;
    post_message(0x2C);
    lose_item();
    mem.vars.event_flags &= ~0x20;
}

/* $1638: whether two entities are near enough to touch. */
static int entities_near(const struct Entity *a, const struct Entity *b)
{
    if ((uint16_t)(b->x - a->x + 0x20) >= 0x40)
        return 0;
    return (uint16_t)(b->y - a->y + 0x30) < 0x60;
}

/* $161A: the room an entity goes to over the edge it went over (1 to 5 of the room's neighbours); a negative one
 * is none. The edge is cleared. */
static uint16_t neighbor_room(struct Entity *entity, uint16_t edge_less_one)
{
    entity->edge = 0;
    return room_record(entity->frame)->w[edge_less_one];
}

/* $1542 */
static void turn_around(struct Entity *entity)
{
    entity->flags ^= 0x0001;
}

/* $6928: whether an object at (x, y) can be reached by an entity that faces a way: 0 if it can, -1 if not. */
static int reachable(const struct Entity *object, const struct Entity *by, uint16_t x, uint16_t y)
{
    int16_t dy = y - object->y;
    if (dy >= 0 || dy < (int16_t)0xFFE0)
        return -1;
    int16_t dx = x - object->x;
    if (by->flags & 1) {
        if (dx < 0 || dx < 0x10 || dx >= 0x28)
            return -1;
        return 0;
    }
    if (dx >= 0)
        return -1;
    dx += 0x10;
    if (dx >= 0)
        return -1;
    return dx >= (int16_t)0xFFE8 ? 0 : -1;
}

/* $68F8: which object of the room, if any, can be reached by an entity: its number, or -1. The object found is
 * left in `found`. */
static int16_t find_reachable_object(const struct Entity *by, struct Entity **found)
{
    for (int i = 11; i < 21; i++) {
        struct Entity *object = &mem.text.entities[i];
        if ((object->flags & 0x0002) && reachable(object, by, by->x, by->y) == 0) {
            *found = object;
            return object->frame;
        }
    }
    return -1;
}

/* $68D4: an object is picked up: it leaves the room, its record is marked, and the panel is drawn. */
static void take_object(struct Entity *object, int16_t number)
{
    object->flags &= ~0x0002;
    mem.vars.dropped_objects--;
    object_record(number)->x |= 0x8000;
    draw_status_panel();
}

/* $679E: a free entity for an object put down in the room (the 11th to the 20th), unless there are ten already. */
static struct Entity *free_object_entity(void)
{
    if (mem.vars.dropped_objects == 0x0A)
        return NULL;
    for (int i = 11; i < 21; i++)
        if (!(mem.text.entities[i].flags & 0x0002))
            return &mem.text.entities[i];
    return NULL;
}

/* $67DC: an object lies at a place: an entity shows it, and its record says where it is. */
static void place_object(struct Entity *entity, struct Object *object, uint16_t x, uint16_t y)
{
    entity->x = x;
    entity->y = y;
    entity->frame = object->picture - 0x59;
    entity->flags = 2;
    entity->field_8 = object->picture;
    object->x = x;
    object->y = y;
    object->room = mem.vars.room;
}

/* $6784: object number `number` is put in the room at (x, y), if there is a free entity for it. */
static void place_new_object(uint16_t number, uint16_t x, uint16_t y)
{
    struct Entity *entity = free_object_entity();
    if (entity)
        place_object(entity, object_record(number), x, y);
}

/* $31C6: the object that was put down in the ninth room of the third level starts to move. */
static void start_animation_2b64(void)
{
    post_message(0x45);
    mem.text.animated_2b64.image = mem.text.sequences + 8;
    mem.vars.mode_flags |= 0x40;
    mem.text.animated_2b64.at.frame = 0;
    mem.text.animated_2b64.at.field_8 = 0x28;
}

/* The routine an object runs when it has been put down (the long at +$0C of its record): the entity that shows it
 * is `entity`. */
static void run_object_placed(uint32_t address, struct Object *object, struct Entity *entity)
{
    switch (address - TEXT_ADDRESS(0)) {
    case 0x66B8:
        return;
    case 0x66BA:                                /* the trapdoor of the third room opens */
        if (mem.vars.room != 3 || (mem.vars.event_flags & 0x01))
            return;
        if (mem.text.entities[PLAYER].x < 0x90 || mem.text.entities[PLAYER].x >= 0xF0)
            return;
        mem.vars.event_flags |= 0x01;
        mem.vars.event_flags |= 0x08;
        place_new_object(9, 0xC0, 0x60);
        place_new_object(0x15, 0xB8, 0x60);
        draw_entity_pictures(&mem.text.trapdoor_picture);
        return;
    case 0x6704:
        if (mem.vars.room != 0x20)
            return;
        mem.vars.progress_flags |= 0x10;
        mem.vars.dropped_object = object;
        return;
    case 0x6718:
        mem.vars.walking_object = object;
        mem.vars.walking_entity = entity;
        mem.vars.walk_steps_left = 0x32;
        if (mem.vars.room != 0x35)
            return;
        start_animation_2b64();
        if (mem.vars.progress_flags & 0x04)
            return;
        mem.vars.progress_flags |= 0x04;
        mem.vars.hidden_entity->flags &= ~0x0002;
        place_new_object(0x0B, 0xF0, 0x46);
        return;
    case 0x6752:
        mem.vars.shaking_entity = entity;
        mem.vars.shake_phase = 0;
        mem.vars.display_flags |= 0x80;
        if (mem.vars.room != 0x35)
            return;
        post_message(0x48);
        start_bubble(3, 0x1E);
        add_score_ten();
        return;
    default:
        unknown_room_routine(address);
        return;
    }
}

/* The routine an object runs when it is used (the long at +$08 of its record). */
static void run_object_use(uint32_t address)
{
    switch (address - TEXT_ADDRESS(0)) {
    case 0x6642:
        lose_item();
        return;
    case 0x666C:                                /* it gets better */
        mem.vars.player_item++;
        post_message(mem.vars.player_item + 5);
        add_score_ten();
        draw_status_panel();
        return;
    case 0x667E:
        if (mem.vars.room != 0x35)
            return;
        mem.vars.player_item = 0x0D;
        post_message(0x12);
        add_score_ten();
        draw_status_panel();
        return;
    case 0x6696:
        add_score_hundred();
        lose_item();
        return;
    case 0x669C:
        if (mem.vars.room != 0x37)
            return;
        post_message(0x11);
        mem.vars.player_item = 0x0C;
        draw_status_panel();
        return;
    case 0x66B6:
        return;
    default:
        unknown_room_routine(address);
        return;
    }
}

/* $6822 (from $6810, $6868, $687E): what a creature carries in one place of the panel is put down in front of it. */
void drop_item(uint16_t *slot, struct Entity *by)
{
    if (*slot & 0x8000)
        return;
    struct Entity *entity = free_object_entity();
    if (!entity)
        return;
    struct Object *object = object_record(*slot);
    uint16_t x = by->x + ((by->flags & 1) ? 0xFFE0 : 0x20);
    uint16_t y = (by->y + 0x18) & 0xFFFD;
    clamp_to_room(&x, &y);
    place_object(entity, object, x, y);
    run_object_placed(word_pair(object->placed), object, entity);
    *slot = 0xFFFF;
    draw_status_panel();
}

/* $6808: the player puts down what he carries, or picks up what is near. */
void use_item(void)
{
    struct Entity *player = &mem.text.entities[PLAYER];
    if (!(mem.vars.player_item & 0x8000)) {
        drop_item(&mem.vars.player_item, player);
        return;
    }
    struct Entity *object;
    int16_t number = find_reachable_object(player, &object);
    if (number < 0)
        return;
    mem.vars.player_item = number;
    post_message(number + 5);
    take_object(object, number);
}

/* $6894: the creature picks up what it can reach, in the second place of the panel, if that is free. */
static void creature_picks_up(struct Entity *creature)
{
    if (!(mem.vars.first_creature_item & 0x8000))
        return;
    struct Entity *object;
    int16_t number = find_reachable_object(creature, &object);
    if (number < 0)
        return;
    mem.vars.first_creature_item = number;
    take_object(object, number);
}

/* $165E: in the ninth room the creature that gets to the object shaking_entity points at is held. */
static void creature_meets_object(struct Entity *creature)
{
    if (mem.vars.room != 9)
        return;
    struct Entity *object = mem.vars.shaking_entity;
    if (!object || object->frame != 8 || !(object->flags & 0x0002))
        return;
    uint16_t x = creature->x;
    if (!(creature->flags & 1)) {
        if (x < 0x100) {
            int16_t d = x - object->x;
            if (d < 0 || d < 0x20)
                return;
        }
    } else {
        if (x >= 0x40) {
            int16_t d = x - object->x;
            if (d >= 0)
                return;
            if ((uint16_t)d >= 0xFFE0)
                return;
        } else {
            mem.vars.progress_flags |= 0x20;
            mem.vars.mode_flags |= 0x01;
            post_message(0x49);
        }
    }
    creature->flags |= 0x0200;
}

/* $140E: the creature of the room, when it is near the player: the items of the panel change places, or, when the
 * game is waiting for it, it takes the player's turn. */
static void creature_meets_player(struct Entity *creature)
{
    if (mem.vars.room != creature->frame)
        return;
    if (mem.text.vbl_counter < 0x10)
        creature_picks_up(creature);
    if (mem.vars.event_flags & 0x40)
        return;
    if (mem.vars.event_flags & 0x20)
        return;
    struct Entity *player = &mem.text.entities[PLAYER];
    if (!entities_near(creature, player))
        return;
    if (!(mem.vars.display_flags & 0x20)) {
        uint16_t first = mem.vars.player_item;
        if (first == 4)
            return;
        if (mem.vars.first_creature_item == 4 && first != 7)
            return;
        mem.vars.player_item = mem.vars.first_creature_item;
        mem.vars.first_creature_item = first;
        draw_status_panel();
        if (mem.text.vbl_counter & 1)
            fatigue_up();
        return;
    }
    creature->image = entity_image(creature, 1);
    mem.vars.display_flags &= ~0x20;
    creature->flags = (creature->flags & ~1) | (player->flags & 1);
    start_bubble(3, 0x0A);
    add_score_ten();
    post_message(0x2F);
    sound_effect(8);
}

/* The routines a companion runs when it comes into a room, by the way it came ($4494, $44A0, $44E0, $44E8). */
static void companion_enters(struct Entity *companion, uint16_t entry)
{
    switch (entry) {
    case 1:
        companion->x = 0x150;
        companion->flags |= 0x0001;
        break;
    case 2:
        companion->x = 4;
        companion->flags &= ~0x0001;
        break;
    case 3:
        companion->y = 0x9F;
        break;
    case 4:
        companion->y = 0x18;
        break;
    case 5:                                     /* $44C6: it tests bit 6 of a byte of its own code, which is set */
        companion->x = 0xB0;
        companion->image = entity_image(companion, 0x16);
        break;
    }
}

/* $1502: the creature (the sixth entity) goes from room to room on its own while the player is elsewhere, and
 * comes back after a long time; when it is in the room it meets the player. */
void update_companion(void)
{
    struct Entity *creature = &mem.text.entities[6];
    uint16_t room = mem.vars.room;
    if (room == 0x32 || room == 0x33 || room == 0x34 || room == 0x3A) {
        creature->flags &= ~0x0002;
        return;
    }
    if (room == creature->frame) {
        creature->flags |= 0x0002;
        mem.vars.creature_away_ticks = 0;
        if (creature->edge == 0) {
            creature_meets_object(creature);
            creature_meets_player(creature);
            return;
        }
    } else {
        creature->flags &= ~0x0002;
        if (++mem.vars.creature_away_ticks == 0x3E8) {
            mem.vars.creature_away_ticks = 1;
            creature->frame = mem.vars.previous_room;
        }
        move_entity_horizontally(creature, 8);
        if (creature->edge == 0)
            return;
    }
    creature->flags &= ~0x0002;
    uint16_t from = creature->frame;
    uint16_t next = neighbor_room(creature, creature->edge - 1);
    if (next & 0x8000) {
        turn_around(creature);
        return;
    }
    creature->frame = next;
    const struct RoomRecord *record = room_record(next);
    uint16_t entry = 0;
    if (room == next || find_neighbor(record, room) != 0)
        entry = find_neighbor(record, from);
    if (entry == 0) {
        record = room_record(from);
        uint16_t via = find_neighbor(record, room);
        creature->frame = via ? from : mem.vars.previous_room;
        creature->x = 0xA0;
        turn_around(creature);
        return;
    }
    companion_enters(creature, entry);
    if (creature->frame != mem.vars.room)
        return;
    creature->flags |= 0x0002;
    if (!(mem.text.vbl_counter & 2))
        return;
    post_message(0x2D);
    if (mem.vars.control_flags & 0x08)
        return;
    sound_start(1);
    mem.vars.control_flags |= 0x04;
}

/* $16D6: the second creature (the fifth entity) likewise; it is the one that is called, and takes an item. */
void update_second_companion(void)
{
    struct Entity *creature = &mem.text.entities[5];
    struct Entity *player = &mem.text.entities[PLAYER];
    if (mem.vars.room == creature->frame) {
        creature->flags |= 0x0002;
        if (creature->edge == 0) {
            if (!(mem.vars.display_flags & 0x20))
                return;
            if (reachable(creature, player, player->x, player->y - 0x18) != 0)
                return;
            creature->image = entity_image(creature, 2);
            creature->flags = (creature->flags & ~1) | (player->flags & 1);
            if (!(mem.vars.event_flags & 0x04)) {
                mem.vars.event_flags |= 0x04;
                add_score_hundred();
                return;
            }
            mem.vars.event_flags |= 0x04;
            if ((int16_t)--mem.vars.hand_over_delay >= 0)
                return;
            mem.vars.hand_over_delay = 2;
            if (!(mem.vars.second_creature_item & 0x8000))
                drop_item(&mem.vars.second_creature_item, creature);
            return;
        }
    } else {
        if (!(mem.vars.event_flags & 0x04))
            return;
        creature->flags &= ~0x0002;
        move_entity_horizontally(creature, 0x10);
        if (creature->edge == 0)
            return;
    }
    creature->flags &= ~0x0002;
    uint16_t from = creature->frame;
    uint16_t next = neighbor_room(creature, creature->edge - 1);
    if (next & 0x8000) {
        turn_around(creature);
        return;
    }
    creature->frame = next;
    uint16_t entry = find_neighbor(room_record(next), from);
    companion_enters(creature, entry);
    if (creature->frame == mem.vars.room)
        creature->flags |= 0x0002;
}

/* ---- the panel ---- */

/* $5F68: a speech bubble: which picture of the entity it is, and how long it stays. */
void start_bubble(uint16_t kind, uint16_t time)
{
    mem.text.entities[9].field_8 = kind + 0xD7;
    mem.vars.bubble_delay = time;
}

/* $1A68: a message for the panel. One that is being shown has to be finished first, and a new one waits; some
 * of them come with a speech bubble. */
void post_message(uint16_t number)
{
    if (mem.vars.message_progress != 0x0F) {
        mem.vars.next_message = number;
        return;
    }
    mem.vars.message_number = number;
    mem.vars.message_progress = 0;
    switch (number) {
    case 0x00: case 0x05: case 0x07: case 0x0A: case 0x28: case 0x2B: case 0x2E: case 0x17: case 0x18:
    case 0x1B: case 0x21: case 0x46: case 0x47: case 0x49: case 0x4A: case 0x4D:
        start_bubble(9, 0x14);
    }
}

/* $1BA8: a message goes in the middle of the line of 20 characters, and one that is longer is not cut. */
static void center_message(const char *text)
{
    char *line = mem.text.message_line;
    memset(line, ' ', 20);
    line[20] = 0;
    line[21] = 0;
    unsigned length = 0;
    unsigned start = 0;
    while (text[length]) {
        length++;
        if (length == 20) {
            length = 0;
            goto copy;
        }
    }
    start = (20 - length) >> 1;
copy:
    {
        char *to = line + start;
        const char *from = text;
        while ((*to++ = *from++))
            ;
        to[-1] = ' ';
    }
}

/* $1B64: up to 20 characters of a line are drawn in the font, 16 pixels wide, as many rows of them as `rows`. */
static void draw_text_line(const char *text, uint16_t *cell, unsigned rows)
{
    const uint16_t *font = FONT;
    for (int n = 0; n < 20; n++, cell += 4) {
        uint8_t character = (uint8_t)text[n];
        if (character == 0)
            return;
        uint16_t glyph = (uint16_t)((character - 0x20) << 7) / 2;
        for (unsigned row = 0; row < rows; row++) {
            const uint16_t *from = font + glyph + row * 4;
            uint16_t *to = cell + row * 80;
            to[0] = from[0];
            to[1] = from[1];
            to[2] = from[2];
            to[3] = from[3];
        }
    }
}

/* $1AF0: the message of the panel is typed a row at a time; when it is done the next one comes, and while the
 * game is on a new message comes every 64 frames. */
void update_message(void)
{
    uint16_t progress = mem.vars.message_progress;
    if (progress == 0x0F) {
        if (!(mem.vars.next_message & 0x8000)) {
            mem.vars.message_number = mem.vars.next_message;
            mem.vars.next_message = 0xFFFF;
            mem.vars.message_progress = 0;
            return;
        }
        if (mem.vars.mode_flags & 0x08) {
            mem.vars.idle_ticks++;
            if (mem.vars.idle_ticks == 0x40) {
                mem.vars.idle_ticks = 0;
                uint16_t next = mem.vars.idle_step + 1;
                if (next == 0x0F)
                    next = 0;
                mem.vars.idle_step = next;
                post_message(next + 0x32);
            }
        }
        return;
    }
    mem.vars.message_progress++;
    center_message(mem.text.message_pointers[(uint16_t)(mem.vars.message_number << 2) / 4]);
    draw_text_line(mem.text.message_line, SCREEN_AT(0x73A0), progress + 1);
}

/* $19AE: the clock of the panel, once a second: its colons blink, and minutes and seconds count up to 59:59. */
void update_clock(void)
{
    char *clock = mem.text.clock_text;
    if (!(mem.text.vbl_event & 0x8000))
        return;
    mem.text.vbl_event = 0;
    clock[2] ^= 0x1A;
    clock[5] ^= 0x1A;
    if (clock[2] != 0x20) {
        int minutes = (mem.vars.event_flags & 0x40) != 0;
        if (!minutes) {
            if (++clock[7] == 0x3A) {
                clock[7] = '0';
                if (++clock[6] == 0x36) {
                    clock[6] = '0';
                    minutes = 1;
                }
            }
        }
        if (minutes && ++clock[4] == 0x3A) {
            clock[4] = '0';
            mem.vars.time_stage++;
            if (++clock[3] == 0x36) {
                clock[3] = '0';
                memcpy(clock + 2, ":59:59", 6);
                if (!(mem.vars.mode_flags & 0x08))
                    mem.vars.event_flags |= 0xA0;
            }
        }
    }
    draw_text_line(clock + 3, SCREEN_AT(0x5A60), 16);
}

/* $1954: the score is drawn when it has changed. */
void update_score_display(void)
{
    if (!(mem.vars.display_flags & 0x01))
        return;
    mem.vars.display_flags &= ~0x01;
    draw_text_line(mem.text.score_text, SCREEN_AT(0x6460), 16);
}

/* $647E: a picture of 32 rows is drawn onto the screen with its mask, and what it covers is kept. */
static void draw_masked_picture(const uint16_t *picture, uint16_t *screen)
{
    uint16_t *save = BACKGROUNDS_HERE;
    for (int row = 0; row < 32; row++, picture += 2, screen += 80) {
        uint32_t mask = ~(word_pair(picture) | word_pair(picture + 0x40) | word_pair(picture + 0x80) |
                          word_pair(picture + 0xC0));
        for (int half = 0; half < 2; half++) {
            uint16_t keep = half == 0 ? mask >> 16 : (uint16_t)mask;
            for (int plane = 0; plane < 4; plane++) {
                uint16_t *word = &screen[half * 4 + plane];
                *save++ = *word;
                *word = (*word & keep) | picture[plane * 0x40 + half];
            }
        }
    }
}

/* $1896: the energy bar: full at no fatigue, else the part that is left. */
void draw_fatigue_bar(void)
{
    if (!(mem.vars.display_flags & 0x08))
        return;
    mem.vars.display_flags &= ~0x08;
    if (mem.vars.fatigue == 0) {
        const uint16_t *picture = SPRITES + 0xA00 / 2;
        draw_masked_picture(picture, SCREEN_AT(0x4FF0 + 0xB40));
        draw_masked_picture(picture, SCREEN_AT(0x4FF0));
    } else {
        const uint16_t *picture = SPRITES + 0xC00 / 2;
        draw_masked_picture(picture, SCREEN_AT(0x5130 + (uint16_t)(mem.vars.fatigue >> 1) * 0x140));
    }
}

/* ---- sound ---- */

#define SOUND_BASE TEXT_ADDRESS(0x6D5C)

/* Where the sound data is, from an offset the tables keep (relative to $6D5C). */
static const uint8_t *sound_data(uint16_t offset)
{
    return ST_PTR(const uint8_t, SOUND_BASE + (int16_t)offset);
}

/* A word of the sound data, which lies among the bytes of the tunes. */
static uint16_t sound_word(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

/* $7772: the sound chip goes silent. */
static void silence_sound_chip(void)
{
    ym_write(7, ym_read(7) | 0x3F);   /* all channels off */
    ym_write(8, 0);                   /* the three volumes */
    ym_write(9, 0);
    ym_write(10, 0);
}

/* $876A: the effects stop and the sound chip goes silent; a tune goes on. */
void sound_effects_stop(void)
{
    memset(mem.text.effect_on, 0, sizeof mem.text.effect_on);
    silence_sound_chip();
}

/* $7758: the sound engine stops and the sound chip goes silent. */
void sound_stop(void)
{
    mem.text.tune_playing = 0;
    mem.text.psg_volume[0] = 0;
    mem.text.psg_volume[1] = 0;
    mem.text.psg_volume[2] = 0;
    mem.text.psg_mixer = 0x3F;
    sound_effects_stop();
}

/* $7378: a set of sounds begins: it brings the tempo and, for each of the three voices, the start of what the
 * voice plays. A set is four words: the tempo in the low byte of the first, then the offset (from $6D5C) of the
 * word that tells where each voice starts. */
void sound_start(int8_t set)
{
    sound_stop();
    mem.text.transpose = 0;
    unsigned entry = (unsigned)(uint16_t)((uint16_t)(int16_t)set * 8) / 2;     /* in words of sound_sets */
    mem.text.tempo = (uint8_t)mem.text.sound_sets[entry];
    mem.text.tempo_reload = mem.text.tempo;
    for (int v = 0; v < 3; v++) {
        struct Voice *voice = &mem.text.voices[v];
        voice->tick = 1;
        voice->state = 0;
        voice->vibrato_state = 0;
        voice->flag_2d = 0;
        voice->transpose = 0;
        voice->pattern_base = TEXT_AT(const uint8_t, 0x7C6C);
        voice->pattern = TEXT_AT(const uint8_t, 0x7C6C);
        uint16_t offset = mem.text.sound_sets[entry + 1 + v];
        voice->instrument = offset;
        voice->speed = 2;
        uint16_t start = sound_word(sound_data(offset));
        voice->sequence = sound_data(start);
    }
    memset(mem.text.voice_muted, 0, sizeof mem.text.voice_muted);
    mem.text.fade_level = 0x0F;
    mem.text.repeat_length = 0;
    mem.text.tempo_phase = 0xFF;
    mem.text.tune_resumes = 0xFF;
    mem.text.tune_playing = 0xFF;
}

/* $817E: a sound effect begins on one of the three channels of the sound chip. */
static void start_effect(unsigned channel, uint8_t effect)
{
    struct Effect *fx = &mem.text.effects[channel];
    mem.text.effect_on[channel] = 0;
    const uint8_t *description = sound_data(mem.text.effect_descriptors[effect]);
    fx->timer = description[0];
    fx->pause = description[1];
    fx->period = description[2] << 8 | description[3];
    fx->slide = description[4] << 8 | description[5];
    fx->flags = description[6];
    fx->noisy = description[7];
    fx->direction = description[8];
    fx->repeats = description[9];
    fx->volumes = description[10];
    fx->volume_pause = description[11];
    fx->length = description[12];
    fx->slide_every = description[13];
    fx->step = fx->pause;
    mem.text.effect_period[channel] = fx->period;
    if (!(fx->flags & 0x08))
        mem.text.effect_noise = (uint8_t)fx->period;
    fx->volume_timer = fx->volume_pause;
    fx->slide_timer = fx->slide_every;
    const uint8_t *volumes = sound_data(mem.text.effect_volume_lists[fx->volumes]);
    fx->volumes_start = volumes;
    fx->volumes_at = volumes;
    mem.text.effect_volume[channel] = *volumes;
    mem.text.effect_on[channel] = 1;
}

/* $03B2: a sound effect, unless the sound is off or one is playing. */
void sound_effect(uint16_t effect)
{
    if (mem.vars.control_flags & 0x08)
        return;
    if (mem.vars.control_flags & 0x04)
        return;
    start_effect(2, effect);
}

/* $874A: the random number the effects use is a shift register of 32 bits. */
static void shift_random(void)
{
    uint8_t taps = ((mem.text.sound_random >> 24) & 0x48) + 0x38;
    mem.text.sound_random = mem.text.sound_random << 1 | ((taps >> 6) & 1);
}

/* $82F6, $83B0, $846A: an effect goes one step on: its tone changes, and its volume follows its list. */
static void effect_step(unsigned channel)
{
    struct Effect *fx = &mem.text.effects[channel];
    uint16_t *period = &mem.text.effect_period[channel];
    if (fx->timer == 0 && fx->length == 0) {
        mem.text.effect_on[channel] = 0;
        mem.text.psg_volume[channel] = 0;
        return;
    }
    fx->timer--;
    if (fx->step == 0) {
        if (fx->length == 0 && fx->repeats == 0)
            goto volumes;
        fx->repeats--;
        fx->step = fx->pause;
        uint8_t noise = fx->noisy ? (uint8_t)(mem.text.sound_random >> (24 - 8 * channel)) : 0;
        unsigned low = (uint8_t)fx->period + noise;
        unsigned high = (fx->period >> 8) + noise + (low >> 8);
        *period = (uint16_t)((uint8_t)high << 8 | (uint8_t)low);
    }
    fx->step--;
    if (fx->slide_every != 0) {
        if (--fx->slide_timer != 0)
            goto noise;
        fx->slide_timer = fx->slide_every;
    }
    if (fx->direction != 0) {
        if (fx->direction < 0)
            *period += fx->slide;
        else
            *period -= fx->slide;
    }
noise:
    if (!(fx->flags & 0x08))
        mem.text.effect_noise = (uint8_t)*period;
volumes:
    if (--fx->volume_timer != 0)
        return;
    fx->volume_timer = fx->volume_pause;
    const uint8_t *at = fx->volumes_at;
    uint8_t volume = *at++;
    if (volume & 0x80) {
        if (volume != 0x80)
            return;
        at = fx->volumes_start;
        volume = *at++;
    }
    fx->volumes_at = at;
    mem.text.effect_volume[channel] = volume;
}

/* $82CE: the effects that are playing. */
static void effects_step(void)
{
    shift_random();
    if (mem.text.effect_on[0] & 0x80)
        return;
    if (mem.text.effect_on[0])
        effect_step(0);
    if (mem.text.effect_on[1])
        effect_step(1);
    if (mem.text.effect_on[2])
        effect_step(2);
}

/* $7A28: the tone a voice has now, and its volume: its envelope goes on, its note and the pattern of pitches give the
 * tone, the vibrato and the slide change it, and the mixer is told which of tone and noise the voice makes. */
static uint16_t voice_tone(struct Voice *voice, uint8_t *volume)
{
    uint8_t state = voice->state;
    if (state & 0x20) {
        if (voice->envelope_timer-- == 0) {
            voice->envelope_timer = voice->envelope_rate;
            uint8_t next = voice->envelope[1];
            if (!(next & 0x80)) {
                voice->envelope++;
                voice->envelope_level = next;
            }
        }
        voice->volume = voice->envelope_level;
    }
    uint8_t note = voice->note + mem.text.transpose + voice->transpose;
    const uint8_t *pattern = voice->pattern;
    uint8_t offset = *pattern++;
    if (offset & 0x80) {
        offset &= 0x7F;
        pattern = voice->pattern_base;
    }
    voice->pattern = pattern;
    note += offset;
    uint8_t index = (uint8_t)(note * 2);
    uint16_t tone = mem.text.note_table[index / 2];
    uint8_t vibrato = voice->vibrato_state;
    if (vibrato & 0x40) {
        uint8_t shift_count = index;
        uint8_t depth = (uint8_t)(voice->vibrato_depth << 1);
        uint8_t position = voice->vibrato_position;
        if (!((vibrato & 0x80) && (state & 1))) {
            if (vibrato & 0x20) {
                position += voice->vibrato_step;
                if (position >= depth) {
                    voice->vibrato_state &= ~0x20;
                    position = depth;
                }
            } else {
                int borrow = position < voice->vibrato_step;
                position -= voice->vibrato_step;
                if (borrow) {
                    voice->vibrato_state |= 0x20;
                    position = 0;
                }
            }
            voice->vibrato_position = position;
        }
        depth >>= 1;
        uint16_t swing = position;
        if (position < depth)
            swing = (uint16_t)((uint8_t)(position - depth) - 0x100);
        else
            swing = (uint8_t)(position - depth);
        unsigned sum = shift_count + 0xA0;
        shift_count = (uint8_t)sum;
        if (!(sum & 0x100)) {
            for (;;) {
                swing = (uint16_t)(swing << 1);
                sum = shift_count + 0x18;
                shift_count = (uint8_t)sum;
                if (sum & 0x100)
                    break;
            }
        }
        tone += swing;
    }
    state ^= 1;
    voice->state = state;
    if (state & 0x04) {
        uint8_t count = voice->slide_count - 1;
        if (count != 0) {
            voice->slide_count = count;
        } else {
            voice->slide += (int16_t)voice->slide_step;
            tone += voice->slide;
        }
    }
    uint8_t both = ~state & 3;
    uint8_t mixer = mem.text.mixer_wanted;
    if (both == 0) {
        mem.text.noise_period = mem.text.noise_note ^ 0x08;
        mixer = 0x07;
    }
    uint8_t current = mem.text.psg_mixer;
    mixer = ((current ^ mixer) & voice->mixer) ^ current;
    if (voice->flag_2d & 0x80) {
        voice->flag_2d &= 0x7F;
        mixer &= ~(voice->mixer & 0x38);
        mem.text.noise_period = 1;
    }
    mem.text.psg_mixer = mixer;
    *volume = voice->volume;
    return tone;
}

/* Where a voice goes on after a command or a note ($795A): the note lasts as long as it is to, the voice's place is
 * kept, and a voice that is to stand back for the effects does so. */
static void voice_goes_on(struct Voice *voice, const uint8_t *sequence)
{
    voice->tick = voice->length;
    voice->sequence = sequence;
    if (voice->flag_2d == 0)
        return;
    if (mem.text.effect_on[0] && !(mem.text.effects[0].flags & 0x08))
        return;
    if (mem.text.effect_on[1] && !(mem.text.effects[1].flags & 0x08))
        return;
    if (mem.text.effect_on[2] && !(mem.text.effects[2].flags & 0x08))
        return;
    voice->flag_2d = 0xFF;
}

/* $7918: a voice, once a tick of the tempo: when its note has run out it reads what comes next. Returns nonzero when
 * the tune has stopped. */
static int voice_step(struct Voice *voice)
{
    if (--voice->tick != 0) {
        if (voice->state & 0x08) {
            if (voice->state & 0x80)
                voice->note++;
            else
                voice->note--;
        }
        return 0;
    }
    voice->state = 0;
    const uint8_t *sequence = voice->sequence;
    for (;;) {
        uint8_t byte = *sequence++;
        if (!(byte & 0x80)) {                   /* a note */
            voice->note = byte;
            if (voice->flag_1)
                mem.text.noise_note = byte;
            const uint8_t *envelope = voice->envelope_base;
            voice->envelope = voice->envelope_base;
            voice->envelope_level = *envelope;
            voice->volume = *envelope;
            voice->envelope_timer = voice->envelope_rate;
            voice->state |= 0x20;
            break;
        }
        if (byte >= 0xB8) {
            if (byte >= 0xE0) {                 /* how long the notes last */
                voice->length = (uint8_t)(byte + 0x21);
            } else if (byte >= 0xD0) {          /* which envelope */
                const uint8_t *envelope = sound_data(mem.text.envelopes[byte - 0xD0]);
                voice->envelope_base = envelope;
                voice->envelope_rate = envelope[-1];
            } else if (byte >= 0xC0) {          /* which pattern of pitches */
                const uint8_t *pattern = sound_data(mem.text.note_table[96 + byte - 0xC0]);
                voice->pattern_base = pattern;
                voice->pattern = pattern;
            } else {                            /* the tempo */
                mem.text.tempo = (uint8_t)(byte + 0x49);
            }
            continue;
        }
        unsigned command = byte & 0x7F;
        uint8_t mix;
        switch (command) {
        case 0:                                 /* a rest */
            voice->volume = 0;
            goto rest;
        case 15:
            goto rest;
        case 1:
            voice->vibrato_state = 0;
            continue;
        case 8:                                 /* vibrato */
            voice->vibrato_step = sequence[0];
            voice->vibrato_depth = sequence[1];
            voice->vibrato_position = sequence[1];
            sequence += 2;
            /* fall through */
        case 2:
            voice->vibrato_state = 0x40;
            continue;
        case 3:
        case 13:
            voice->state |= 0x02;
            continue;
        case 4:                                 /* a slide */
            voice->slide = 0;
            voice->state |= 0x04;
            voice->slide_step = (int8_t)sequence[0];
            voice->slide_count = sequence[1];
            sequence += 2;
            continue;
        case 6:
            voice->state |= 0x80;
            /* fall through */
        case 5:
            voice->state |= 0x08;
            continue;
        case 7: {                               /* the next sequence of the voice's list */
            uint16_t at = voice->speed;
            uint16_t entry = voice->instrument + at;
            at += 2;
            if (sound_word(sound_data(entry)) == 0) {
                entry = voice->instrument;
                at = 2;
            }
            sequence = sound_data(sound_word(sound_data(entry)));
            voice->speed = at;
            continue;
        }
        case 9:
            mem.text.transpose = *sequence++;
            continue;
        case 10:                                /* the voice makes noise */
            mix = voice->mixer;
            mem.text.mixer_wanted = (uint8_t)(((mix & 0x38) ^ mem.text.mixer_wanted) & mix) ^ mem.text.mixer_wanted;
            voice->flag_1 = 0;
            continue;
        case 11:                                /* the voice makes a tone */
            mix = voice->mixer;
            mem.text.mixer_wanted = (uint8_t)(((mix & 0x07) ^ mem.text.mixer_wanted) & mix) ^ mem.text.mixer_wanted;
            voice->flag_1 = 0xFF;
            continue;
        case 12:
            mem.text.mixer_wanted &= ~voice->mixer;
            voice->flag_1 = 0xFF;
            continue;
        case 14:                                /* the tune is over */
            mem.text.tune_resumes = 0;
            sound_stop();
            return 1;
        case 16:
            voice->flag_2d = 0xFF;
            continue;
        case 17:
            voice->flag_2d = 0;
            continue;
        case 18:
            voice->transpose = *sequence++;
            continue;
        case 19:
            voice->instrument = (uint16_t)(sequence[0] << 8 | sequence[1]);
            sequence += 2;
            voice->speed = 0;
            continue;
        case 20:
            mem.text.tempo = *sequence++;
            continue;
        case 21:
            mem.text.repeat_length = *sequence++;
            mem.text.repeat_count = mem.text.repeat_length;
            continue;
        default:
            unknown_sound_command(byte);
            return 1;
        }
    }
rest:
    voice_goes_on(voice, sequence);
    return 0;
}

/* $74A0: the sound player, once a vertical blank: the effects and the voices go on as the tempo says, and what the sound
 * chip is to play is worked out and given to it. */
static void sound_tick(void)
{
    if (!(mem.text.call_started & 0x01)) {
        mem.text.call_started |= 0x01;
        mem.text.call_rate = 0;
        if (!(tos_mfp_gpip() & 0x80))
            mem.text.call_rate = 0x48;
        else if (!(tos_video_sync() & 0x02))
            mem.text.call_rate = 0x2B;
    }
    if (!mem.text.tune_playing && !(mem.text.effect_on[0] | mem.text.effect_on[1] | mem.text.effect_on[2] | mem.text.effect_on[3]))
        return;
    unsigned phase = mem.text.call_phase + mem.text.call_rate;
    mem.text.call_phase = (uint8_t)phase;
    if (phase & 0x100)
        return;
    effects_step();
    if (mem.text.tune_playing) {
        if (mem.text.repeat_length) {
            if (mem.text.fade_level == 0) {
                sound_start(0);
                return;
            }
            if (--mem.text.repeat_count == 0) {
                if (--mem.text.fade_level == 0) {
                    sound_start(0);
                    return;
                }
                mem.text.repeat_count = mem.text.repeat_length;
            }
        }
        mem.text.noise_period = mem.text.noise_note;
        unsigned beat = mem.text.tempo_phase + mem.text.tempo;
        mem.text.tempo_phase = (uint8_t)beat;
        if (beat & 0x100) {
            for (int v = 0; v < 3; v++)
                if (voice_step(&mem.text.voices[v]))
                    return;
        }
        mem.text.fade_level &= 0x0F;
        for (int v = 0; v < 3; v++) {
            uint8_t volume;
            uint16_t tone = voice_tone(&mem.text.voices[v], &volume);
            mem.text.last_tone = tone;
            mem.text.psg_tone[2 * v] = (uint8_t)tone;
            mem.text.psg_tone[2 * v + 1] = (uint8_t)(tone >> 8);
            uint8_t quieter = mem.text.fade_level ^ 0x0F;
            mem.text.psg_volume[v] = volume >= quieter ? volume - quieter : 0;
        }
        mem.text.psg_noise = mem.text.noise_period;
    }
    for (int c = 0; c < 3; c++) {
        if (!mem.text.effect_on[c])
            continue;
        if (c == 0 && (int8_t)mem.text.effect_on[0] < 0)
            return;
        const struct Effect *fx = &mem.text.effects[c];
        mem.text.psg_tone[2 * c] = (uint8_t)mem.text.effect_period[c];
        mem.text.psg_tone[2 * c + 1] = (uint8_t)(mem.text.effect_period[c] >> 8);
        if (!(fx->flags & 0x08))
            mem.text.psg_noise = mem.text.effect_noise;
        static const uint8_t voice_bits[3] = { 0x09, 0x12, 0x24 };
        uint8_t flags = fx->flags;
        flags = (uint8_t)(flags << c | flags >> (8 - c));
        mem.text.psg_mixer |= voice_bits[c];
        mem.text.psg_mixer &= flags;
        mem.text.psg_volume[c] = mem.text.effect_volume[c];
    }
    mem.text.psg_mixer &= 0x3F;
    uint8_t keep = 0;
    if (!(mem.text.voice_muted[0] | mem.text.voice_muted[1] | mem.text.voice_muted[2] | mem.text.voice_muted[3]))
        ym_write(6, mem.text.psg_noise);
    for (int c = 0; c < 3; c++) {
        if (mem.text.voice_muted[c])
            continue;
        ym_write(2 * c, mem.text.psg_tone[2 * c]);
        ym_write(2 * c + 1, mem.text.psg_tone[2 * c + 1]);
        ym_write(8 + c, mem.text.psg_volume[c]);
        keep |= (uint8_t[]){ 0x09, 0x12, 0x24 }[c];
    }
    uint8_t current = ym_read(7);
    ym_write(7, ((current ^ mem.text.psg_mixer) & keep) ^ current);
}

/* $7332: the sound player is called from the vertical blank. */
void sound_update(void)
{
    sound_tick();
}

/* ---- the program ---- */

/* $0536 */
void initialize_program(void)
{
    mem.vars.super_sp = PROGRAM_STACK_TOP - 10;      /* the stack pointer when Super was called */
    tos_enter_supervisor();
    ikbd_command(0x12);                              /* the mouse is not wanted */
    tos_setscreen();
    save_system_palette();
    set_black_palette();
    mem.vars.font = TEXT_ADDRESS(0x8874);
    mem.vars.sprites = mem.vars.font + 0x2D80;
    mem.vars.level_start = tos_malloc(0x222E0);
    mem.vars.back_buf = tos_malloc(0x9000);
    mem.vars.backgrounds_a = tos_malloc(0x4E20);
    mem.vars.backgrounds_b = tos_malloc(0x2710);
    mem.vars.free_memory = tos_malloc(0xFFFFFFFF);
    mem.vars.scratch_ptr = ADDR_BSS + 0x2AE;
    mem.vars.screen_ptr = tos_physbase();
    build_bit_reversed();
    build_record_lists();
    load_picture(mem.text.loader_name);
    set_palette(mem.text.palettes[3]);
    /* $4A4E: the copy protection, which reads the disk's timing from the floppy controller, passes. */
    load_level_data();
    install_joystick_handler();
    load_file(mem.text.status_name, BACK_AT(0), 0x23A0);
}

/* ---- the rooms ---- */

/* The entity at `slot` comes into play: where it is, which picture (and animation, and table of pictures) it has. The
 * slot after it is returned. */
static struct Entity *spawn(struct Entity *slot, uint16_t x, uint16_t y, uint16_t picture, const uint16_t *image,
                            union EntityTable table)
{
    slot->x = x;
    slot->y = y;
    slot->frame = 0;
    slot->flags = 2;
    slot->field_8 = picture;
    slot->image = image;
    slot->table = table;
    return slot + 1;
}

#define FIRST_ROOM_ENTITY (&mem.text.entities[1])

/* $22F6 */
static void init_22f6(void)
{
    mem.text.blinking_picture.field_8 = 1;
}

/* $230E */
static void init_230e(void)
{
    if (mem.vars.event_flags & 0x01)
        draw_entity_pictures(&mem.text.trapdoor_picture);
}

/* $232E */
static void init_232e(void)
{
    if (mem.vars.event_flags & 0x02)
        draw_sprite(&mem.text.picture_2348, mem.text.no_offset, 0x31);
}

/* $2356 */
static void init_2356(void)
{
    if (mem.vars.progress_flags & 0x20)
        draw_entity_pictures(&mem.text.picture_2368);
}

/* $2374, $23FC, $26CA, $2744: things that stand on the ground. */
static void init_2374(struct Entity *slot)
{
    spawn(slot, 0x10, 0x90, 0, 0, NO_TABLE);
}

/* $239E */
static void init_239e(struct Entity *slot)
{
    spawn(slot, 0x100, 0x90, 0, 0, NO_TABLE);
    spawn(FIRST_ROOM_ENTITY, 0x30, 0x30, 0x0B, 0, NO_TABLE);
}

/* $23FC */
static void init_23fc(struct Entity *slot)
{
    spawn(slot, 0x150, 0x90, 0, 0, NO_TABLE);
}

/* $2426 */
static void init_2426(struct Entity *slot)
{
    mem.vars.creature_entity = slot;
    struct Entity *next = spawn(slot, 0, 0x78, 0, TEXT_AT(const uint16_t, 0xF32), PICTURES(mem.text.picture_tables + (0x1370 - 0x12E0) / 4));
    if (mem.vars.progress_flags & 0x01)
        next[-1].image = TEXT_AT(const uint16_t, 0xF46);
    mem.vars.bubble_entity = next;
    next = spawn(next, 0x40, 0x90, 0x19, 0, NO_TABLE);
    if (mem.vars.progress_flags & 0x08)
        next[-1].field_8 = 0x1A;
    mem.vars.blocker_x = 0x34;
    post_message(0x42);
    sound_start(2);
    mem.vars.control_flags |= 0x04;
}

/* $24C6 */
static void init_24c6(struct Entity *slot)
{
    int16_t count = mem.vars.points_collected;
    if (count >= 0x41) {
        slot = spawn(slot, 0x50, 0x78, 0, TEXT_AT(const uint16_t, 0x1254), NO_TABLE);
        uint16_t message = 0x4F;
        if (count >= 0x5A) {
            slot = spawn(slot, 0x110, 0x78, 0, TEXT_AT(const uint16_t, 0x1236), NO_TABLE);
            message++;
            if (count >= 0x6E) {
                slot = spawn(slot, 0, 0x78, 0, TEXT_AT(const uint16_t, 0x11F4), NO_TABLE);
                message++;
            }
        }
        post_message(message);
        start_bubble(9, 0x1E);
    }
    slot = spawn(FIRST_ROOM_ENTITY, 0xB0, 0x50, 0x21, 0, NO_TABLE);
    spawn(slot, 0xB0, 0x74, 0x24, 0, CYCLE(0x28, 0x24));
    mem.vars.points_collected = 0;
}

/* $25F6 */
static void init_25f6(void)
{
    struct Entity *slot = spawn(FIRST_ROOM_ENTITY, 0x100, 0x78, 0x2B, 0, CYCLE(0x2D, 0x28));
    slot = spawn(slot, 0xE0, 0x78, 0x28, 0, CYCLE(0x2E, 0x28));
    spawn(slot, 0x80, 0x40, 0x24, 0, CYCLE(0x28, 0x24));
}

/* $268C */
static void init_268c(void)
{
    spawn(FIRST_ROOM_ENTITY, 0x58, 0x70, 0x2E, 0, CYCLE(0x33, 0x2E));
}

/* $26CA */
static void init_26ca(struct Entity *slot)
{
    slot = spawn(slot, 0x80, 0x90, 0, 0, NO_TABLE);
    slot = spawn(slot, 0x90, 0x90, 0, 0, NO_TABLE);
    spawn(slot, 0x140, 0x90, 0, 0, NO_TABLE);
}

/* $2744 */
static void init_2744(struct Entity *slot)
{
    spawn(slot, 0x60, 0x90, 0, 0, NO_TABLE);
    spawn(FIRST_ROOM_ENTITY, 0xB0, 0x30, 7, 0, NO_TABLE);
}

/* $27A2 */
static void init_27a2(void)
{
    if (mem.vars.progress_flags & 0x20)
        draw_entity_pictures(&mem.text.picture_27e8);
    spawn(FIRST_ROOM_ENTITY, 0x130, 0x30, 0x0C, 0, NO_TABLE);
}

/* $27F4 */
static void init_27f4(struct Entity *slot)
{
    slot = spawn(slot, 0x60, 0x80, 0x1C, 0, NO_TABLE);
    spawn(slot, 0xC0, 0x90, 0, 0, NO_TABLE);
    mem.vars.carrier_entity = FIRST_ROOM_ENTITY;
    spawn(FIRST_ROOM_ENTITY, 0, 0x30, 0, TEXT_AT(const uint16_t, 0xF66), PICTURES(mem.text.picture_tables + (0x1370 - 0x12E0) / 4));
    mem.vars.progress_flags &= ~0x10;
    mem.vars.event_flags &= ~0x10;
    mem.text.picture_318c.field_8 = 1;
}

/* A single thing, in the first place of the room's. */
static void spawn_single(uint16_t x, uint16_t y, uint16_t picture)
{
    spawn(FIRST_ROOM_ENTITY, x, y, picture, 0, NO_TABLE);
}

/* $2908 */
static void init_2908(void)
{
    struct Entity *slot = spawn(FIRST_ROOM_ENTITY, 0x70, 0x30, 0x0D, 0, NO_TABLE);
    spawn(slot, 0x20, 0x30, 0x11, 0, NO_TABLE);
}

/* $2A1C */
static void init_2a1c(struct Entity *slot)
{
    mem.vars.room_timer = 0x40;
    mem.vars.partner_entity = slot;
    struct Entity *next = spawn(slot, 0x120, 0x78, 0, TEXT_AT(const uint16_t, 0xFB0), PICTURES(mem.text.picture_tables + (0x133C - 0x12E0) / 4));
    struct Entity *player = &mem.text.entities[PLAYER];
    player->y = 0x78;
    if (mem.vars.player_item == 1) {
        mem.vars.ending_countdown = 0x46;
        player->image = TEXT_AT(const uint16_t, 0xCE0);
        mem.vars.partner_entity->image = TEXT_AT(const uint16_t, 0x1018);
        mem.vars.progress_flags |= 0x80;
        post_message(0x41);
        return;
    }
    player->image = TEXT_AT(const uint16_t, 0xEF2);
    spawn(next, 0xC0, 0x60, 0x7C, 0, NO_TABLE);
    post_message(0x46);
}

/* $323A: one picture of an animated thing: the table says which, by the frame. */
static void draw_animated(struct AnimatedPlacement *thing, const uint16_t *pictures, uint16_t offset)
{
    draw_sprite(&thing->at, mem.text.no_offset, pictures[offset / 2]);
}

/* $2AE4 */
static void init_2ae4(void)
{
    post_message(0x44);
    if ((mem.text.vbl_counter & 3) == 0) {
        post_message(0x4B);
        start_bubble(3, 0x1E);
    }
    struct AnimatedPlacement *thing = &mem.text.animated_2b64;
    thing->at.frame = 0;
    thing->at.field_8 = 0x28;
    draw_animated(thing, &mem.text.room_pictures[8], thing->at.frame);
    if (mem.vars.progress_flags & 0x04)
        return;
    mem.vars.hidden_entity = FIRST_ROOM_ENTITY;
    spawn(FIRST_ROOM_ENTITY, 0xF6, 0x44, 0x64, 0, NO_TABLE);
}

/* $2B72 */
static void init_2b72(void)
{
    mem.vars.room_timer = 0x28;
    struct AnimatedPlacement *thing = &mem.text.animated_2b96;
    thing->image = mem.text.sequences;
    thing->at.frame = 0;
    thing->at.field_8 = 0x0A;
    draw_animated(thing, &mem.text.room_pictures[4], thing->at.frame);
}

/* $2BA4 */
static void init_2ba4(void)
{
    post_message(0x44);
    struct AnimatedPlacement *thing = &mem.text.animated_2bc0;
    thing->at.frame = 0;
    thing->at.field_8 = 0x32;
    draw_animated(thing, &mem.text.room_pictures[16], thing->at.frame);
}

/* $2CFA: the five pictures of the frieze, all at one frame (a byte offset into the table of pictures). */
static void draw_frieze(uint16_t frame)
{
    for (int i = 0; i < 5; i++) {
        struct Quad *quad = &mem.text.frieze_pictures[i];
        quad->frame = frame;
        draw_sprite((const struct Placement *)quad, mem.text.no_offset, mem.text.room_pictures[(frame + 8 * i) / 2]);
    }
}

/* $2DC2: the picture that blinks above the frieze. */
static void draw_frieze_blinker(void)
{
    struct Placement *picture = &mem.text.frieze_blinker;
    draw_sprite(picture, mem.text.no_offset, picture->frame + 7);
}

/* $2C82 */
static void init_2c82(struct Entity *slot)
{
    mem.vars.points_collected = 0;
    mem.vars.frieze_entity = slot;
    spawn(slot, 0x168, 0x62, 0x1B, 0, NO_TABLE);
    struct Entity *player = &mem.text.entities[PLAYER];
    mem.text.rooms[1].w[0] = 0x80;
    player->image = entity_image(player, 0x13);
    mem.vars.room_timer = 0x80;
    post_message(0x24);
    mem.vars.fatigue = 0;
    mem.vars.display_flags |= 0x08;
    mem.text.frieze_blinker.field_8 = 1;
    mem.text.frieze_blinker.frame = 0;
    draw_frieze_blinker();
    draw_frieze(6);
}

/* The routines of a room that say how a player may leave it, and what happens when he does. */
static void routine_332a(void);

void run_room_init(uint32_t address)
{
    struct Entity *slot = &mem.text.entities[21];
    switch (address - TEXT_ADDRESS(0)) {
    case 0x22F4: case 0x2E16: return;
    case 0x22F6: init_22f6(); return;
    case 0x230E: init_230e(); return;
    case 0x232E: init_232e(); return;
    case 0x2356: init_2356(); return;
    case 0x2374: init_2374(slot); return;
    case 0x239E: init_239e(slot); return;
    case 0x23FC: init_23fc(slot); return;
    case 0x2426: init_2426(slot); return;
    case 0x24C6: init_24c6(slot); return;
    case 0x25F6: init_25f6(); return;
    case 0x268C: init_268c(); return;
    case 0x26C2: mem.vars.blocker_x = 0x34; return;
    case 0x26CA: init_26ca(slot); return;
    case 0x2744: init_2744(slot); return;
    case 0x27A2: init_27a2(); return;
    case 0x27F4: init_27f4(slot); return;
    case 0x289C: spawn_single(0xBC, 0x30, 0x0A); return;
    case 0x28D2: spawn_single(0xB0, 0x2F, 0x09); return;
    case 0x2908: init_2908(); return;
    case 0x296E: spawn_single(0x100, 0x30, 0x0E); return;
    case 0x29A4: spawn_single(0xA0, 0x30, 0x0F); return;
    case 0x29DA: spawn_single(0x120, 0x30, 0x08); return;
    case 0x2A1C: init_2a1c(slot); return;
    case 0x2C82: init_2c82(slot); return;
    case 0x332A: routine_332a(); return;
    case 0x2AE4: init_2ae4(); return;
    case 0x2B72: init_2b72(); return;
    case 0x2BA4: init_2ba4(); return;
    case 0x2BCA: case 0x2C72: mem.vars.blocker_x = 0xEC; return;
    case 0x2BD2: case 0x2BDA: case 0x2C62: case 0x2C6A: case 0x2C7A: mem.vars.blocker_x = 0x4C; return;
    default: unknown_room_routine(address); return;
    }
}

/* $2E64: how long a picture stays: from 16 frames in steps of 4, by chance. Returns what was calculated. */
static uint16_t blink_interval(struct Placement *picture)
{
    uint16_t interval = ((mem.text.vbl_counter & 7) << 2) + 0x10;
    picture->field_8 = interval;
    return interval;
}

/* $2E18: a picture of the room blinks. */
static void state_2e18(void)
{
    struct Placement *picture = &mem.text.blinking_picture;
    if (--picture->field_8 != 0)
        return;
    uint16_t interval = blink_interval(picture);
    uint16_t number;
    int open = (picture->frame & 1) != 0;
    picture->frame ^= 1;
    if (!open) {
        picture->field_8 = 5;
        if (!(mem.vars.display_flags & 0x02) && (interval & 0x0C) == 0) {
            post_message(2);
            picture->field_8 = 0x0F;
            number = 2;
            draw_sprite(picture, mem.text.no_offset, number + 7);
            return;
        }
    }
    draw_sprite(picture, mem.text.no_offset, picture->frame + 7);
}

/* $2E7C */
static void state_2e7c(void)
{
    if (mem.vars.event_flags & 0x08) {
        mem.vars.event_flags &= ~0x08;
        draw_entity_pictures(&mem.text.trapdoor_picture);
    }
}

/* $2E8A */
static void state_2e8a(void)
{
    if (!(mem.vars.display_flags & 0x20))
        return;
    struct Entity *player = &mem.text.entities[PLAYER];
    if (player->x < 0x38 || player->x >= 0x50 || player->y == 0x78)
        return;
    mem.vars.event_flags |= 0x02;
    init_232e();
    add_score_hundred();
    post_message(4);
    start_bubble(9, 0x0A);
}

/* $2EF4: an entity near the left edge looks round at what is there. */
static void look_at_left_edge(struct Entity *entity)
{
    entity->flags &= ~0x0010;
    if (entity->x >= 0x40)
        return;
    entity->flags |= 0x0010;
    entity->image = entity_image(entity, 9);
    sound_effect(4);
}

/* $2ED0 */
static void state_2ed0(void)
{
    if (mem.vars.mode_flags & 0x01) {
        mem.vars.mode_flags &= ~0x01;
        draw_entity_pictures(&mem.text.picture_2368);
        return;
    }
    if (mem.vars.progress_flags & 0x20)
        return;
    look_at_left_edge(&mem.text.entities[PLAYER]);
    struct Entity *creature = &mem.text.entities[6];
    if (creature->frame != 9)
        return;
    look_at_left_edge(creature);
}

/* $2F18 */
static void state_2f18(void)
{
    struct Entity *creature = mem.vars.creature_entity;
    struct Entity *player = &mem.text.entities[PLAYER];
    if (creature->edge != 0 || creature->x >= 0x100) {
        creature->edge = 0;
        creature->flags ^= 0x0001;
        if (mem.vars.progress_flags & 0x01) {
            creature->image = entity_image(creature, 1);
            creature->x += 0x10;
        }
    }
    if (!(mem.vars.progress_flags & 0x01)) {
        struct Entity *object;
        int16_t number = find_reachable_object(creature, &object);
        if (number >= 0) {
            take_object(object, number);
            if (number == 0x0B) {
                creature->flags |= 0x0001;
                mem.vars.progress_flags |= 0x01;
                post_message(0x43);
            }
        }
    }
    if (!(mem.vars.control_flags & 0x01) && !(mem.vars.progress_flags & 0x01) && entities_near(player, creature) &&
        !(creature->flags & 1))
        player->x = creature->x + 0x20;
    if ((mem.vars.progress_flags & 0x01) && !(mem.vars.progress_flags & 0x08) && (mem.vars.display_flags & 0x20) && player->x < 0x80 &&
        player->x >= 0x40) {
        mem.vars.progress_flags |= 0x08;
        place_new_object(1, 0x40, 0x8C);
        mem.vars.bubble_entity->field_8 = 0x1A;
        sound_effect(6);
    }
}

/* $301E: a picture changes to the next, and when it has come to the end of the table it goes back to where it began. */
static void step_picture(struct Entity *entity)
{
    entity->field_8++;
    if (entity->field_8 == (uint16_t)(entity->table.cycle >> 16))
        entity->field_8 = (uint16_t)entity->table.cycle;
}

/* $3034: the picture of a thing that wakes now and then. */
static void animate_sleeper(struct Entity *entity)
{
    uint16_t picture = 0x21;
    uint16_t counter = entity->table.cycle >> 16;
    if ((int16_t)counter < 0) {
        mem.vars.sleeper_seed = (mem.vars.sleeper_seed + mem.text.vbl_counter) & 0x7F;
        if (mem.vars.sleeper_seed < 5) {
            entity->table.cycle = (entity->table.cycle & 0xFFFF) | (uint32_t)((mem.vars.sleeper_seed & 7) + 3) << 16;
            picture++;
        }
    } else {
        picture++;
        counter--;
        entity->table.cycle = (entity->table.cycle & 0xFFFF) | (uint32_t)counter << 16;
        if (!((int16_t)counter < 0))
            picture++;
    }
    entity->field_8 = picture;
}

/* $2FF4 */
static void state_2ff4(void)
{
    animate_sleeper(FIRST_ROOM_ENTITY);
    step_picture(&mem.text.entities[2]);
}

/* $3002 */
static void state_3002(void)
{
    for (int i = 1; i <= 3; i++)
        step_picture(&mem.text.entities[i]);
}

/* $3016 */
static void state_3016(void)
{
    step_picture(FIRST_ROOM_ENTITY);
}

/* $3094: an entity far to the right looks round. */
static void look_at_right_edge(struct Entity *entity)
{
    if (entity->x < 0x120)
        return;
    entity->image = entity_image(entity, 5);
    sound_effect(4);
}

/* $307A */
static void state_307a(void)
{
    if (mem.vars.progress_flags & 0x20)
        return;
    look_at_right_edge(&mem.text.entities[PLAYER]);
    struct Entity *creature = &mem.text.entities[6];
    if (creature->frame != 0x1E)
        return;
    look_at_right_edge(creature);
}

/* $30AC */
static void state_30ac(void)
{
    struct Placement *picture = &mem.text.picture_318c;
    struct Entity *creature = mem.vars.carrier_entity;
    uint16_t x = creature->x;
    uint16_t number = 2;
    if (x >= 0x9B && x < 0xB4) {
        draw_sprite(picture, mem.text.no_offset, number);
    } else {
        if (--picture->field_8 == 0) {
            blink_interval(picture);
            number = 0;
            picture->frame++;
            if ((int16_t)picture->frame >= 2) {
                number = 1;
                picture->field_8 = 5;
                picture->frame = 0;
            }
            draw_sprite(picture, mem.text.no_offset, number);
        }
    }
    struct Entity *player = &mem.text.entities[PLAYER];
    if (creature->edge) {
        creature->edge = 0;
        creature->flags ^= 0x0001;
        if (creature->y != 0x30) {
            creature->y = 0x30;
            creature->image = TEXT_AT(const uint16_t, 0xF68);
        }
        if (mem.vars.progress_flags & 0x10) {
            mem.vars.progress_flags &= ~0x10;
            creature->y = 0x60;
        }
    }
    if (!(mem.vars.event_flags & 0x10)) {
        if (!entities_near(creature, player))
            return;
        player->image = TEXT_AT(const uint16_t, 0xD3A);
        mem.vars.event_flags |= 0x10;
        if (creature->y >= 0x5C) {
            creature->image = TEXT_AT(const uint16_t, 0xF8C);
            mem.vars.dropped_object->x = 0xFFFF;
        }
    }
    player->x = creature->x;
    player->y = creature->y;
    player->y += 0x10;
    player->x += 8;
    if (!(creature->flags & 1))
        player->x -= 0x10;
}

/* $319C */
static void state_319c(void)
{
    if (mem.vars.progress_flags & 0x80) {
        mem.vars.ending_countdown--;
        return;
    }
    struct Entity *player = &mem.text.entities[PLAYER];
    player->y = 0x60;
    if (--mem.vars.room_timer == 0)
        player->image = entity_image(player, 6);
}

/* $31F2: an animated thing waits for the player to come near, and then plays its pictures. */
static void animate_thing(struct AnimatedPlacement *thing, const uint16_t *pictures)
{
    struct Placement *at = &thing->at;
    if (!(mem.vars.mode_flags & 0x40)) {
        if (mem.text.entities[PLAYER].x < 0x40) {
            at->field_8 = 0x0A;
            at->frame = 0;
            draw_animated(thing, pictures, at->frame);
            return;
        }
    }
    if (--at->field_8 != 0) {
        if (!(mem.vars.mode_flags & 0x40))
            return;
        at->frame = (at->frame + 1) & 7;
        const uint16_t *sequence = thing->image;
        draw_sprite(at, mem.text.no_offset, pictures[sequence[at->frame] / 2]);
        return;
    }
    mem.vars.mode_flags &= ~0x40;
    at->field_8 = 5;
    at->frame += 4;
    if (at->frame != 6) {
        blink_interval(at);
        at->frame = 2;
    }
    draw_animated(thing, pictures, at->frame);
}

/* $31EA */
static void state_31ea(void)
{
    animate_thing(&mem.text.animated_2b64, &mem.text.room_pictures[8]);
}

/* $328E */
static void state_328e(void)
{
    if (!((int16_t)mem.vars.room_timer < 0)) {
        mem.vars.room_timer--;
        if ((int16_t)mem.vars.room_timer < 0) {
            mem.vars.progress_flags |= 0x40;
            drop_item(&mem.vars.player_item, &mem.text.entities[PLAYER]);
            post_message(0x23);
            mem.vars.mode_flags |= 0x40;
            mem.text.animated_2b96.at.frame = 0;
            mem.text.animated_2b96.at.field_8 = 0x14;
        }
    }
    animate_thing(&mem.text.animated_2b96, &mem.text.room_pictures[4]);
}

/* $32CA */
static void state_32ca(void)
{
    animate_thing(&mem.text.animated_2bc0, &mem.text.room_pictures[16]);
}

/* $2D60: the frieze of the room: one of its pictures changes now and then, and the blinker above it; when the time is up
 * the frieze falls apart. */
static void animate_frieze(void)
{
    if ((int16_t)mem.vars.room_timer < 0) {
        struct Entity *entity = mem.vars.frieze_entity;
        if (entity->flags & 0x0002) {
            if (mem.vars.mode_flags & 0x10)
                return;
            mem.vars.mode_flags |= 0x10;
            draw_frieze(4);
            return;
        }
        if (!(mem.vars.mode_flags & 0x80)) {
            mem.vars.mode_flags |= 0x80;
            draw_frieze(2);
            return;
        }
        if (mem.vars.mode_flags & 0x20)
            return;
        mem.vars.mode_flags |= 0x20;
        draw_frieze(0);
        mem.text.frieze_blinker.frame = 2;
        draw_frieze_blinker();
        return;
    }
    uint16_t n = mem.vars.frieze_step + 1;
    if (n == 5)
        n = 0;
    mem.vars.frieze_step = n;
    struct Quad *quad = &mem.text.frieze_pictures[n];
    int showing = (quad->frame & 4) != 0;
    quad->frame ^= 4;
    if (!showing && (mem.text.vbl_counter & 3) != 0)
        return;
    draw_sprite((const struct Placement *)quad, mem.text.no_offset, mem.text.room_pictures[(n * 8 + quad->frame) / 2]);
    struct Placement *picture = &mem.text.frieze_blinker;
    if (--picture->field_8 != 0)
        return;
    blink_interval(picture);
    int was_set = (picture->frame & 1) != 0;
    picture->frame ^= 1;
    if (!was_set)
        picture->field_8 = 5;
    draw_frieze_blinker();
}

/* $32E0 */
static void state_32e0(void)
{
    animate_frieze();
    if (!((int16_t)mem.vars.room_timer < 0)) {
        mem.vars.room_timer--;
        if ((int16_t)mem.vars.room_timer < 0)
            post_message(0x25);
        return;
    }
    struct Entity *entity = mem.vars.frieze_entity;
    if (!(entity->flags & 0x0002))
        return;
    entity->x -= 0x10;
    if (entity->x >= 0xC8)
        return;
    entity->flags &= ~0x0002;
    struct Entity *player = &mem.text.entities[PLAYER];
    player->image = entity_image(player, 0x14);
    post_message(0x26);
}

/* $332A: the object the player is near, when he is facing the right way and standing in the right place, is picked up and
 * thrown: the entity of the object is set to fly (move_thrown_object) or to be hit at once. */
static void routine_332a(void)
{
    if (!(mem.vars.display_flags & 0x20))
        return;
    struct Entity *player = &mem.text.entities[PLAYER];
    struct Entity *found;
    if (find_reachable_object(player, &found) != 0x1C)
        return;
    if (found->flags & 0x0080)
        return;
    mem.vars.thrown_object = object_record(0x1C);
    if (!(player->flags & 1)) {
        uint16_t from_left = player->x - 0x48;
        if (player->x >= 0x48 && from_left < 0x60) {
            found->image = TEXT_AT(const uint16_t, 0x115E);
            found->flags &= ~0x0001;
            found->flags |= 0x0080;
            post_message(0x29);
            return;
        }
    }
    mem.vars.mode_flags |= 0x04;
    mem.vars.thrown_ticks = 0x10;
    mem.vars.thrown_entity = found;
    found->flags = (found->flags & ~0x0001) | (player->flags & 1);
}

/* The routine a room runs every frame. */
void run_room_state(uint32_t address)
{
    switch (address - TEXT_ADDRESS(0)) {
    case 0x2E16: return;
    case 0x2E18: state_2e18(); return;
    case 0x2E7C: state_2e7c(); return;
    case 0x2E8A: state_2e8a(); return;
    case 0x2ED0: state_2ed0(); return;
    case 0x2F18: state_2f18(); return;
    case 0x2FF4: state_2ff4(); return;
    case 0x3002: state_3002(); return;
    case 0x3016: state_3016(); return;
    case 0x307A: state_307a(); return;
    case 0x30AC: state_30ac(); return;
    case 0x319C: state_319c(); return;
    case 0x31EA: state_31ea(); return;
    case 0x328E: state_328e(); return;
    case 0x32CA: state_32ca(); return;
    case 0x32E0: state_32e0(); return;
    case 0x332A: routine_332a(); return;
    default: unknown_room_routine(address); return;
    }
}

/* $00A0: the intro: the player in the first room, nothing carried. */
static void start_intro(void)
{
    struct Entity *player = &mem.text.entities[PLAYER];
    player->image = entity_image(player, 0);
    player->x = 0xB0;
    player->y = 0x78;
    draw_status_panel();
    mem.vars.room = 6;
    draw_room();
    mem.vars.player_item = 0xFFFF;
    mem.vars.first_creature_item = 0xFFFF;
    mem.vars.second_creature_item = 0xFFFF;
    draw_status_panel();
    mem.vars.display_flags |= 0x02;
    mem.vars.display_flags |= 0x40;
    mem.vars.mode_flags |= 0x08;
    mem.vars.idle_step = 0;
}

/* $008C: after a game, the screen fades out and the intro starts again. */
static void restart_game(void)
{
    start_fade();
    wait_fade();
    if (!(mem.vars.control_flags & 0x08)) {
        mem.vars.control_flags |= 0x08;
        sound_start(0);
    }
    start_intro();
}

/* $6E4C: what is on the screen goes into the back buffer, the planes present_back_buffer takes it from. */
void copy_screen_to_back_buffer(void)
{
    const uint16_t *screen = SCREEN_AT(0);
    uint16_t *plane[4];
    for (int p = 0; p < 4; p++)
        plane[p] = BACK_AT(0x600) + p * (0x1E00 / 2);
    for (int row = 0; row < 129; row++) {
        for (int p = 0; p < 4; p++)
            plane[p] += row == 0 ? 2 : 4;
        for (int group = 0; group < 20; group++)
            for (int p = 0; p < 4; p++)
                *plane[p]++ = *screen++;
    }
}

/* $6DBA: the screen is wiped away, a few columns of pixels more each time. */
static void wipe_screen(void)
{
    copy_screen_to_back_buffer();
    mem.vars.wipe_mask = 0;
    uint16_t keep;
    do {
        present_back_buffer();
        uint16_t previous = mem.vars.wipe_mask;
        uint16_t mask = ((previous & 0xFF00) >> 1) | 0x8000;
        mask |= (uint16_t)(((previous << 1) & 0xFF) + 1);
        mem.vars.wipe_mask = mask;
        keep = ~mask;
        uint16_t *plane[4];
        for (int p = 0; p < 4; p++)
            plane[p] = BACK_AT(0x600) + p * (0x1E00 / 2);
        for (int row = 0; row < 128; row++) {
            for (int p = 0; p < 4; p++)
                plane[p] += row == 0 ? 2 : 4;
            for (int group = 0; group < 20; group++)
                for (int p = 0; p < 4; p++)
                    *plane[p]++ &= keep;
        }
        present_back_buffer();
        wait_vbl_tick();
    } while (keep != 0);
}

/* $4696: the pictures of the end, moved by (dx, dy), are drawn one after the other, each shown at once. The placement they
 * are drawn from is the sprite entity only for the first: present_back_buffer leaves a3 at the end of the third plane, and
 * the original goes on writing there. */
static void draw_end_pictures(int16_t dx, int16_t dy)
{
    struct Placement *where = (struct Placement *)&mem.text.sprite_entity;
    for (uint16_t *record = mem.text.end_pictures; !(record[0] & 0x8000); record += 3) {
        uint16_t x = record[0], y = record[1];
        record[0] += dx;
        record[1] += dy;
        where->x = record[0];
        where->y = record[1];
        where->field_8 = record[2];
        draw_entity_pictures(where);
        record[0] = x;
        record[1] = y;
        present_back_buffer();
        where = (struct Placement *)BACK_AT(0x600 + 4 + 127 * 8 + 128 * 40 + 2 * 0x1E00);
        mem.vars.backgrounds_now = mem.vars.backgrounds_a;
        mem.vars.queue_count = 0;
    }
}

/* $467A */
void scatter_end_pictures(void)
{
    draw_end_pictures(0, 0);
    draw_end_pictures(0x10, 0x10);
    draw_end_pictures(-0x10, 0x10);
    draw_end_pictures(-0x10, -0x10);
    draw_end_pictures(0x10, -0x10);
    draw_end_pictures(-0x20, 0x20);
}

/* $022A: the message stays for a while, and then the game starts again. */
static void show_message_and_restart(void)
{
    mem.vars.message_progress = 0x0E;
    update_message();
    for (int i = 0; i < 0xC9; i++)
        wait_vbl_tick();
    restart_game();
}

/* $0190: Garfield has reached Arlene: the two of them walk over the picture, and the game says so. */
static void ending(void)
{
    copy_screen_to_back_buffer();
    scatter_end_pictures();
    struct Entity *player = &mem.text.entities[PLAYER];
    player->image = TEXT_AT(const uint16_t, 0x127E);
    player->x = 0x0A;
    struct Entity *other = mem.vars.partner_entity;
    other->image = TEXT_AT(const uint16_t, 0x12C0);
    other->x = 0xFFE2;
    for (int i = 0; i < 0x79; i++) {
        mem.vars.backgrounds_now = mem.vars.backgrounds_a;
        mem.vars.queue_base = 0x16E;
        mem.vars.queue_count = 0;
        update_entity(player);
        update_entity(&mem.text.entities[9]);
        update_entity(other);
        present_back_buffer();
        mem.vars.queue_base = 0x16E;
        draw_sprite_queue();
    }
    wipe_screen();
    draw_text_line(&mem.text.end_text[0], SCREEN_AT(0x0C80), 16);
    draw_text_line(&mem.text.end_text[0x285 - 0x270], SCREEN_AT(0x1BA8), 16);
    mem.vars.message_number = 0x32;
    show_message_and_restart();
}

/* $0244: the clock has run out, or the player gave up. */
static void fell_asleep(void)
{
    wipe_screen();
    draw_text_line(&mem.text.end_text[0x28E - 0x270], SCREEN_AT(0x0C80), 16);
    draw_text_line(&mem.text.end_text[0x29F - 0x270], SCREEN_AT(0x1B80), 16);
    mem.vars.message_number = 0x50;
    show_message_and_restart();
}

/* $0000 */
void program_entry(void)
{
    mem.vars.saved_sp = PROGRAM_STACK_TOP;
    initialize_program();
    mem.sys.conterm &= ~1;                            /* no key click */
    sound_start(0);
    save_vbl_vector();
    install_vbl_handler();
    wait_vbl_tick();
    start_fade();
    wait_fade();
    /* $005A: the panel's picture, kept in the back buffer, goes to the screen. */
    const uint16_t *panel = BACK_AT(0);
    uint16_t *screen = SCREEN_AT(0x5000);
    for (int i = 0; i < 0x5A1 * 4; i++)
        screen[i] = panel[i];
    mem.vars.state_routine = TEXT_ADDRESS(0x2E16);
    reset_game_state();
    mem.vars.message_number = 0x4E;
    start_intro();
    play_game();
}

/* $02B4: one pass of the main loop. */
void main_loop_iteration(void)
{
    while (sync_arrive(SYNC_FRAME))
        ;
    ikbd_command(0x16);                               /* what the joystick is doing */
    update_message();
    update_clock();
    update_score_display();
    draw_fatigue_bar();
    mem.vars.backgrounds_now = mem.vars.backgrounds_a;
    mem.vars.queue_count = 0;
    joystick_direction();
    update_companion();
    update_second_companion();
    drop_objects();
    move_walking_object();
    shake_object();
    move_thrown_object();
    fall_asleep();
    update_first_entity();
    mem.vars.backgrounds_now = mem.vars.backgrounds_a;
    mem.vars.queue_base = 0x16E;
    mem.vars.queue_count = 0;
    for (int i = 1; i <= 23; i++)
        update_entity(&mem.text.entities[i]);
    if (mem.vars.display_flags & 0x02) {
        int fade = (mem.vars.display_flags & 0x40) != 0;
        if (fade) {
            start_fade();
            wait_fade();
        }
        const uint16_t *colours = mem.heap.level_data + 12 / 2 + (mem.vars.level_fourth - mem.vars.level_start) / 2;
        for (int i = 0; i < 8; i++)
            mem.text.palettes[0][8 + i] = colours[i];
        mem.vars.display_flags &= ~0x40;
        if (fade) {
            start_fade_in();
        } else {
            wait_vbl_tick();
            set_palette(mem.text.palettes[0]);
        }
    }
    present_back_buffer();
    mem.vars.queue_base = 0x16E;
    draw_sprite_queue();
    restart_sound_if_done();
    check_room_exit();
    run_room_state(mem.vars.state_routine);
}

/* $22B2: the key that has been pressed, if any: (scancode << 16) | ascii. */
uint32_t read_key(void)
{
    uint32_t key = tos_raw_input();
    mem.vars.last_key = (uint16_t)key;
    return key;
}

/* $00FA: the game begins: what the player carried over from the intro is cleared. */
static void begin_game(void)
{
    mem.vars.mode_flags &= ~0x08;
    reset_game_state();
    draw_status_panel();
    mem.vars.display_flags |= 0x02;
    mem.vars.display_flags |= 0x40;
}

/* $00EE and $0114: the intro (room 6, until the player leaves it) and then the game: the main loop runs until the player
 * is out of energy, or the game is over, or a key says what to do: space pauses, F1 and F2 switch the sound on and off.
 * That the intro is on is kept in bit 3 of mode_flags, so a saved state is enough to carry on. */
void play_game(void)
{
    for (;;) {
        main_loop_iteration();
        if (mem.vars.mode_flags & 0x08) {
            if (mem.vars.room != 6)
                begin_game();
            continue;
        }
        if (mem.vars.control_flags & 0x02) {
            fell_asleep();
            continue;
        }
        if (mem.vars.ending_countdown & 0x8000) {
            ending();
            continue;
        }
        uint32_t key = read_key();
        if (KEY_ASCII(key) == ASCII_ESCAPE) {
            fell_asleep();
            continue;
        }
        if (KEY_ASCII(key) == ASCII_SPACE) {
            mem.vars.message_progress = 0x0E;
            mem.vars.message_number = 0x4C;
            update_message();
            while (KEY_ASCII(tos_wait_key()) != ASCII_RETURN)
                ;
            post_message(0x4D);
        } else if (KEY_SCAN(key) == SCAN_F1) {
            if (!(mem.vars.control_flags & 0x08)) {
                mem.vars.control_flags |= 0x08;
                sound_start(0);
            }
        } else if (KEY_SCAN(key) == SCAN_F2) {
            mem.vars.control_flags &= ~0x08;
            sound_stop();
        }
    }
}
