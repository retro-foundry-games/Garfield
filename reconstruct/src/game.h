#pragma once
#include <stddef.h>
#include <stdint.h>

#define ST_RAM_SIZE   0x100000u
#define ADDR_BASEPAGE 0x10000u
#define ADDR_TEXT     0x10100u
#define TEXT_SIZE     212980u
#define BSS_SIZE      3250u
#define ADDR_BSS      (ADDR_TEXT + TEXT_SIZE)
#define ADDR_HEAP     0x54400u   /* where the original's four Malloc calls put its buffers */
#define ADDR_SCREEN   0xF8000u

/* The memory of the original, as a struct: every member is at the offset of its address in the original (PAD
 * fills what nothing names), and the struct is the whole 1 MiB. */
#define PAD(n) uint8_t PAD_NAME(__LINE__)[n];
#define PAD_NAME(line) PAD_NAME_(line)
#define PAD_NAME_(line) pad_##line
#define LAYOUT_ASSERT(cond, msg) _Static_assert(cond, msg)

#pragma pack(push, 1)
struct SysVars {                       /* the low memory TOS owns */
    PAD(0x70)
    uint32_t vbl_vector;               /* $070 level 4 autovector */
    PAD(0x466 - 0x74)
    uint32_t frclock;                  /* $466 vertical blanks since the start */
    PAD(0x484 - 0x46A)
    uint8_t conterm;                   /* $484 bit 0: key click */
    PAD(0x600 - 0x485)
    uint32_t kbd_vectors[8];           /* $600 the keyboard chip's handlers; the seventh takes the joystick */
    PAD(0x640 - 0x620)
    uint8_t joy_packet[3];             /* $640 the joystick report handed to that handler */
    PAD(0x800 - 0x643)
};

/* what a room needs (20 bytes: ten words, some of them longs) */
struct RoomRecord {
    uint16_t w[10];                    /* the long at +$0A is the routine that sets the room up, the one at +$0E what the
                                          main loop calls, and the low byte of the word at +$12 has flags */
};

/* the records of the things in play: the player, the creatures, their shots (28 bytes each, 24 from $4E6A) */
/* where something is and which pictures it has: the first five fields of an entity, and all that sprites of the rooms
 * (which are not entities) have. */
struct Placement {
    uint16_t x;                        /* +$00 */
    uint16_t y;                        /* +$02 */
    uint16_t frame;                    /* +$04 */
    uint16_t flags;                    /* +$06 */
    uint16_t field_8;                  /* +$08 */
};

/* where something is: four of the fields of a placement */
struct Quad {
    uint16_t x;                        /* +$00 */
    uint16_t y;                        /* +$02 */
    uint16_t frame;                    /* +$04 */
    uint16_t flags;                    /* +$06 */
};

/* the picture, and the way a picture moves, of the few sprites of a room that animate (the head of an animation) */
struct AnimatedPlacement {
    struct Placement at;
    const uint16_t *image;             /* +$0A the table the frames come from */
};

/* an object of the rooms, as the working copy keeps them: 16 bytes */
struct Object {
    uint16_t x;                        /* +$00 where it lies; the top bit says it has been taken */
    uint16_t y;                        /* +$02 */
    uint16_t picture;                  /* +$04 the picture number, $59 and up */
    uint16_t room;                     /* +$06 the room it is in */
    uint16_t use[2];                   /* +$08 the routine that runs when it is used (a long) */
    uint16_t placed[2];                /* +$0C the routine that runs when it has been put down (a long) */
};
LAYOUT_ASSERT(sizeof(struct Object) == 16, "object");

/* What an entity keeps at +$10: the pictures of its frames, or (for the things of a room that go through a few pictures)
 * the picture it stops at in the high word and the one it starts again from in the low. */
union EntityTable {
    const uint16_t *const *pictures;
    uint32_t cycle;
};
#define PICTURES(array) ((union EntityTable){ .pictures = (array) })
#define NO_TABLE ((union EntityTable){ .cycle = 0 })
#define CYCLE(stop, start) ((union EntityTable){ .cycle = (uint32_t)(stop) << 16 | (start) })

struct Entity {
    uint16_t x;                        /* +$00 */
    uint16_t y;                        /* +$02 */
    uint16_t frame;                    /* +$04 */
    uint16_t flags;                    /* +$06 the low byte's bit 0: facing left */
    uint16_t field_8;                  /* +$08 */
    const uint16_t *image;             /* +$0A the animation record it is at */
    uint16_t fall;                     /* +$0E how long it has been falling: 0 on the ground, up to 9 */
    union EntityTable table;           /* +$10 the table of its pictures, or what a room's thing counts by */
    uint16_t edge;                     /* +$14 which edge of the room it went over: 1 right, 2 left */
    uint16_t count;                    /* +$16 how many times a loop of its animation has run */
    const uint16_t *loop_image;        /* +$18 where the loop starts */
};
LAYOUT_ASSERT(sizeof(struct Entity) == 0x1C, "entity");
#define PLACEMENT(entity) ((const struct Placement *)(entity))
LAYOUT_ASSERT(offsetof(struct Entity, field_8) == offsetof(struct Placement, field_8), "placement");
#define PLAYER 8                       /* the player's entity */

/* a voice of the sound engine (3 of them, 0x30 bytes each) */
struct Voice {
    uint8_t state;                     /* +$00 bit 2: slide, 3: the note moves by a step each tick, 5: envelope on, 7: up, 1, 0: toggles */
    uint8_t flag_1;                    /* +$01 */
    const uint8_t *sequence;           /* +$02 the sequence it plays */
    uint16_t instrument;               /* +$06 offset (from $6D5C) of the list of sequences it takes turns with */
    PAD(2)
    uint16_t speed;                    /* +$0A where in that list it is */
    PAD(2)
    uint16_t slide;                    /* +$0E how far the slide has taken the pitch */
    const uint8_t *pattern_base;       /* +$10 the pattern of pitch offsets it goes through */
    const uint8_t *pattern;            /* +$14 where it is in it */
    int8_t slide_step;                 /* +$18 */
    uint8_t slide_count;               /* +$19 */
    uint8_t envelope_rate;             /* +$1A */
    uint8_t tick;                      /* +$1B ticks until the next note */
    uint8_t length;                    /* +$1C ticks a note lasts */
    uint8_t note;                      /* +$1D */
    uint8_t volume;                    /* +$1E */
    uint8_t envelope_timer;            /* +$1F */
    const uint8_t *envelope;           /* +$20 where it is in the envelope */
    const uint8_t *envelope_base;      /* +$24 */
    uint8_t envelope_level;            /* +$28 */
    uint8_t vibrato_depth;             /* +$29 */
    uint8_t vibrato_step;              /* +$2A */
    uint8_t vibrato_position;          /* +$2B */
    uint8_t vibrato_state;             /* +$2C bit 6: on, 5: going up, 7: restarts every note */
    uint8_t flag_2d;                   /* +$2D */
    uint8_t transpose;                 /* +$2E */
    uint8_t mixer;                     /* +$2F the bits of the mixer that belong to this voice */
};
LAYOUT_ASSERT(sizeof(struct Voice) == 0x30, "voice");

