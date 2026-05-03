//
// Cthugha ESP32-P4 Port — Boom Boxes
// Inspired by the JS port by delaneyparker (github.com/delaneyparker/cthugha-js)
// Not in the original v5.3 DOS source.
//
// Two colored squares bounce around the framebuffer seeding pixels that the
// flame and wave effects then propagate. Size is driven by audio amplitude.
// Randomly enabled/disabled by randomize_all().
//

#include "boom_box.h"
#include "cthugha.h"
#include "esp_random.h"

BoomBox boom_boxes[NUM_BOOM_BOXES];
int boom_boxes_active = 0;
int boom_table_color  = 0;
int boom_scale        = 1;

void boom_box_reset(BoomBox *b, int start_x, int start_y)
{
    b->x = start_x;
    b->y = start_y;

    // Velocity: 1-3 pixels/frame, random sign, never zero
    b->vx = (int)(1 + esp_random() % 3);
    if (esp_random() & 1) b->vx = -b->vx;
    b->vy = (int)(1 + esp_random() % 3);
    if (esp_random() & 1) b->vy = -b->vy;

    b->color     = (int)(1 + esp_random() % 200);
    b->color_inc = (int)(1 + esp_random() % 4);
    b->size      = 1;
}

void boom_boxes_init(void)
{
    boom_box_reset(&boom_boxes[0], BUFF_WIDTH / 2 - 20, BUFF_HEIGHT / 2);
    boom_box_reset(&boom_boxes[1], BUFF_WIDTH / 2 + 20, BUFF_HEIGHT / 2);
    boom_boxes_active = 0;
}

void boom_boxes_randomize(void)
{
    // ~40% chance of being active each time effects are randomized
    boom_boxes_active = ((esp_random() % 10) < 4) ? 1 : 0;

    if (boom_boxes_active) {
        // 10% chance of resetting positions/velocities (matches JS behavior)
        if ((esp_random() % 10) == 0) {
            boom_box_reset(&boom_boxes[0], BUFF_WIDTH / 2 - 20, BUFF_HEIGHT / 2);
            boom_box_reset(&boom_boxes[1], BUFF_WIDTH / 2 + 20, BUFF_HEIGHT / 2);
        }
        boom_table_color = esp_random() & 1;          // 50% chance
        boom_scale       = 1 + (int)(esp_random() % 3); // 1×, 2×, or 3×
    }
}

static void boom_box_update(BoomBox *b, int loudness)
{
    // Base size 1-6 from audio, multiplied by boom_scale (1, 2, or 3)
    b->size = boom_scale * (1 + loudness * 5 / 128);
    b->size = ct_clamp(b->size, boom_scale, boom_scale * 6);

    // Move
    b->x += b->vx;
    b->y += b->vy;

    // Bounce off buffer edges with size margin
    if (b->x < b->size) {
        b->x  = b->size;
        b->vx = -b->vx;
    } else if (b->x >= (int)BUFF_WIDTH - b->size) {
        b->x  = (int)BUFF_WIDTH - b->size - 1;
        b->vx = -b->vx;
    }
    if (b->y < b->size) {
        b->y  = b->size;
        b->vy = -b->vy;
    } else if (b->y >= (int)BUFF_HEIGHT - b->size) {
        b->y  = (int)BUFF_HEIGHT - b->size - 1;
        b->vy = -b->vy;
    }

    // Cycle color counter, stay off black
    b->color = (b->color + b->color_inc) % 256;
    if (b->color == 0) b->color = 1;

    // Resolve actual palette index: direct or through the current wave table
    uint8_t paint = boom_table_color
                    ? (uint8_t)table[curtable][b->color]
                    : (uint8_t)b->color;
    if (paint == 0) paint = 1;

    // Paint box into framebuffer
    for (int dy = -b->size; dy <= b->size; dy++) {
        int py = b->y + dy;
        if (py < 0 || py >= (int)BUFF_HEIGHT) continue;
        for (int dx = -b->size; dx <= b->size; dx++) {
            int px = b->x + dx;
            if (px < 0 || px >= (int)BUFF_WIDTH) continue;
            buff[py * BUFF_WIDTH + px] = paint;
        }
    }
}

void boom_boxes_update(void)
{
    if (!boom_boxes_active) return;

    // Derive per-channel peak amplitude from stereo[] (values 0-255, centered at 128)
    int loud_l = 0, loud_r = 0;
    for (int i = 0; i < (int)BUFF_WIDTH; i++) {
        int dl = abs(stereo[i][0] - 128);
        int dr = abs(stereo[i][1] - 128);
        if (dl > loud_l) loud_l = dl;
        if (dr > loud_r) loud_r = dr;
    }

    boom_box_update(&boom_boxes[0], loud_l);
    boom_box_update(&boom_boxes[1], loud_r);
}
