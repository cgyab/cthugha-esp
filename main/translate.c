//
// Cthugha ESP32-P4 Port — Translation/remap effects
// Spatial remapping of the framebuffer using precomputed lookup tables
// Original: Zaph, Digital Aasvogel Group, Torps Productions 1993-1995
//

#include "cthugha.h"
#include <math.h>

#define MAXTRANS 8

int nrtrans = 0;
int translate_idx = 0;

static uint16_t *trans_maps[MAXTRANS];
static int current_loaded = -1;

// Built-in procedural translation maps
static void gen_swirl(uint16_t *map, float strength)
{
    int cx = BUFF_WIDTH / 2;
    int cy = BUFF_HEIGHT / 2;
    for (int y = 0; y < (int)BUFF_HEIGHT; y++) {
        for (int x = 0; x < (int)BUFF_WIDTH; x++) {
            float dx = (float)(x - cx);
            float dy = (float)(y - cy);
            float dist = sqrtf(dx * dx + dy * dy);
            float angle = strength / (dist + 1.0f);
            float cs = cosf(angle);
            float sn = sinf(angle);
            int sx = (int)(cs * dx - sn * dy + cx);
            int sy = (int)(sn * dx + cs * dy + cy);
            sx = ct_clamp(sx, 0, BUFF_WIDTH - 1);
            sy = ct_clamp(sy, 0, BUFF_HEIGHT - 1);
            map[y * BUFF_WIDTH + x] = (uint16_t)(sy * BUFF_WIDTH + sx);
        }
    }
}

static void gen_tunnel(uint16_t *map)
{
    int cx = BUFF_WIDTH / 2;
    int cy = BUFF_HEIGHT / 2;
    for (int y = 0; y < (int)BUFF_HEIGHT; y++) {
        for (int x = 0; x < (int)BUFF_WIDTH; x++) {
            float dx = (float)(x - cx);
            float dy = (float)(y - cy);
            float dist = sqrtf(dx * dx + dy * dy);
            float scale = (dist > 1.0f) ? (1.0f - 2.0f / dist) : 0.0f;
            int sx = (int)(dx * scale + cx);
            int sy = (int)(dy * scale + cy);
            sx = ct_clamp(sx, 0, BUFF_WIDTH - 1);
            sy = ct_clamp(sy, 0, BUFF_HEIGHT - 1);
            map[y * BUFF_WIDTH + x] = (uint16_t)(sy * BUFF_WIDTH + sx);
        }
    }
}

static void gen_fisheye(uint16_t *map)
{
    // True fisheye: each destination pixel reads from a source FARTHER from center.
    // nr = sqrt(r) > r for r<1, so source_dist = nr/r * dist = dist/sqrt(r) > dist.
    // Near-center pixels read from outer areas (where flame content lives).
    // The old formula used nr = r*r (barrel distortion, source CLOSER to dark center),
    // which drained the buffer to black.
    int cx = BUFF_WIDTH / 2;
    int cy = BUFF_HEIGHT / 2;
    float max_r = sqrtf((float)(cx * cx + cy * cy));
    for (int y = 0; y < (int)BUFF_HEIGHT; y++) {
        for (int x = 0; x < (int)BUFF_WIDTH; x++) {
            float dx = (float)(x - cx);
            float dy = (float)(y - cy);
            float dist = sqrtf(dx * dx + dy * dy);
            float r = dist / max_r;
            float nr = sqrtf(r);
            int sx = (int)(dx * nr / (r + 0.001f) + cx);
            int sy = (int)(dy * nr / (r + 0.001f) + cy);
            sx = ct_clamp(sx, 0, BUFF_WIDTH - 1);
            sy = ct_clamp(sy, 0, BUFF_HEIGHT - 1);
            map[y * BUFF_WIDTH + x] = (uint16_t)(sy * BUFF_WIDTH + sx);
        }
    }
}

// Dual-vortex effect ported from Cthugha v5.3 molestab.c (via cthugha-js).
// Two counter-rotating spiral centers: left at (W/4, H/2), right at (3W/4, H/2).
// Each half of the image spirals toward/away from its local center.
static void gen_moles(uint16_t *map, float delta_r, float delta_a)
{
    int cy       = BUFF_HEIGHT / 2;
    int cx_left  = BUFF_WIDTH  / 4;
    int cx_right = 3 * BUFF_WIDTH / 4;

    for (int y = 0; y < (int)BUFF_HEIGHT; y++) {
        for (int x = 0; x < (int)BUFF_WIDTH; x++) {
            int map_x, map_y;

            if ((x == cx_left && y == cy) || (x == cx_right && y == cy)) {
                map_x = 0;
                map_y = 0;
            } else {
                int cent_x = (x > (int)BUFF_WIDTH / 2) ? cx_right : cx_left;

                float polar_r = sqrtf((float)((x - cent_x) * (x - cent_x) +
                                              (y - cy)     * (y - cy)));
                float polar_a = atan2f((float)(x - cent_x), (float)(y - cy));

                polar_r += delta_r;
                if (polar_r < 0.0f) polar_r = 0.0f;

                polar_a += (x > (int)BUFF_WIDTH / 2) ? delta_a : -delta_a;

                map_x = (int)roundf(polar_r * sinf(polar_a)) + cent_x;
                map_y = (int)roundf(polar_r * cosf(polar_a)) + cy;

                if (map_y < 0 || map_y >= (int)BUFF_HEIGHT ||
                    map_x < 0 || map_x >= (int)BUFF_WIDTH) {
                    map_x = 0;
                    map_y = 0;
                }
            }

            map[y * BUFF_WIDTH + x] = (uint16_t)(map_y * BUFF_WIDTH + map_x);
        }
    }
}