/* a sound effect on one of the three channels of the sound chip: a tone that slides, and a sequence of volumes */
struct Effect {
    uint8_t timer;                     /* +$00 ticks left */
    uint8_t pause;                     /* +$01 */
    uint16_t period;                   /* +$02 the tone it starts at */
    uint16_t slide;                    /* +$04 how much it changes at a time */
    uint8_t flags;                     /* +$06 bit 3: the noise stays as it is */
    uint8_t noisy;                     /* +$07 nonzero: a random amount is added to the tone */
    int8_t direction;                  /* +$08 */
    uint8_t repeats;                   /* +$09 */
    uint8_t volumes;                   /* +$0A which sequence of volumes */
    uint8_t volume_pause;              /* +$0B ticks a volume lasts */
    uint8_t length;                    /* +$0C */
    uint8_t slide_every;               /* +$0D */
    uint8_t step;                      /* +$0E */
    uint8_t volume_timer;              /* +$0F */
    uint8_t slide_timer;               /* +$10 */
    PAD(1)
    const uint8_t *volumes_start;      /* +$12 */
    const uint8_t *volumes_at;         /* +$16 */
};
LAYOUT_ASSERT(sizeof(struct Effect) == 0x1A, "effect");

struct Text {                          /* the code segment: embedded variables and tables, named at their offsets */
    PAD(0x270)
    char end_text[0x2B4 - 0x270];      /* $0270 what is written when the game ends: " See you in Paradise", "  Arlene", "   Garfield fell", " asleep, try again." */
    PAD(0x644 - 0x2B4)
    char loader_name[11];              /* $0644 "loader.neo" */
    PAD(0x67C - 0x644 - 11)
    uint16_t vbl_counter;              /* $067C counts the vertical blanks, 0 to 24 */
    uint16_t vbl_event;                /* $067E $FFFF when the counter wraps */
    PAD(0x6AA - 0x680)
    uint32_t vbl_old_vector;           /* $06AA the operand of the jmp that ends the VBL handler */
    PAD(0x714 - 0x6AE)
    uint8_t joy_hdr;                   /* $0714 */
    uint8_t joy_state;                 /* $0715 IKBD joystick 1 report: bit0 up, 1 down, 2 left, 3 right, 7 fire */
    PAD(0x848 - 0x716)
    uint16_t frame_data[(0x12E0 - 0x848) / 2];   /* $0848 the pictures' frame records */
    const uint16_t *picture_tables[(0x13B8 - 0x12E0) / 4];   /* $12E0 for each of a few kinds of entity, the animation record of every frame */
    PAD(0x1970 - 0x13B8)
    char score_text[5];                /* $1970 ASCII digits, shown on the panel */
    PAD(0x1A5E - 0x1975)
    char clock_text[8];                /* $1A5E ASCII, the clock of the panel, its colons at 2 and 5 */
    PAD(0x1BF8 - 0x1A66)
    const char *message_pointers[82];  /* $1BF8 the messages */
    char message_line[22];             /* $1D40 the message being shown, 20 characters */
    char message_text[0x2288 - 0x1D56];   /* $1D56 the messages */
    PAD(0x2302 - 0x2288)
    struct Placement blinking_picture;     /* $2302 a picture of the room that blinks */
    PAD(0x2320 - 0x2302 - sizeof(struct Placement))
    struct Placement trapdoor_picture;     /* $2320 */
    PAD(0x2348 - 0x2320 - sizeof(struct Placement))
    struct Placement picture_2348;     /* $2348 */
    PAD(0x2368 - 0x2348 - sizeof(struct Placement))
    struct Placement picture_2368;     /* $2368 */
    PAD(0x27E8 - 0x2368 - sizeof(struct Placement))
    struct Placement picture_27e8;     /* $27E8 */
    PAD(0x2B64 - 0x27E8 - sizeof(struct Placement))
    struct AnimatedPlacement animated_2b64;   /* $2B64 */
    PAD(0x2B96 - 0x2B64 - sizeof(struct AnimatedPlacement))
    struct AnimatedPlacement animated_2b96;   /* $2B96 */
    PAD(0x2BC0 - 0x2B96 - sizeof(struct AnimatedPlacement))
    struct AnimatedPlacement animated_2bc0;   /* $2BC0 */
    PAD(0x2D2E - 0x2BC0 - sizeof(struct AnimatedPlacement))
    struct Quad frieze_pictures[5];         /* $2D2E the five pictures of the room's frieze */
    struct Placement frieze_blinker;     /* $2D56 */
    PAD(0x3184 - 0x2D56 - sizeof(struct Placement))
    uint16_t no_offset[4];             /* $3184 sprites drawn at no distance from what they belong to */
    struct Placement picture_318c;     /* $318C */
    PAD(0x326E - 0x318C - sizeof(struct Placement))
    uint16_t sequences[16];            /* $326E the order the pictures of the animated things come in: two tables of 8 */
    PAD(0x33F6 - 0x326E - sizeof(uint16_t[16]))
    int16_t wave[7];                   /* $33F6 how an object thrown rises and falls; the eighth value is the first word of rooms[0] */
    struct RoomRecord rooms[80];       /* $3404 what each room needs: its routines and flags; room 80's record, which the table has no
                                          room for, overlaps the blocks that follow */
    PAD(0x3A48 - 0x3404 - sizeof(struct RoomRecord[80]))
    uint8_t room_blocks[0x480];        /* $3A48 16 bytes a room: four runs of blocks (x, y, count - 1, unused) */
    PAD(0x3F58 - 0x3A48 - 0x480)
    uint16_t records_3f58[(0x43DC - 0x3F58) / 2];   /* $3F58 80 records of (word, long) pairs ended by a negative word */
    PAD(0x4530 - 0x43DC)
    uint8_t room_alias[16];            /* $4530 the picture of the rooms from $3B on */
    PAD(0x4656 - 0x4530 - 16)
    struct Entity sprite_entity;       /* $4656 what the sprites of a room's pictures are drawn for */
    PAD(0x46D0 - 0x4656 - sizeof(struct Entity))
    uint16_t end_pictures[(0x486A - 0x46D0) / 2];   /* $46D0 pictures (x, y, which) that scatter over the screen at the end, ended by a negative word */
    PAD(0x4E6A - 0x46D0 - sizeof(uint16_t[(0x486A - 0x46D0) / 2]))
    struct Entity entities[24];        /* $4E6A */
    PAD(0x510A - 0x4E6A - sizeof(struct Entity[24]))
    uint16_t records_510a[(0x5A9A - 0x510A) / 2];   /* $510A records of a count and 6 bytes for each, ended by a negative count */
    uint16_t room_pictures[(0x5ACA - 0x5A9A) / 2];   /* $5A9A the pictures of the animated things of the rooms: sets of 8 that overlap */
    PAD(0x5B86 - 0x5ACA)
    int16_t fall_speeds[14];           /* $5B86 how far an entity falls in the n'th frame of a fall */
    PAD(0x5F78 - 0x5BA2)
    uint16_t bubble_placement[2];      /* $5F78 where the speech bubble goes from the player */
    PAD(0x6408 - 0x5F7C)
    uint8_t flipped_sprites[0x40];     /* $6408 a bit for each sprite: it has been turned to face left */
    PAD(0x69DA - 0x6448)
    uint16_t object_templates[243];    /* $69DA the objects of every room, as the program starts */
    uint16_t objects[243];             /* $6BC0 the working copy */
    PAD(0x7050 - 0x6BC0 - sizeof(uint16_t[243]))
    uint16_t fade_palette[16];         /* $7050 the colours a fade is at */
    uint16_t fade_output[16];          /* $7070 what a fade in sends to the palette registers */
    PAD(0x7102 - 0x7090)
    uint16_t fade_in_table[14];        /* $7102 how many steps the fade in of a room takes */
    uint8_t fade_in_toggle;            /* $711E */
    int8_t fade_in_steps;              /* $711F steps left, negative when the fade in is over */
    PAD(0x71EA - 0x7120)
    uint8_t fade_toggle;               /* $71EA */
    int8_t fade_steps;                 /* $71EB steps left, negative when the fade is over */
    PAD(0x726C - 0x71EC)
    char backchrs_name[13];            /* $726C "backchrs.xyz" and a carriage return */
    char status_name[11];              /* $7279 "status.xyz" and a carriage return */
    uint16_t palettes[4][16];          /* $7284 the game's, the one TOS had, black, and the loader's */
    uint8_t joy_dir_table[32];         /* $7304 */
    PAD(0x7404 - 0x7324)
    struct Voice voices[3];            /* $7404 */
    uint8_t tempo_reload;                 /* $7494 */
    uint8_t tempo;                 /* $7495 the tempo */
    uint8_t tempo_phase;                 /* $7496 */
    uint8_t noise_note;                 /* $7497 */
    uint8_t noise_period;                 /* $7498 */
    uint8_t mixer_wanted;                 /* $7499 */
    uint8_t call_rate;                 /* $749A how fast the sound player is called: 0, $48 or $2B */
    uint8_t call_phase;                 /* $749B */
    uint8_t call_started;                 /* $749C */
    PAD(0x7B72 - 0x749D)
    uint8_t psg_tone[6];               /* $7B72 what the sound chip is to be given: three tones (low, high) */
    uint8_t psg_noise;                 /* $7B78 */
    uint8_t psg_mixer;                 /* $7B79 */
    uint8_t psg_volume[3];             /* $7B7A */
    PAD(0x7B80 - 0x7B7D)
    uint16_t effect_period[3];         /* $7B80 the tones of the three effect channels */
    uint8_t effect_noise;              /* $7B86 */
    PAD(1)
    uint8_t effect_volume[3];          /* $7B88 */
    PAD(0x7B8E - 0x7B8B)
    uint16_t note_table[(0x7C6C - 0x7B8E) / 2];   /* $7B8E the tone of every note; the offsets of the patterns follow from $7C4E */
    PAD(0x7C9A - 0x7C6C)
    uint8_t tune_playing;                 /* $7C9A nonzero while a tune plays */
    PAD(1)
    uint8_t effect_on[4];              /* $7C9C the three effect channels are in use */
    uint8_t tune_resumes;                 /* $7CA0 */
    uint8_t transpose;                 /* $7CA1 transposes every note */
    uint8_t repeat_length;                 /* $7CA2 */
    uint8_t repeat_count;                 /* $7CA3 */
    uint8_t voice_muted[4];              /* $7CA4 the three voices are silenced */
    uint16_t last_tone;                /* $7CA8 */
    uint8_t fade_level;                 /* $7CAA */
    PAD(1)
    uint16_t sound_sets[12];           /* $7CAC three sets of four words: the tempo, and where each voice's list of sequences is */
    PAD(0x8524 - 0x7CAC - sizeof(uint16_t[12]))
    uint16_t effect_descriptors[8];    /* $8524 where the description of each effect is (from $6D5C) */
    PAD(0x8674 - 0x8524 - sizeof(uint16_t[8]))
    uint16_t effect_volume_lists[8];   /* $8674 where the volumes of each effect are */
    PAD(0x86FC - 0x8674 - sizeof(uint16_t[8]))
    struct Effect effects[3];          /* $86FC */
    PAD(0x8766 - 0x86FC - sizeof(struct Effect[3]))
    uint32_t sound_random;             /* $8766 shifted every time the effects are looked after */
    PAD(0x8776 - 0x876A)
    uint16_t envelopes[16];            /* $8776 where each envelope is (from $6D5C) */
    PAD(0x8874 - 0x8776 - sizeof(uint16_t[16]))
    uint16_t graphics[(TEXT_SIZE - 0x8874) / 2];   /* $8874 the pictures: sprites of 32 rows (512 bytes) and more */
};

