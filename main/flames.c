//
// Cthugha ESP32-P4 Port — Flame effects
// Ported from x86 inline assembly to portable C
// Original: Zaph, Digital Aasvogel Group, Torps Productions 1993-1995
//

#include "cthugha.h"

uint8_t divsub[1024];

void init_divsub(void)
{
    for (int i = 0; i < 1024; i++) {
        divsub[i] = i >> 2;
        if (divsub[i])
            divsub[i]--;
    }
}

void (*flame)(void);
int numflames = -1;

// --- Flame effects ---
// Each flame averages neighboring pixels and writes the result to a shifted
// position, creating directional scrolling with fade. The divsub[] lookup
// produces: max(0, sum/4 - 1), giving a slow brightness decay.

// Scroll upward — avg(left, center, right, below)
static void flame_upslow(void)
{
    for (unsigned int y = 0; y < BUFF_HEIGHT - 1; y++) {
        uint8_t *dst = buff + y * BUFF_WIDTH;
        uint8_t *src = buff + (y + 1) * BUFF_WIDTH;
        unsigned int prev = src[-1 < 0 ? 0 : 0]; // handle x=0 edge
        for (unsigned int x = 0; x < BUFF_WIDTH; x++) {
            unsigned int left  = (x > 0) ? buff[(y+1) * BUFF_WIDTH + x - 1] : 0;
            unsigned int center = buff[(y+1) * BUFF_WIDTH + x];
            unsigned int right = (x < BUFF_WIDTH - 1) ? buff[(y+1) * BUFF_WIDTH + x + 1] : 0;
            unsigned int below = (y + 2 < BUFF_HEIGHT) ? buff[(y+2) * BUFF_WIDTH + x] : 0;
            dst[x] = divsub[left + center + right + below];
        }
    }
}

// Scroll upward — 8-bit wrapping sum (subtler effect)
static void flame_upsubtle(void)
{
    for (unsigned int y = 0; y < BUFF_HEIGHT - 1; y++) {
        for (unsigned int x = 0; x < BUFF_WIDTH; x++) {
            uint8_t sum = 0;
            if (x > 0)               sum += buff[(y+1) * BUFF_WIDTH + x - 1];
            sum += buff[(y+1) * BUFF_WIDTH + x];
            if (x < BUFF_WIDTH - 1)  sum += buff[(y+1) * BUFF_WIDTH + x + 1];
            if (y + 2 < BUFF_HEIGHT) sum += buff[(y+2) * BUFF_WIDTH + x];
            buff[y * BUFF_WIDTH + x] = divsub[sum];
        }
    }
}

// Fast upward — iterates bottom-to-top, reads below-neighbors
static void flame_upfast(void)
{
    for (int y = BUFF_HEIGHT - 2; y >= 0; y--) {
        for (int x = BUFF_WIDTH - 1; x >= 0; x--) {
            unsigned int center = buff[y * BUFF_WIDTH + x];
            unsigned int bl = (x > 0 && y + 1 < (int)BUFF_HEIGHT) ?
                              buff[(y+1) * BUFF_WIDTH + x - 1] : 0;
            unsigned int br = (x + 1 < (int)BUFF_WIDTH && y + 1 < (int)BUFF_HEIGHT) ?
                              buff[(y+1) * BUFF_WIDTH + x + 1] : 0;
            unsigned int below = (y + 1 < (int)BUFF_HEIGHT) ?
                                 buff[(y+1) * BUFF_WIDTH + x] : 0;
            buff[y * BUFF_WIDTH + x] = divsub[center + bl + br + below];
        }
    }
}

// Scroll left — avg(upper-right, center, right, below)
static void flame_leftslow(void)
{
    for (unsigned int y = 0; y < BUFF_HEIGHT - 1; y++) {
        for (unsigned int x = 0; x < BUFF_WIDTH; x++) {
            unsigned int ur = (x + 1 < BUFF_WIDTH && y > 0) ?
                              buff[(y-1) * BUFF_WIDTH + x + 1] : 0;
            unsigned int center = buff[(y+1) * BUFF_WIDTH + x];
            unsigned int right = (x + 1 < BUFF_WIDTH) ?
                                 buff[(y+1) * BUFF_WIDTH + x + 1] : 0;
            unsigned int below = (y + 2 < BUFF_HEIGHT) ?
                                 buff[(y+2) * BUFF_WIDTH + x] : 0;
            buff[y * BUFF_WIDTH + x] = divsub[ur + center + right + below];
        }
    }
}