// Down-spiral: pixel at (i,j) reads from (i+dx, j+dy) where the offset is
// perpendicular to (ang - 45°) at magnitude dist/10. Creates a 45°-phase
// spiral pull toward/away from center, stronger at the edges.
// Wraps out-of-bounds with abs()%BSIZE rather than clamping (original behavior).
static void gen_downspiral(uint16_t *map)
{
    float cx = BUFF_WIDTH  / 2.0f;
    float cy = BUFF_HEIGHT / 2.0f;
    float p  = (float)M_PI / 4.0f;

    for (int j = 0; j < (int)BUFF_HEIGHT; j++) {
        for (int i = 0; i < (int)BUFF_WIDTH; i++) {
            int dx, dy;
            if (j == 0 || j == (int)BUFF_HEIGHT - 1 ||
                i == 0 || i == (int)BUFF_WIDTH  - 1) {
                dx = (int)roundf((cx - i) * 0.75f);
                dy = (int)roundf((cy - j) * 0.75f);
            } else {
                float dist = sqrtf((i - cx) * (i - cx) + (j - cy) * (j - cy));
                float ang  = atan2f((float)(j) - cy, (float)(i) - cx);
                dx = (int)roundf(-sinf(ang - p) * dist / 10.0f);
                dy = (int)roundf( cosf(ang - p) * dist / 10.0f);
            }
            int idx = abs((i + dx) + (j + dy) * (int)BUFF_WIDTH) % (int)BUFF_SIZE;
            map[j * BUFF_WIDTH + i] = (uint16_t)idx;
        }
    }
}

// Big half-wheel: center off-screen at (0.4*W, 0) — top edge, left of center.
// Each pixel is swept radially around that off-screen pivot point, creating
// an asymmetric swooping distortion across the whole buffer.
// Wraps out-of-bounds with abs()%BSIZE (original behavior).
static void gen_bighalfwheel(uint16_t *map)
{
    float cx = BUFF_WIDTH  * 0.4f;
    float cy = 0.0f;
    float q  = (float)M_PI / 2.0f;

    for (int j = 0; j < (int)BUFF_HEIGHT; j++) {
        for (int i = 0; i < (int)BUFF_WIDTH; i++) {
            int dx, dy;
            if (j == 0 || j == (int)BUFF_HEIGHT - 1) {
                dx = (int)((cx - i) * 0.75f);
                dy = (int)(cy - j);
            } else {
                float fi   = (float)i;
                float fj   = (float)j;
                float dist = sqrtf((fi - cx) * (fi - cx) + (fj - cy) * (fj - cy));
                float ang;
                if (fi == cx)
                    ang = (fj > cx) ? q : -q;
                else
                    ang = atanf((fj - cy) / (fi - cx));
                if (fi < cx) ang += (float)M_PI;
                if (dist < (float)BUFF_HEIGHT) {
                    dx = (int)ceilf(-sinf(ang) * dist / 10.0f);
                    dy = (int)ceilf( cosf(ang) * dist / 10.0f);
                } else {
                    dx = (i < (int)cx) ? 3 : -3;
                    dy = 0;
                }
            }
            int idx = abs((j + dy) * (int)BUFF_WIDTH + (i + dx)) % (int)BUFF_SIZE;
            map[j * BUFF_WIDTH + i] = (uint16_t)idx;
        }
    }
}

static void gen_ripple(uint16_t *map)
{
    int cx = BUFF_WIDTH / 2;
    int cy = BUFF_HEIGHT / 2;
    for (int y = 0; y < (int)BUFF_HEIGHT; y++) {
        for (int x = 0; x < (int)BUFF_WIDTH; x++) {
            float dx = (float)(x - cx);
            float dy = (float)(y - cy);
            float dist = sqrtf(dx * dx + dy * dy);
            float wave = sinf(dist * 0.15f) * 4.0f;
            float angle = atan2f(dy, dx);
            int sx = (int)(x + wave * cosf(angle));
            int sy = (int)(y + wave * sinf(angle));
            sx = ct_clamp(sx, 0, BUFF_WIDTH - 1);
            sy = ct_clamp(sy, 0, BUFF_HEIGHT - 1);
            map[y * BUFF_WIDTH + x] = (uint16_t)(sy * BUFF_WIDTH + sx);
        }
    }
}

void init_translate(void)
{
    nrtrans = 0;
    for (int i = 0; i < MAXTRANS; i++)
        trans_maps[i] = NULL;

    // Allocate and generate built-in maps
    for (int i = 0; i < 7; i++) {
        trans_maps[i] = (uint16_t *)malloc(BUFF_SIZE * sizeof(uint16_t));
        if (!trans_maps[i]) break;
        nrtrans++;
    }

    if (nrtrans >= 1) gen_swirl(trans_maps[0], 3.0f);
    if (nrtrans >= 2) gen_tunnel(trans_maps[1]);
    if (nrtrans >= 3) gen_fisheye(trans_maps[2]);
    if (nrtrans >= 4) gen_ripple(trans_maps[3]);
    if (nrtrans >= 5) gen_moles(trans_maps[4], 2.0f, 0.1f);
    if (nrtrans >= 6) gen_downspiral(trans_maps[5]);
    if (nrtrans >= 7) gen_bighalfwheel(trans_maps[6]);

    translate_idx = 0;
}

void translate_screen(void)
{
    if (nrtrans <= 0 || translate_idx <= 0 || translate_idx > nrtrans)
        return;

    uint16_t *map = trans_maps[translate_idx - 1];
    if (!map) return;

    buff[0] = 0;

    for (unsigned int i = 0; i < BUFF_SIZE; i++)
        shadow[i] = buff[map[i]];

    flip_screens();
}