/* an entry of the draw queue: one 32-row sprite for the back buffer */
struct DrawEntry {
    uint32_t dest;                     /* address of its top row; bit 31: masked (6 bytes a row per plane, else 4) */
    uint32_t source;                   /* address of its rows */
};

struct GameVars {                      /* the bss: a6 points at its start, so offsets are the a6 displacements */
    uint8_t block_map[0x7E];           /* $000 a bit for every block of the room: 21 rows of 6 bytes */
    PAD(0xA0 - 0x7E)
    uint16_t room;                      /* $0A0 current room number */
    uint16_t previous_room;                    /* $0A2 */
    PAD(0xA6 - 0xA4)
    uint16_t dropped_objects;                    /* $0A6 */
    uint16_t player_item;                    /* $0A8 */
    uint16_t first_creature_item;                    /* $0AA */
    uint16_t second_creature_item;                    /* $0AC */
    uint16_t creature_away_ticks;                    /* $0AE */
    uint16_t message_progress;                    /* $0B0 */
    uint16_t message_number;                    /* $0B2 */
    uint16_t next_message;                    /* $0B4 */
    uint16_t idle_ticks;                    /* $0B6 */
    uint16_t idle_step;                    /* $0B8 */
    uint16_t fatigue;                    /* $0BA */
    uint16_t hand_over_delay;                    /* $0BC */
    uint16_t blocker_x;                    /* $0BE */
    uint32_t level_start;               /* $0C0 the level's data in heap.level_data, after a 12 byte header */
    uint32_t level_second;              /* $0C4 */
    uint32_t level_third;               /* $0C8 */
    uint32_t level_fourth;              /* $0CC */
    uint16_t map_run_length;                    /* $0D0 */
    PAD(0xD4 - 0xD2)
    uint16_t wipe_mask;                    /* $0D4 */
    uint16_t points_collected;                    /* $0D6 */
    PAD(0xDC - 0xD8)
    struct Entity *hidden_entity;                    /* $0DC */
    struct Entity *walking_entity;                    /* $0E0 */
    struct Object *walking_object;                    /* $0E4 */
    struct Entity *frieze_entity;                    /* $0E8 */
    uint16_t walk_steps_left;                    /* $0EC */
    struct Entity *shaking_entity;                    /* $0EE */
    uint16_t shake_phase;                    /* $0F2 */
    struct Entity *creature_entity;                    /* $0F4 */
    struct Entity *bubble_entity;                    /* $0F8 */
    struct Entity *carrier_entity;                    /* $0FC */
    uint16_t room_timer;                   /* $100 */
    struct Entity *partner_entity;                   /* $102 */
    uint16_t ending_countdown;                   /* $106 */
    uint16_t sleeper_seed;                   /* $108 */
    struct Entity *thrown_entity;                   /* $10A */
    struct Object *thrown_object;                   /* $10E */
    uint16_t thrown_ticks;                   /* $112 */
    PAD(0x116 - 0x114)
    uint16_t frieze_step;                   /* $116 */
    struct Object *dropped_object;                   /* $118 */
    uint16_t time_stage;                   /* $11C */
    uint16_t bubble_delay;                  /* $11E */
    uint16_t var_120;                   /* $120 */
    uint32_t state_routine;             /* $122 the routine the main loop calls every frame */
    uint32_t saved_vbl_vector;          /* $126 the vertical blank vector the game replaced */
    PAD(0x12E - 0x12A)
    uint32_t kbdvecs;                   /* $12E the keyboard vectors TOS keeps */
    uint32_t old_joyvec;                /* $132 the vector the game replaced */
    uint16_t last_key;                   /* $136 */
    uint8_t joy_dir;                    /* $138 */
    PAD(0x13A - 0x139)
    uint16_t joy_idle;                  /* $13A frames without input */
    uint32_t screen_ptr;                /* $13C physical screen */
    uint16_t file_handle;               /* $140 */
    uint32_t back_buf;                  /* $142 off-screen buffer: four planes 0x1e00 apart from +0x600 */
    uint32_t scratch_ptr;               /* $146 the sprite scratch area, $2AE */
    uint32_t queue_base;                /* $14A offset of the draw queue from a6 */
    uint32_t backgrounds_a;                /* $14E */
    uint32_t backgrounds_b;                /* $152 */
    uint32_t backgrounds_now;                /* $156 */
    uint16_t queue_count;               /* $15A pending draw entries */
    uint16_t first_entity_queue_count;                   /* $15C */
    uint32_t sprites;               /* $15E */
    uint32_t font;               /* $162 */
    uint32_t saved_sp;                  /* $166 the stack pointer the program starts with */
    uint32_t super_sp;                  /* $16A what Super returned */
    struct DrawEntry draw_queue[40];   /* $16E */
    uint16_t sprite_scratch[384];      /* $2AE a sprite's four planes of 32 rows, three words a row, while it is shifted */
    PAD(0x66C - 0x5AE)
    uint16_t bit_reversed_below;       /* $66C the loop that fills the table runs once more, with $FFFF */
    uint16_t bit_reversed[256];        /* $66E each byte with its bits reversed, in both halves of the word */
    uint32_t picture_lists[180];            /* $86E addresses of the records of records_510a */
    uint32_t room_picture_lists[80];             /* $B3E addresses of the records of records_3f58 */
    uint8_t wake_minute_ones;                   /* $C7E */
    uint8_t wake_minute_tens;                   /* $C7F */
    uint32_t free_memory;              /* $C80 what was left after the allocations */
    uint8_t display_flags;                 /* $C84 */
    uint8_t progress_flags;                 /* $C85 */
    uint8_t event_flags;                 /* $C86 */
    uint8_t mode_flags;                 /* $C87 */
    uint8_t control_flags;                 /* $C88 */
    PAD(BSS_SIZE - 0xC89)
};