// Left subtle — wrapping 8-bit sum
static void flame_leftsubtle(void)
{
    for (unsigned int y = 0; y < BUFF_HEIGHT - 1; y++) {
        for (unsigned int x = 0; x < BUFF_WIDTH; x++) {
            uint8_t sum = 0;
            if (x + 1 < BUFF_WIDTH && y > 0)
                sum += buff[(y-1) * BUFF_WIDTH + x + 1];
            sum += buff[(y+1) * BUFF_WIDTH + x];
            if (x + 1 < BUFF_WIDTH)
                sum += buff[(y+1) * BUFF_WIDTH + x + 1];
            if (y + 2 < BUFF_HEIGHT)
                sum += buff[(y+2) * BUFF_WIDTH + x];
            buff[y * BUFF_WIDTH + x] = divsub[sum];
        }
    }
}

// Fast left — iterates backwards
static void flame_leftfast(void)
{
    for (int y = BUFF_HEIGHT - 2; y >= 0; y--) {
        for (int x = BUFF_WIDTH - 1; x >= 0; x--) {
            unsigned int center = buff[y * BUFF_WIDTH + x];
            unsigned int br = (x + 1 < (int)BUFF_WIDTH && y + 1 < (int)BUFF_HEIGHT) ?
                              buff[(y+1) * BUFF_WIDTH + x + 1] : 0;
            unsigned int below = (y + 1 < (int)BUFF_HEIGHT) ?
                                 buff[(y+1) * BUFF_WIDTH + x] : 0;
            buff[y * BUFF_WIDTH + x] = divsub[center + br + br + below];
        }
    }
}

// Scroll right — avg(upper-left, center, left, below)
static void flame_rightslow(void)
{
    for (unsigned int y = 0; y < BUFF_HEIGHT - 1; y++) {
        for (unsigned int x = 0; x < BUFF_WIDTH; x++) {
            unsigned int ul = (x > 0 && y > 0) ?
                              buff[(y-1) * BUFF_WIDTH + x - 1] : 0;
            unsigned int center = buff[(y+1) * BUFF_WIDTH + x];
            unsigned int left = (x > 0) ?
                                buff[(y+1) * BUFF_WIDTH + x - 1] : 0;
            unsigned int below = (y + 2 < BUFF_HEIGHT) ?
                                 buff[(y+2) * BUFF_WIDTH + x] : 0;
            buff[y * BUFF_WIDTH + x] = divsub[ul + center + left + below];
        }
    }
}

// Right subtle — wrapping 8-bit sum
static void flame_rightsubtle(void)
{
    for (unsigned int y = 0; y < BUFF_HEIGHT - 1; y++) {
        for (unsigned int x = 0; x < BUFF_WIDTH; x++) {
            uint8_t sum = 0;
            if (x > 0 && y > 0) sum += buff[(y-1) * BUFF_WIDTH + x - 1];
            sum += buff[(y+1) * BUFF_WIDTH + x];
            if (x > 0) sum += buff[(y+1) * BUFF_WIDTH + x - 1];
            if (y + 2 < BUFF_HEIGHT) sum += buff[(y+2) * BUFF_WIDTH + x];
            buff[y * BUFF_WIDTH + x] = divsub[sum];
        }
    }
}

