//
// Cthugha ESP32-P4 Port
// Original: Zaph, Digital Aasvogel Group, Torps Productions 1993-1995
//

#pragma once

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

// Internal framebuffer: 240x240 @ 8-bit indexed color
// Scales 3x to 720x720 LCD output
#define BUFF_WIDTH   240u
#define BUFF_HEIGHT  240u
#define BUFF_SIZE    (BUFF_WIDTH * BUFF_HEIGHT)
#define BUFF_BOTTOM  (BUFF_HEIGHT - 4u)

#define NUMTABLES    10
#define NUMMASSAGES  3
#define LUTSIZE      (3 * 256)
#define MAXLUTS      30

typedef struct {
    void (*function)(void);
    uint8_t flag_when;
    char name[21];
} function_opt;

#define WHEN_ALWAYS  0xFF
#define WHEN_NEVER   0x00

// --- Core state ---
extern uint8_t *buff;
extern uint8_t *shadow;
extern int stereo[BUFF_WIDTH][2];
extern int table[NUMTABLES][256];
extern int curtable;
extern int minnoise;
extern int min_time, rand_time;

// --- Palette ---
extern uint8_t LUTbuffer[LUTSIZE];
extern uint8_t LUTfiles[MAXLUTS][LUTSIZE];
extern int numluts;
extern int curpal;
extern int use_pal_cycle;    // palette rotation active this cycle
extern int pal_cycle_offset; // current rotation offset (0-255)
extern int use_alignment;    // zero-crossing alignment for wave effects
void fill_lut_buffer(int pal);
void init_palettes(void);

// --- Flames ---
extern void (*flame)(void);
extern function_opt flamearray[];
extern int numflames;
int change_flame(int flamenum);

// --- Waves ---
extern void (*wave)(void);
extern function_opt wavearray[];
extern int numwaves;
extern int usewave;
void change_wave(int wavenum);
void next_wave(void);

// --- Display modes ---
extern void (*display_effect)(void);
extern function_opt disparray[];
extern int numdisplays;
extern int curdisplay;
int change_display(int disp);
void flip_screens(void);

// --- Translation ---
extern int nrtrans;
extern int translate_idx;
void translate_screen(void);
void init_translate(void);

// --- Audio ---
int get_stereo_data(void);

// --- Init ---
void init_tables(void);
void init_divsub(void);
extern uint8_t divsub[1024];
extern int sine_table[BUFF_WIDTH];
void init_sine(void);

// --- Helpers ---
static inline int ct_min(int a, int b) { return a < b ? a : b; }
static inline int ct_max(int a, int b) { return a > b ? a : b; }
static inline int ct_clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