struct Screen {                        /* four interleaved bitplanes, 16-pixel groups */
    uint16_t planes[16000];
};

/* the buffers the program allocates at start-up */
struct Heap {
    uint16_t level_data[0x222E0 / 2];  /* $0C0(a6) BACKCHRS.XYZ: tiles and sprites (big-endian words in the file) */
    uint16_t back_buf[0x9000 / 2];     /* $142(a6) the off-screen picture: four planes 0x1E00 bytes apart, 48 bytes a row */
    uint16_t backgrounds_a[0x4E20 / 2];   /* $14E(a6) */
    uint16_t backgrounds_b[0x2710 / 2];   /* $152(a6) */
};

struct StMem {
    struct SysVars sys;
    PAD(ADDR_BASEPAGE - sizeof(struct SysVars))
    uint8_t basepage[0x100];
    struct Text text;
    struct GameVars vars;
    PAD(ADDR_HEAP - ADDR_BSS - BSS_SIZE)
    struct Heap heap;
    PAD(ADDR_SCREEN - ADDR_HEAP - sizeof(struct Heap))
    struct Screen screen;
    PAD(ST_RAM_SIZE - ADDR_SCREEN - sizeof(struct Screen))
};
#pragma pack(pop)

typedef struct SysVars SysVars;
typedef struct Text Text;
typedef struct GameVars GameVars;
typedef struct Screen Screen;
typedef struct StMem StMem;

LAYOUT_ASSERT(sizeof(struct SysVars) == 0x800, "SysVars");

/* where the stack starts: the program shrinks its block to the basepage, 0x400 bytes and its three segments */
#define PROGRAM_STACK_TOP (ADDR_BASEPAGE + 0x400 + TEXT_SIZE + BSS_SIZE)
#define SYNC_TICK  0   /* the places where the original takes interrupts: the wait for a vertical blank, */
#define SYNC_FADE  1   /* the wait for a fade to end, */
#define SYNC_FRAME 2   /* and the top of the main loop */
LAYOUT_ASSERT(sizeof(struct Text) == TEXT_SIZE, "Text");
LAYOUT_ASSERT(sizeof(struct GameVars) == BSS_SIZE, "GameVars");
LAYOUT_ASSERT(offsetof(struct StMem, text) == ADDR_TEXT, "text address");
LAYOUT_ASSERT(offsetof(struct StMem, vars) == ADDR_BSS, "bss address");
LAYOUT_ASSERT(offsetof(struct StMem, heap) == ADDR_HEAP, "heap address");
LAYOUT_ASSERT(offsetof(struct StMem, screen) == ADDR_SCREEN, "screen address");
LAYOUT_ASSERT(sizeof(struct StMem) == ST_RAM_SIZE, "StMem is the whole 1 MiB");

/* The multi-byte members, by the address the original keeps them at (sorted): what the loader swaps to native
 * order and what scripts see by name. M(address, member, element size). Each is checked against its address. */
#define VOICE_MEMBERS(M, i) \
    M(ADDR_TEXT + 0x7404 + 0x30 * (i) + 0x00, text.voices[i].state, 1) \
    M(ADDR_TEXT + 0x7404 + 0x30 * (i) + 0x01, text.voices[i].flag_1, 1) \
    M(ADDR_TEXT + 0x7404 + 0x30 * (i) + 0x02, text.voices[i].sequence, 4) \
    M(ADDR_TEXT + 0x7404 + 0x30 * (i) + 0x06, text.voices[i].instrument, 2) \
    M(ADDR_TEXT + 0x7404 + 0x30 * (i) + 0x0A, text.voices[i].speed, 2) \
    M(ADDR_TEXT + 0x7404 + 0x30 * (i) + 0x0E, text.voices[i].slide, 2) \
    M(ADDR_TEXT + 0x7404 + 0x30 * (i) + 0x10, text.voices[i].pattern_base, 4) \
    M(ADDR_TEXT + 0x7404 + 0x30 * (i) + 0x14, text.voices[i].pattern, 4) \
    M(ADDR_TEXT + 0x7404 + 0x30 * (i) + 0x18, text.voices[i].slide_step, 1) \
    M(ADDR_TEXT + 0x7404 + 0x30 * (i) + 0x19, text.voices[i].slide_count, 1) \
    M(ADDR_TEXT + 0x7404 + 0x30 * (i) + 0x1A, text.voices[i].envelope_rate, 1) \
    M(ADDR_TEXT + 0x7404 + 0x30 * (i) + 0x1B, text.voices[i].tick, 1) \
    M(ADDR_TEXT + 0x7404 + 0x30 * (i) + 0x1C, text.voices[i].length, 1) \
    M(ADDR_TEXT + 0x7404 + 0x30 * (i) + 0x1D, text.voices[i].note, 1) \
    M(ADDR_TEXT + 0x7404 + 0x30 * (i) + 0x1E, text.voices[i].volume, 1) \
    M(ADDR_TEXT + 0x7404 + 0x30 * (i) + 0x1F, text.voices[i].envelope_timer, 1) \
    M(ADDR_TEXT + 0x7404 + 0x30 * (i) + 0x20, text.voices[i].envelope, 4) \
    M(ADDR_TEXT + 0x7404 + 0x30 * (i) + 0x24, text.voices[i].envelope_base, 4) \
    M(ADDR_TEXT + 0x7404 + 0x30 * (i) + 0x28, text.voices[i].envelope_level, 1) \
    M(ADDR_TEXT + 0x7404 + 0x30 * (i) + 0x29, text.voices[i].vibrato_depth, 1) \
    M(ADDR_TEXT + 0x7404 + 0x30 * (i) + 0x2A, text.voices[i].vibrato_step, 1) \
    M(ADDR_TEXT + 0x7404 + 0x30 * (i) + 0x2B, text.voices[i].vibrato_position, 1) \
    M(ADDR_TEXT + 0x7404 + 0x30 * (i) + 0x2C, text.voices[i].vibrato_state, 1) \
    M(ADDR_TEXT + 0x7404 + 0x30 * (i) + 0x2D, text.voices[i].flag_2d, 1) \
    M(ADDR_TEXT + 0x7404 + 0x30 * (i) + 0x2E, text.voices[i].transpose, 1) \
    M(ADDR_TEXT + 0x7404 + 0x30 * (i) + 0x2F, text.voices[i].mixer, 1)