// Fast right — iterates backwards
static void flame_rightfast(void)
{
    for (int y = BUFF_HEIGHT - 2; y >= 0; y--) {
        for (int x = BUFF_WIDTH - 1; x >= 0; x--) {
            unsigned int center = buff[y * BUFF_WIDTH + x];
            unsigned int bl = (x > 0 && y + 1 < (int)BUFF_HEIGHT) ?
                              buff[(y+1) * BUFF_WIDTH + x - 1] : 0;
            unsigned int below = (y + 1 < (int)BUFF_HEIGHT) ?
                                 buff[(y+1) * BUFF_WIDTH + x] : 0;
            buff[y * BUFF_WIDTH + x] = divsub[center + bl + bl + below];
        }
    }
}

// Water — upward scroll on top half, downward on bottom half
static void flame_water(void)
{
    unsigned int mid = BUFF_HEIGHT / 2;

    // Top half: scroll upward
    for (unsigned int y = 0; y < mid; y++) {
        for (unsigned int x = 0; x < BUFF_WIDTH; x++) {
            unsigned int left  = (x > 0) ? buff[(y+1) * BUFF_WIDTH + x - 1] : 0;
            unsigned int center = buff[(y+1) * BUFF_WIDTH + x];
            unsigned int right = (x + 1 < BUFF_WIDTH) ? buff[(y+1) * BUFF_WIDTH + x + 1] : 0;
            unsigned int below = (y + 2 < BUFF_HEIGHT) ? buff[(y+2) * BUFF_WIDTH + x] : 0;
            buff[y * BUFF_WIDTH + x] = divsub[left + center + right + below];
        }
    }

    // Bottom half: scroll downward
    for (unsigned int y = BUFF_HEIGHT - 1; y > mid; y--) {
        for (int x = BUFF_WIDTH - 1; x >= 0; x--) {
            unsigned int ur = (x + 1 < (int)BUFF_WIDTH && y > 0) ?
                              buff[(y-1) * BUFF_WIDTH + x + 1] : 0;
            unsigned int center = buff[y * BUFF_WIDTH + x];
            unsigned int right = (x + 1 < (int)BUFF_WIDTH) ?
                                 buff[y * BUFF_WIDTH + x + 1] : 0;
            unsigned int above = (y > 0) ? buff[(y-1) * BUFF_WIDTH + x] : 0;
            unsigned int sum = ur + center + right + above;
            buff[y * BUFF_WIDTH + x] = (uint8_t)(sum >> 2);
        }
    }
}

// Water subtle variant
static void flame_watersubtle(void)
{
    unsigned int mid = BUFF_HEIGHT / 2;

    for (unsigned int y = 0; y < mid; y++) {
        for (unsigned int x = 0; x < BUFF_WIDTH; x++) {
            uint8_t sum = 0;
            if (x > 0)               sum += buff[(y+1) * BUFF_WIDTH + x - 1];
            sum += buff[(y+1) * BUFF_WIDTH + x];
            if (x + 1 < BUFF_WIDTH)  sum += buff[(y+1) * BUFF_WIDTH + x + 1];
            if (y + 2 < BUFF_HEIGHT) sum += buff[(y+2) * BUFF_WIDTH + x];
            unsigned int v = sum >> 2;
            if (v) v--;
            buff[y * BUFF_WIDTH + x] = (uint8_t)v;
        }
    }

    for (unsigned int y = BUFF_HEIGHT - 1; y > mid; y--) {
        for (int x = BUFF_WIDTH - 1; x >= 0; x--) {
            uint8_t sum = 0;
            if (x + 1 < (int)BUFF_WIDTH && y > 0)
                sum += buff[(y-1) * BUFF_WIDTH + x + 1];
            sum += buff[y * BUFF_WIDTH + x];
            if (x + 1 < (int)BUFF_WIDTH)
                sum += buff[y * BUFF_WIDTH + x + 1];
            if (y > 0)
                sum += buff[(y-1) * BUFF_WIDTH + x];
            buff[y * BUFF_WIDTH + x] = (uint8_t)(sum >> 2);
        }
    }
}

