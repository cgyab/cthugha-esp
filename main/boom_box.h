#pragma once

#include <stdint.h>

typedef struct {
    int x, y;       // current position
    int vx, vy;     // velocity in pixels/frame (±1..3)
    int color;      // current palette index (1-255)
    int color_inc;  // color cycle speed
    int size;       // half-size in pixels (draw box is 2*size+1 square)
} BoomBox;

#define NUM_BOOM_BOXES 2

extern BoomBox boom_boxes[NUM_BOOM_BOXES];
extern int boom_boxes_active;
extern int boom_table_color; // 0=direct palette index, 1=map color through curtable
extern int boom_scale;       // size multiplier: 1, 2, or 3

void boom_boxes_init(void);
void boom_box_reset(BoomBox *b, int start_x, int start_y);
void boom_boxes_update(void); // call each frame after wave()
void boom_boxes_randomize(void); // call from randomize_all()