#define EFFECT_MEMBERS(M, i) \
    M(ADDR_TEXT + 0x86FC + 0x1A * (i) + 0x00, text.effects[i].timer, 1) \
    M(ADDR_TEXT + 0x86FC + 0x1A * (i) + 0x01, text.effects[i].pause, 1) \
    M(ADDR_TEXT + 0x86FC + 0x1A * (i) + 0x02, text.effects[i].period, 2) \
    M(ADDR_TEXT + 0x86FC + 0x1A * (i) + 0x04, text.effects[i].slide, 2) \
    M(ADDR_TEXT + 0x86FC + 0x1A * (i) + 0x06, text.effects[i].flags, 1) \
    M(ADDR_TEXT + 0x86FC + 0x1A * (i) + 0x07, text.effects[i].noisy, 1) \
    M(ADDR_TEXT + 0x86FC + 0x1A * (i) + 0x08, text.effects[i].direction, 1) \
    M(ADDR_TEXT + 0x86FC + 0x1A * (i) + 0x09, text.effects[i].repeats, 1) \
    M(ADDR_TEXT + 0x86FC + 0x1A * (i) + 0x0A, text.effects[i].volumes, 1) \
    M(ADDR_TEXT + 0x86FC + 0x1A * (i) + 0x0B, text.effects[i].volume_pause, 1) \
    M(ADDR_TEXT + 0x86FC + 0x1A * (i) + 0x0C, text.effects[i].length, 1) \
    M(ADDR_TEXT + 0x86FC + 0x1A * (i) + 0x0D, text.effects[i].slide_every, 1) \
    M(ADDR_TEXT + 0x86FC + 0x1A * (i) + 0x0E, text.effects[i].step, 1) \
    M(ADDR_TEXT + 0x86FC + 0x1A * (i) + 0x0F, text.effects[i].volume_timer, 1) \
    M(ADDR_TEXT + 0x86FC + 0x1A * (i) + 0x10, text.effects[i].slide_timer, 1) \
    M(ADDR_TEXT + 0x86FC + 0x1A * (i) + 0x12, text.effects[i].volumes_start, 4) \
    M(ADDR_TEXT + 0x86FC + 0x1A * (i) + 0x16, text.effects[i].volumes_at, 4)

#define PLACEMENT_FIELDS(M, base, p) \
    M((base) + 0x00, p.x, 2) \
    M((base) + 0x02, p.y, 2) \
    M((base) + 0x04, p.frame, 2) \
    M((base) + 0x06, p.flags, 2) \
    M((base) + 0x08, p.field_8, 2)

#define QUAD_FIELDS(M, base, q) \
    M((base) + 0x00, q.x, 2) \
    M((base) + 0x02, q.y, 2) \
    M((base) + 0x04, q.frame, 2) \
    M((base) + 0x06, q.flags, 2)

#define ANIMATED_FIELDS(M, base, a) \
    PLACEMENT_FIELDS(M, base, a.at) \
    M((base) + 0x0A, a.image, 4)

#define ENTITY_FIELDS(M, base, e) \
    M((base) + 0x00, e.x, 2) \
    M((base) + 0x02, e.y, 2) \
    M((base) + 0x04, e.frame, 2) \
    M((base) + 0x06, e.flags, 2) \
    M((base) + 0x08, e.field_8, 2) \
    M((base) + 0x0A, e.image, 4) \
    M((base) + 0x0E, e.fall, 2) \
    M((base) + 0x10, e.table, 4) \
    M((base) + 0x14, e.edge, 2) \
    M((base) + 0x16, e.count, 2) \
    M((base) + 0x18, e.loop_image, 4)
#define ENTITY_MEMBERS(M, i) ENTITY_FIELDS(M, ADDR_TEXT + 0x4E6A + 0x1C * (i), text.entities[i])

/* The members that hold addresses in the original are pointers here. The memory image the original keeps has the addresses;
 * mem_from_st and mem_to_st turn one into the other (the first is the offset of the second from the start of `mem`). */
#define ENTITY_POINTERS(P, base, e) P((base) + 0x0A, e.image) P((base) + 0x10, e.table) P((base) + 0x18, e.loop_image)
#define ENTITY_POINTER_MEMBERS(P, i) ENTITY_POINTERS(P, ADDR_TEXT + 0x4E6A + 0x1C * (i), text.entities[i])
#define VOICE_POINTER_MEMBERS(P, i) \
    P(ADDR_TEXT + 0x7404 + 0x30 * (i) + 0x02, text.voices[i].sequence) \
    P(ADDR_TEXT + 0x7404 + 0x30 * (i) + 0x10, text.voices[i].pattern_base) \
    P(ADDR_TEXT + 0x7404 + 0x30 * (i) + 0x14, text.voices[i].pattern) \
    P(ADDR_TEXT + 0x7404 + 0x30 * (i) + 0x20, text.voices[i].envelope) \
    P(ADDR_TEXT + 0x7404 + 0x30 * (i) + 0x24, text.voices[i].envelope_base)
#define EFFECT_POINTER_MEMBERS(P, i) \
    P(ADDR_TEXT + 0x86FC + 0x1A * (i) + 0x12, text.effects[i].volumes_start) \
    P(ADDR_TEXT + 0x86FC + 0x1A * (i) + 0x16, text.effects[i].volumes_at)
#define POINTER_MEMBERS(P) \
    P(ADDR_TEXT + 0x1BF8, text.message_pointers) \
    P(ADDR_TEXT + 0x4656 + 0x0A, text.sprite_entity.image) P(ADDR_TEXT + 0x4656 + 0x10, text.sprite_entity.table) P(ADDR_TEXT + 0x4656 + 0x18, text.sprite_entity.loop_image) \
    P(ADDR_TEXT + 0x12E0, text.picture_tables) \
    ENTITY_POINTER_MEMBERS(P, 0) ENTITY_POINTER_MEMBERS(P, 1) ENTITY_POINTER_MEMBERS(P, 2) ENTITY_POINTER_MEMBERS(P, 3) \
    ENTITY_POINTER_MEMBERS(P, 4) ENTITY_POINTER_MEMBERS(P, 5) ENTITY_POINTER_MEMBERS(P, 6) ENTITY_POINTER_MEMBERS(P, 7) \
    ENTITY_POINTER_MEMBERS(P, 8) ENTITY_POINTER_MEMBERS(P, 9) ENTITY_POINTER_MEMBERS(P, 10) ENTITY_POINTER_MEMBERS(P, 11) \
    ENTITY_POINTER_MEMBERS(P, 12) ENTITY_POINTER_MEMBERS(P, 13) ENTITY_POINTER_MEMBERS(P, 14) ENTITY_POINTER_MEMBERS(P, 15) \
    ENTITY_POINTER_MEMBERS(P, 16) ENTITY_POINTER_MEMBERS(P, 17) ENTITY_POINTER_MEMBERS(P, 18) ENTITY_POINTER_MEMBERS(P, 19) \
    ENTITY_POINTER_MEMBERS(P, 20) ENTITY_POINTER_MEMBERS(P, 21) ENTITY_POINTER_MEMBERS(P, 22) ENTITY_POINTER_MEMBERS(P, 23) \
    P(ADDR_TEXT + 0x2B64 + 0x0A, text.animated_2b64.image) P(ADDR_TEXT + 0x2B96 + 0x0A, text.animated_2b96.image) \
    P(ADDR_TEXT + 0x2BC0 + 0x0A, text.animated_2bc0.image) \
    VOICE_POINTER_MEMBERS(P, 0) VOICE_POINTER_MEMBERS(P, 1) VOICE_POINTER_MEMBERS(P, 2) \
    EFFECT_POINTER_MEMBERS(P, 0) EFFECT_POINTER_MEMBERS(P, 1) EFFECT_POINTER_MEMBERS(P, 2) \
    P(ADDR_BSS + 0xDC, vars.hidden_entity) P(ADDR_BSS + 0xE0, vars.walking_entity) P(ADDR_BSS + 0xE4, vars.walking_object) \
    P(ADDR_BSS + 0xE8, vars.frieze_entity) P(ADDR_BSS + 0xEE, vars.shaking_entity) P(ADDR_BSS + 0xF4, vars.creature_entity) \
    P(ADDR_BSS + 0xF8, vars.bubble_entity) P(ADDR_BSS + 0xFC, vars.carrier_entity) P(ADDR_BSS + 0x102, vars.partner_entity) \
    P(ADDR_BSS + 0x10A, vars.thrown_entity) P(ADDR_BSS + 0x10E, vars.thrown_object) P(ADDR_BSS + 0x118, vars.dropped_object)