// Skyline — horizontal blur with fade, no vertical scroll
static void flame_skyline(void)
{
    for (unsigned int y = 0; y < BUFF_HEIGHT - 1; y++) {
        for (unsigned int x = 0; x < BUFF_WIDTH; x++) {
            unsigned int left  = (x > 0) ? buff[(y+1) * BUFF_WIDTH + x - 1] : 0;
            unsigned int center = buff[(y+1) * BUFF_WIDTH + x];
            unsigned int right = (x + 1 < BUFF_WIDTH) ? buff[(y+1) * BUFF_WIDTH + x + 1] : 0;
            unsigned int sum = left + center + right + center; // center weighted 2x
            if (sum) sum--;
            buff[y * BUFF_WIDTH + x] = (uint8_t)(sum >> 2);
        }
    }
}

// Weird — OR of neighbors (bitwise, not average) with halving decay.
// OR creates unusual bit-pattern spreading; >>1 each step prevents the
// single-decrement saturation that would flood the screen with one color.
static void flame_weird(void)
{
    for (unsigned int y = 0; y < BUFF_HEIGHT - 1; y++) {
        for (unsigned int x = 0; x < BUFF_WIDTH; x++) {
            unsigned int v = buff[(y+1) * BUFF_WIDTH + x];
            if (x > 0) v |= buff[(y+1) * BUFF_WIDTH + x - 1];
            if (x + 1 < BUFF_WIDTH) v |= buff[(y+1) * BUFF_WIDTH + x + 1];
            if (y + 2 < BUFF_HEIGHT) v |= buff[(y+2) * BUFF_WIDTH + x];
            buff[y * BUFF_WIDTH + x] = (uint8_t)(v >> 1);
        }
    }
}

// Fade — each pixel decremented by 2
static void flame_fade(void)
{
    for (unsigned int i = 0; i < BUFF_SIZE; i++) {
        int v = buff[i];
        if (v > 0) v--;
        if (v > 0) v--;
        buff[i] = (uint8_t)v;
    }
}

// Zzz — diagonal blend (left + below) / 2, then decrement
static void flame_zzz(void)
{
    for (unsigned int y = 0; y < BUFF_HEIGHT - 1; y++) {
        for (unsigned int x = 0; x < BUFF_WIDTH; x++) {
            unsigned int left = (x > 0) ? buff[(y+1) * BUFF_WIDTH + x - 1] : 0;
            unsigned int below = (y + 2 < BUFF_HEIGHT) ? buff[(y+2) * BUFF_WIDTH + x] : 0;
            unsigned int v = (left + below) >> 1;
            if (v) v--;
            buff[y * BUFF_WIDTH + x] = (uint8_t)v;
        }
    }
}

function_opt flamearray[] = {
    { flame_leftslow,     WHEN_ALWAYS, "Slow Left"      },
    { flame_leftsubtle,   WHEN_ALWAYS, "Left Subtle"    },
    { flame_leftfast,     WHEN_ALWAYS, "Left Fast"      },
    { flame_upslow,       WHEN_ALWAYS, "Up Slow"        },
    { flame_upsubtle,     WHEN_ALWAYS, "Up Subtle"      },
    { flame_upfast,       WHEN_ALWAYS, "Up Fast"        },
    { flame_rightslow,    WHEN_ALWAYS, "Right Slow"     },
    { flame_rightsubtle,  WHEN_ALWAYS, "Right Subtle"   },
    { flame_rightfast,    WHEN_ALWAYS, "Right Fast"     },
    { flame_water,        WHEN_ALWAYS, "Water"          },
    { flame_watersubtle,  WHEN_ALWAYS, "Water Subtle"   },
    { flame_skyline,      WHEN_ALWAYS, "Skyline"        },
    { flame_weird,        WHEN_ALWAYS, "Weird"          },
    { flame_zzz,          WHEN_ALWAYS, "Zzz"            },
    { flame_fade,         WHEN_ALWAYS, "Fade"           },
    { NULL,               WHEN_NEVER,  "<END>"          }
};

int change_flame(int flamenum)
{
    if (numflames < 0) {
        numflames = 0;
        while (flamearray[numflames].function != NULL)
            numflames++;
    }
    if (numflames == 0) return 0;

    flamenum = ((flamenum % numflames) + numflames) % numflames;
    flame = flamearray[flamenum].function;
    return flamenum;
}