#define GUEST_MEMBERS(M) \
    M(0x70, sys.vbl_vector, 4) \
    M(0x466, sys.frclock, 4) \
    M(0x600, sys.kbd_vectors, 4) \
    M(ADDR_TEXT + 0x46D0, text.end_pictures, 2) \
    M(ADDR_TEXT + 0x67C, text.vbl_counter, 2) \
    M(ADDR_TEXT + 0x67E, text.vbl_event, 2) \
    M(ADDR_TEXT + 0x6AA, text.vbl_old_vector, 4) \
    M(ADDR_TEXT + 0x848, text.frame_data, 2) \
    M(ADDR_TEXT + 0x12E0, text.picture_tables, 4) \
    M(ADDR_TEXT + 0x1BF8, text.message_pointers, 4) \
    PLACEMENT_FIELDS(M, ADDR_TEXT + 0x2302, text.blinking_picture) \
    PLACEMENT_FIELDS(M, ADDR_TEXT + 0x2320, text.trapdoor_picture) \
    PLACEMENT_FIELDS(M, ADDR_TEXT + 0x2348, text.picture_2348) \
    PLACEMENT_FIELDS(M, ADDR_TEXT + 0x2368, text.picture_2368) \
    PLACEMENT_FIELDS(M, ADDR_TEXT + 0x27E8, text.picture_27e8) \
    ANIMATED_FIELDS(M, ADDR_TEXT + 0x2B64, text.animated_2b64) \
    ANIMATED_FIELDS(M, ADDR_TEXT + 0x2B96, text.animated_2b96) \
    ANIMATED_FIELDS(M, ADDR_TEXT + 0x2BC0, text.animated_2bc0) \
    QUAD_FIELDS(M, ADDR_TEXT + 0x2D2E + 0x00, text.frieze_pictures[0]) QUAD_FIELDS(M, ADDR_TEXT + 0x2D2E + 0x08, text.frieze_pictures[1]) \
    QUAD_FIELDS(M, ADDR_TEXT + 0x2D2E + 0x10, text.frieze_pictures[2]) QUAD_FIELDS(M, ADDR_TEXT + 0x2D2E + 0x18, text.frieze_pictures[3]) \
    QUAD_FIELDS(M, ADDR_TEXT + 0x2D2E + 0x20, text.frieze_pictures[4]) \
    PLACEMENT_FIELDS(M, ADDR_TEXT + 0x2D56, text.frieze_blinker) \
    M(ADDR_TEXT + 0x3184, text.no_offset, 2) \
    PLACEMENT_FIELDS(M, ADDR_TEXT + 0x318C, text.picture_318c) \
    M(ADDR_TEXT + 0x326E, text.sequences, 2) \
    M(ADDR_TEXT + 0x33F6, text.wave, 2) \
    M(ADDR_TEXT + 0x3404, text.rooms, 2) \
    M(ADDR_TEXT + 0x3F58, text.records_3f58, 2) \
    ENTITY_FIELDS(M, ADDR_TEXT + 0x4656, text.sprite_entity) \
    ENTITY_MEMBERS(M, 0) ENTITY_MEMBERS(M, 1) ENTITY_MEMBERS(M, 2) ENTITY_MEMBERS(M, 3) \
    ENTITY_MEMBERS(M, 4) ENTITY_MEMBERS(M, 5) ENTITY_MEMBERS(M, 6) ENTITY_MEMBERS(M, 7) \
    ENTITY_MEMBERS(M, 8) ENTITY_MEMBERS(M, 9) ENTITY_MEMBERS(M, 10) ENTITY_MEMBERS(M, 11) \
    ENTITY_MEMBERS(M, 12) ENTITY_MEMBERS(M, 13) ENTITY_MEMBERS(M, 14) ENTITY_MEMBERS(M, 15) \
    ENTITY_MEMBERS(M, 16) ENTITY_MEMBERS(M, 17) ENTITY_MEMBERS(M, 18) ENTITY_MEMBERS(M, 19) \
    ENTITY_MEMBERS(M, 20) ENTITY_MEMBERS(M, 21) ENTITY_MEMBERS(M, 22) ENTITY_MEMBERS(M, 23) \
    M(ADDR_TEXT + 0x510A, text.records_510a, 2) \
    M(ADDR_TEXT + 0x5A9A, text.room_pictures, 2) \
    M(ADDR_TEXT + 0x5B86, text.fall_speeds, 2) \
    M(ADDR_TEXT + 0x5F78, text.bubble_placement, 2) \
    M(ADDR_TEXT + 0x69DA, text.object_templates, 2) \
    M(ADDR_TEXT + 0x6BC0, text.objects, 2) \
    M(ADDR_TEXT + 0x7050, text.fade_palette, 2) \
    M(ADDR_TEXT + 0x7070, text.fade_output, 2) \
    M(ADDR_TEXT + 0x7102, text.fade_in_table, 2) \
    M(ADDR_TEXT + 0x7284, text.palettes, 2) \
    VOICE_MEMBERS(M, 0) VOICE_MEMBERS(M, 1) VOICE_MEMBERS(M, 2) \
    EFFECT_MEMBERS(M, 0) EFFECT_MEMBERS(M, 1) EFFECT_MEMBERS(M, 2) \
    M(ADDR_TEXT + 0x7B80, text.effect_period, 2) \
    M(ADDR_TEXT + 0x7B8E, text.note_table, 2) \
    M(ADDR_TEXT + 0x7CA8, text.last_tone, 2) \
    M(ADDR_TEXT + 0x8524, text.effect_descriptors, 2) \
    M(ADDR_TEXT + 0x8674, text.effect_volume_lists, 2) \
    M(ADDR_TEXT + 0x8766, text.sound_random, 4) \
    M(ADDR_TEXT + 0x8776, text.envelopes, 2) \
    M(ADDR_TEXT + 0x7CAC, text.sound_sets, 2) \
    M(ADDR_TEXT + 0x8874, text.graphics, 2) \
    M(ADDR_BSS + 0xA0, vars.room, 2) \
    M(ADDR_BSS + 0xA2, vars.previous_room, 2) \
    M(ADDR_BSS + 0xA6, vars.dropped_objects, 2) \
    M(ADDR_BSS + 0xA8, vars.player_item, 2) \
    M(ADDR_BSS + 0xAA, vars.first_creature_item, 2) \
    M(ADDR_BSS + 0xAC, vars.second_creature_item, 2) \
    M(ADDR_BSS + 0xAE, vars.creature_away_ticks, 2) \
    M(ADDR_BSS + 0xB0, vars.message_progress, 2) \
    M(ADDR_BSS + 0xB2, vars.message_number, 2) \
    M(ADDR_BSS + 0xB4, vars.next_message, 2) \
    M(ADDR_BSS + 0xB6, vars.idle_ticks, 2) \
    M(ADDR_BSS + 0xB8, vars.idle_step, 2) \
    M(ADDR_BSS + 0xBA, vars.fatigue, 2) \
    M(ADDR_BSS + 0xBC, vars.hand_over_delay, 2) \
    M(ADDR_BSS + 0xBE, vars.blocker_x, 2) \
    M(ADDR_BSS + 0xC0, vars.level_start, 4) \
    M(ADDR_BSS + 0xC4, vars.level_second, 4) \
    M(ADDR_BSS + 0xC8, vars.level_third, 4) \
    M(ADDR_BSS + 0xCC, vars.level_fourth, 4) \
    M(ADDR_BSS + 0xD0, vars.map_run_length, 2) \
    M(ADDR_BSS + 0xD4, vars.wipe_mask, 2) \
    M(ADDR_BSS + 0xD6, vars.points_collected, 2) \
    M(ADDR_BSS + 0xDC, vars.hidden_entity, 4) \
    M(ADDR_BSS + 0xE0, vars.walking_entity, 4) \
    M(ADDR_BSS + 0xE8, vars.frieze_entity, 4) \
    M(ADDR_BSS + 0xE4, vars.walking_object, 4) \
    M(ADDR_BSS + 0xEC, vars.walk_steps_left, 2) \
    M(ADDR_BSS + 0xEE, vars.shaking_entity, 4) \
    M(ADDR_BSS + 0xF2, vars.shake_phase, 2) \
    M(ADDR_BSS + 0xF4, vars.creature_entity, 4) \
    M(ADDR_BSS + 0xF8, vars.bubble_entity, 4) \
    M(ADDR_BSS + 0xFC, vars.carrier_entity, 4) \
    M(ADDR_BSS + 0x100, vars.room_timer, 2) \
    M(ADDR_BSS + 0x102, vars.partner_entity, 4) \
    M(ADDR_BSS + 0x108, vars.sleeper_seed, 2) \
    M(ADDR_BSS + 0x116, vars.frieze_step, 2) \
    M(ADDR_BSS + 0x118, vars.dropped_object, 4) \
    M(ADDR_BSS + 0x106, vars.ending_countdown, 2) \
    M(ADDR_BSS + 0x10A, vars.thrown_entity, 4) \
    M(ADDR_BSS + 0x10E, vars.thrown_object, 4) \
    M(ADDR_BSS + 0x112, vars.thrown_ticks, 2) \
    M(ADDR_BSS + 0x11C, vars.time_stage, 2) \
    M(ADDR_BSS + 0x11E, vars.bubble_delay, 2) \
    M(ADDR_BSS + 0x120, vars.var_120, 2) \
    M(ADDR_BSS + 0x122, vars.state_routine, 4) \
    M(ADDR_BSS + 0x126, vars.saved_vbl_vector, 4) \
    M(ADDR_BSS + 0x12E, vars.kbdvecs, 4) \
    M(ADDR_BSS + 0x132, vars.old_joyvec, 4) \
    M(ADDR_BSS + 0x136, vars.last_key, 2) \
    M(ADDR_BSS + 0x13A, vars.joy_idle, 2) \
    M(ADDR_BSS + 0x13C, vars.screen_ptr, 4) \
    M(ADDR_BSS + 0x140, vars.file_handle, 2) \
    M(ADDR_BSS + 0x142, vars.back_buf, 4) \
    M(ADDR_BSS + 0x146, vars.scratch_ptr, 4) \
    M(ADDR_BSS + 0x14A, vars.queue_base, 4) \
    M(ADDR_BSS + 0x14E, vars.backgrounds_a, 4) \
    M(ADDR_BSS + 0x152, vars.backgrounds_b, 4) \
    M(ADDR_BSS + 0x156, vars.backgrounds_now, 4) \
    M(ADDR_BSS + 0x15A, vars.queue_count, 2) \
    M(ADDR_BSS + 0x15C, vars.first_entity_queue_count, 2) \
    M(ADDR_BSS + 0x15E, vars.sprites, 4) \
    M(ADDR_BSS + 0x162, vars.font, 4) \
    M(ADDR_BSS + 0x166, vars.saved_sp, 4) \
    M(ADDR_BSS + 0x16A, vars.super_sp, 4) \
    M(ADDR_BSS + 0x16E, vars.draw_queue, 4) \
    M(ADDR_BSS + 0x2AE, vars.sprite_scratch, 2) \
    M(ADDR_BSS + 0x66C, vars.bit_reversed_below, 2) \
    M(ADDR_BSS + 0x66E, vars.bit_reversed, 2) \
    M(ADDR_BSS + 0x86E, vars.picture_lists, 4) \
    M(ADDR_BSS + 0xB3E, vars.room_picture_lists, 4) \
    M(ADDR_BSS + 0xC80, vars.free_memory, 4) \
    M(ADDR_HEAP, heap.level_data, 2) \
    M(ADDR_HEAP + 0x222E0, heap.back_buf, 2) \
    M(ADDR_HEAP + 0x222E0 + 0x9000, heap.backgrounds_a, 2) \
    M(ADDR_HEAP + 0x222E0 + 0x9000 + 0x4E20, heap.backgrounds_b, 2) \
    M(ADDR_SCREEN, screen.planes, 2)

#define CHECK_MEMBER(addr, member, elem) \
    LAYOUT_ASSERT(offsetof(struct StMem, member) == (addr), #member " is not at its address");
GUEST_MEMBERS(CHECK_MEMBER)
#define CHECK_POINTER(addr, member) \
    LAYOUT_ASSERT(offsetof(struct StMem, member) == (addr), #member " is not at its address");
POINTER_MEMBERS(CHECK_POINTER)

typedef struct {
    const char *name;
    uint32_t    addr;   /* ST address */
    uint8_t     size;   /* element size: 1, 2 or 4 */
    uint32_t    count;
} MemField;

extern StMem mem;
extern const MemField mem_fields[];
extern const unsigned mem_field_count;
int mem_is_pointer(uint32_t address);
uint32_t mem_address_of_pointer(uint32_t pointer);   /* a pointer member's value as the original's address (what is not a pointer into mem is data) */
uint32_t mem_pointer_of_address(uint32_t address);   /* whether the word at this address is one of the pointer members */

/* An address the original stores in memory (the pointer fields), as a pointer into `mem`. The bus has 24 bits:
 * the top byte is a flag some records keep there. */
#define ST_ADDRESS(pointer) ((uint32_t)((const uint8_t *)(pointer) - (const uint8_t *)&mem))
LAYOUT_ASSERT(sizeof(void *) == 4, "the port is built 32-bit, so pointers are the size of the original's addresses");
#define TEXT_AT(type, offset) ((type *)((uint8_t *)&mem.text + (offset)))   /* something in the code segment, by its offset */
#define ST_PTR(type, addr) ((type *)((uint8_t *)&mem + ((addr) & 0xFFFFFF)))

/* What the original gets from TOS and the hardware, provided by the platform (sdl.c). */
uint32_t tos_malloc(uint32_t size);                          /* GEMDOS Malloc: an address, 0 when it fails; -1: the free size */
uint16_t tos_open(const char *name);                         /* GEMDOS Fopen: a handle, or a negative error */
void tos_read(uint16_t handle, uint16_t *dest, uint32_t bytes);   /* GEMDOS Fread of big-endian words */
void tos_close(uint16_t handle);
/* The console keys: GEMDOS gives a key as (scancode << 16) | ascii. */
#define ASCII_BACKSPACE 0x08
#define ASCII_TAB       0x09
#define ASCII_RETURN    0x0D
#define ASCII_ESCAPE    0x1B
#define ASCII_SPACE     0x20
#define SCAN_ESCAPE     0x01
#define SCAN_BACKSPACE  0x0E
#define SCAN_TAB        0x0F
#define SCAN_RETURN     0x1C
#define SCAN_SPACE      0x39
#define SCAN_F1         0x3B
#define SCAN_F2         0x3C
#define KEY_ASCII(key)  ((uint16_t)(key))
#define KEY_SCAN(key)   ((uint16_t)((key) >> 16))
#define KEY_OF(scan, ascii) ((uint32_t)(scan) << 16 | (ascii))

uint32_t tos_wait_key(void);                                 /* GEMDOS Crawcin: waits for a key, the screen stays as it is */
uint32_t tos_raw_input(void);                                /* GEMDOS Crawio of 0xFF: (scancode << 16) | ascii, 0 when no key */
uint32_t tos_physbase(void);                                 /* XBIOS Physbase */
void tos_setscreen(void);                                    /* XBIOS Setscreen: low resolution, screens unchanged */
void tos_enter_supervisor(void);                             /* GEMDOS Super */
void ikbd_command(uint8_t command);                          /* a byte for the keyboard chip */
void ym_write(uint8_t reg, uint8_t value);                   /* the sound chip */
uint8_t ym_read(uint8_t reg);
uint8_t tos_mfp_gpip(void);                                  /* the MFP's general purpose input: bit 7 is low with a monochrome monitor */
uint8_t tos_video_sync(void);                                /* the video shifter's sync mode: bit 1 set for 50 Hz */
extern uint16_t hardware_palette[16];                        /* the palette registers, 0x0RGB with 3 bits a colour */
int sync_arrive(int id);   /* a place where the original takes interrupts; nonzero: a joystick packet was taken, arrive again */

/* The routines of the original, by the address they have there. */
void program_entry(void);                                 /* $0000: what the program does from its start */
void initialize_program(void);                            /* $0536: memory, tables and the first pictures */
void save_system_palette(void);                           /* $082C */
void set_black_palette(void);                             /* $0826 */
void set_palette(const uint16_t colours[16]);             /* $07E0 */
void build_bit_reversed(void);                            /* $6FFC */
void build_record_lists(void);                            /* $6FCA */
void load_file(const char *name, uint16_t *dest, uint32_t bytes);   /* $6F36 */
void load_picture(const char *name);                      /* $6F76: a Neochrome picture onto the screen */
void load_level_data(void);                               /* $07A4 */
void install_joystick_handler(void);                      /* $06B2 */
void save_vbl_vector(void);                               /* $0650 */
void install_vbl_handler(void);                           /* $0664 */
void wait_vbl_tick(void);                                 /* $0790 */
void start_fade(void);                                    /* $71C6 */
void wait_fade(void);                                     /* $71EC */
const uint16_t *entity_image(const struct Entity *entity, uint16_t frame);   /* $5F46 */
void copy_object_templates(void);                         /* $6DA6 */
void place_player(void);                                  /* $04F6 */
void reset_game_state(void);                              /* $03C8 */
void mark_entities(void);                                 /* $4A12 */
void build_block_map(void);                               /* $494A */
void place_room_objects(void);                            /* $6984 */
void draw_status_panel(void);                             /* $18FA */
void draw_room_sprites(void);                             /* $463C */
void draw_room(void);                                     /* $4546 */
void post_message(uint16_t number);                       /* $1A68 */
void start_bubble(uint16_t kind, uint16_t time);          /* $5F68 */
void update_message(void);                                /* $1AF0 */
void update_clock(void);                                  /* $19AE */
void update_score_display(void);                          /* $1954 */
void draw_fatigue_bar(void);                               /* $1896 */
void clamp_to_room(uint16_t *x, uint16_t *y);             /* $2288 */
void test_ground(struct Entity *entity);                  /* $49B4 */
void move_entity_horizontally(struct Entity *entity, int16_t distance);   /* $5BA2 */
void update_entity(struct Entity *entity);                /* $5ACC */
void restart_sound_if_done(void);                         /* $038C */
void update_first_entity(void);                           /* $13B8 */
uint16_t find_neighbor(const struct RoomRecord *room, uint16_t other);   /* $4504 */
void shake_object(void);                                  /* $6534 */
void move_walking_object(void);                           /* $6572 */
void drop_objects(void);                                  /* $65CC */
void move_thrown_object(void);                            /* $33AA */
void fall_asleep(void);                                   /* $17AA */
void update_companion(void);                              /* $1502 */
void update_second_companion(void);                       /* $16D6 */
void check_room_exit(void);                               /* $43DC */
void add_score_ten(void);                                 /* $199A */
void add_score_hundred(void);                             /* $19A4 */
void fatigue_up(void);                                     /* $1858 */
void lose_item(void);                                     /* $6642 */
void sound_effect(uint16_t effect);                       /* $03B2 */
void use_item(void);                                      /* $6808 */
void drop_item(uint16_t *slot, struct Entity *by);        /* $6810, $6868, $687E */
void start_fade_in(void);                                 /* $7096 */
void sound_stop(void);                                    /* $7758 */
void sound_effects_stop(void);                            /* $876A */
void sound_start(int8_t set);                             /* $7378 */
void ikbd_joystick_event(const uint8_t *packet);          /* $06FE: the keyboard chip's joystick report */
uint8_t joystick_direction(void);                         /* $0716: the report as a direction code (0 idle) */
void draw_sprite_queue(void);                             /* $6310: blit every queued sprite into the back buffer */
unsigned shift_sprite_right(uint16_t *rows, unsigned bits, unsigned carry);  /* $62A4: returns the carry out */
void present_back_buffer(void);                           /* $6E7C: interleave the back buffer onto the screen */

/* What the original does when it is sent through a table to something that is not code (unknown.c). */
void unknown_room_routine(uint32_t address);
void unknown_entity_command(unsigned command);

void run_room_init(uint32_t address);                     /* $4546's jump to the routine of the room */
void run_room_state(uint32_t address);                    /* the routine the room runs every frame */
void copy_screen_to_back_buffer(void);                    /* $6E4C */
void scatter_end_pictures(void);                          /* $467A */
void play_game(void);                                     /* $00FA */
uint32_t read_key(void);                                  /* $22B2 */
void unknown_sound_command(uint32_t address);
void main_loop_iteration(void);                           /* $02B4 */
void run_room_init(uint32_t address);
void run_room_state(uint32_t address);                 /* a routine of a room, by the address the tables keep it at */
void vbl_handler(void);                                   /* $0680: the vertical blank interrupt */
void fade_out_step(void);                                 /* $7202 */
void fade_in_step(void);                                  /* $7120 */
void sound_update(void);                                  /* $7332: the sound player, once a vertical blank */
