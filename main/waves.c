//
// Cthugha ESP32-P4 Port — Wave renderers
// Maps audio data onto the framebuffer as visual patterns
// Original: Zaph, Digital Aasvogel Group, Torps Productions 1993-1995
//

#include "cthugha.h"

void (*wave)(void);
int numwaves = -1;
int usewave = 0;

int sine_table[BUFF_WIDTH];

void init_sine(void)
{
    for (int i = 0; i < (int)BUFF_WIDTH; i++)
        sine_table[i] = (int)(128.0f * sinf((float)i * 0.03927f));
}

// --- Helpers ---

static void do_vwave(int ystart, int yend, int x, unsigned int val)
{
    if ((ystart >= (int)BUFF_HEIGHT) && (yend >= (int)BUFF_HEIGHT)) {
        ystart -= BUFF_HEIGHT;
        yend -= BUFF_HEIGHT;
    }
    if ((ystart < 0) && (yend < 0)) {
        ystart += BUFF_HEIGHT;
        yend += BUFF_HEIGHT;
    }
    ystart = ct_clamp(ystart, 0, BUFF_HEIGHT - 1);
    yend   = ct_clamp(yend,   0, BUFF_HEIGHT - 1);
    x      = ct_clamp(x,      0, BUFF_WIDTH - 1);

    int ys = ct_min(ystart, yend);
    int ye = ct_max(ystart, yend);

    for (int y = ys; y <= ye; y++)
        buff[y * BUFF_WIDTH + x] = (uint8_t)val;
}

static void do_hwave(int xstart, int xend, int y, unsigned int val)
{
    if ((xstart >= (int)BUFF_WIDTH) && (xend >= (int)BUFF_WIDTH)) {
        xstart -= BUFF_WIDTH;
        xend -= BUFF_WIDTH;
    }
    if ((xstart < 0) && (xend < 0)) {
        xstart += BUFF_WIDTH;
        xend += BUFF_WIDTH;
    }
    xstart = ct_clamp(xstart, 0, BUFF_WIDTH - 1);
    xend   = ct_clamp(xend,   0, BUFF_WIDTH - 1);
    y      = ct_clamp(y,      0, BUFF_HEIGHT - 1);

    int xs = ct_min(xstart, xend);
    int xe = ct_max(xstart, xend);

    for (int x = xs; x <= xe; x++)
        buff[y * BUFF_WIDTH + x] = (uint8_t)val;
}

// --- Wave renderers ---

// Dot plot — horizontal, small amplitude
static void wave_dot_hs(void)
{
    for (int x = 0; x < (int)BUFF_WIDTH; x++) {
        int temp = stereo[x][0];
        int py = ct_clamp(BUFF_BOTTOM - (temp >> 2) - 20, 0, BUFF_HEIGHT - 1);
        int px = ct_clamp(x >> 1, 0, BUFF_WIDTH - 1);
        buff[py * BUFF_WIDTH + px] = table[curtable][temp & 0xFF];

        temp = stereo[x][1];
        py = ct_clamp(BUFF_BOTTOM - (temp >> 2) - 20, 0, BUFF_HEIGHT - 1);
        px = ct_clamp((x + BUFF_WIDTH) >> 1, 0, BUFF_WIDTH - 1);
        buff[py * BUFF_WIDTH + px] = table[curtable][temp & 0xFF];
    }
}

// Dot plot — horizontal, large amplitude
static void wave_dot_hl(void)
{
    for (int x = 0; x < (int)BUFF_WIDTH; x++) {
        int temp = stereo[x][0];
        int py = ct_clamp(BUFF_BOTTOM - (temp >> 1) - 20, 0, BUFF_HEIGHT - 1);
        int px = ct_clamp(x >> 1, 0, BUFF_WIDTH - 1);
        buff[py * BUFF_WIDTH + px] = table[curtable][temp & 0xFF];

        temp = stereo[x][1];
        py = ct_clamp(BUFF_BOTTOM - (temp >> 1) - 20, 0, BUFF_HEIGHT - 1);
        px = ct_clamp((x + BUFF_WIDTH) >> 1, 0, BUFF_WIDTH - 1);
        buff[py * BUFF_WIDTH + px] = table[curtable][temp & 0xFF];
    }
}

// Vertical waveform — wide
static void wave_line_vw(void)
{
    int half = BUFF_WIDTH / 2;
    for (int x = 0; x < (int)BUFF_BOTTOM; x++) {
        int temp = stereo[x % BUFF_WIDTH][0];
        int px = ct_clamp(half - (temp >> 2), 0, BUFF_WIDTH - 1);
        buff[x * BUFF_WIDTH + px] = table[curtable][temp & 0xFF];

        temp = stereo[x % BUFF_WIDTH][1];
        px = ct_clamp(half + (temp >> 2), 0, BUFF_WIDTH - 1);
        buff[x * BUFF_WIDTH + px] = table[curtable][temp & 0xFF];
    }
}

// Spike — small amplitude
static void wave_spike_s(void)
{
    for (int x = 0; x < (int)BUFF_WIDTH; x++) {
        int temp = abs(128 - stereo[x][0]) >> 1;
        for (int i = 0; i < temp && i < (int)BUFF_BOTTOM; i++)
            buff[(BUFF_BOTTOM - i) * BUFF_WIDTH + ct_clamp(x >> 1, 0, BUFF_WIDTH - 1)] =
                table[curtable][i & 0xFF];

        temp = abs(128 - stereo[x][1]) >> 1;
        for (int i = 0; i < temp && i < (int)BUFF_BOTTOM; i++)
            buff[(BUFF_BOTTOM - i) * BUFF_WIDTH + ct_clamp((x + BUFF_WIDTH) >> 1, 0, BUFF_WIDTH - 1)] =
                table[curtable][i & 0xFF];
    }
}

// Spike — large amplitude
static void wave_spike_l(void)
{
    for (int x = 0; x < (int)BUFF_WIDTH; x++) {
        int temp = abs(128 - stereo[x][0]);
        for (int i = 0; i < temp && i < (int)BUFF_BOTTOM; i++)
            buff[(BUFF_BOTTOM - i) * BUFF_WIDTH + ct_clamp(x >> 1, 0, BUFF_WIDTH - 1)] =
                table[curtable][temp & 0xFF];
    }
    for (int x = 0; x < (int)BUFF_WIDTH; x++) {
        int temp = abs(128 - stereo[x][1]);
        for (int i = 0; i < temp && i < (int)BUFF_BOTTOM; i++)
            buff[(BUFF_BOTTOM - i) * BUFF_WIDTH + ct_clamp((x + BUFF_WIDTH) >> 1, 0, BUFF_WIDTH - 1)] =
                table[curtable][temp & 0xFF];
    }
}

// Line — horizontal, small amplitude (connected dots)
static void wave_line_hs(void)
{
    int last = 128;
    for (int ch = 0; ch < 2; ch++) {
        last = 128;
        for (int x = 0; x < (int)BUFF_WIDTH; x++) {
            int temp = stereo[x][ch];
            int px = ch ? ((x + BUFF_WIDTH) >> 1) : (x >> 1);
            do_vwave(BUFF_BOTTOM - (temp >> 2), BUFF_BOTTOM - (last >> 2),
                     ct_clamp(px, 0, BUFF_WIDTH - 1), table[curtable][temp & 0xFF]);
            last = temp;
        }
    }
}

// Line — horizontal, large amplitude
static void wave_line_hl(void)
{
    int last = 128;
    for (int ch = 0; ch < 2; ch++) {
        last = 128;
        for (int x = 0; x < (int)BUFF_WIDTH; x++) {
            int temp = stereo[x][ch];
            int px = ch ? ((x + BUFF_WIDTH) >> 1) : (x >> 1);
            do_vwave(BUFF_BOTTOM - (temp >> 1), BUFF_BOTTOM - (last >> 1),
                     ct_clamp(px, 0, BUFF_WIDTH - 1), table[curtable][temp & 0xFF]);
            last = temp;
        }
    }
}

// Dot — vertical, large
static void wave_dot_vl(void)
{
    int half = BUFF_WIDTH / 2;
    for (int x = 0; x < (int)BUFF_BOTTOM; x++) {
        int temp = stereo[x % BUFF_WIDTH][0];
        buff[x * BUFF_WIDTH + ct_clamp(half - (temp >> 1), 0, BUFF_WIDTH - 1)] =
            table[curtable][temp & 0xFF];
        temp = stereo[x % BUFF_WIDTH][1];
        buff[x * BUFF_WIDTH + ct_clamp(half + (temp >> 1), 0, BUFF_WIDTH - 1)] =
            table[curtable][temp & 0xFF];
    }
}

// Spike connected
static void wave_spike(void)
{
    int last = 0;
    for (int ch = 0; ch < 2; ch++) {
        last = 0;
        for (int x = 0; x < (int)BUFF_WIDTH; x++) {
            int temp = abs(128 - stereo[x][ch]);
            int px = ch ? ((x + BUFF_WIDTH) >> 1) : (x >> 1);
            do_vwave(BUFF_BOTTOM - temp, BUFF_BOTTOM - last,
                     ct_clamp(px, 0, BUFF_WIDTH - 1), table[curtable][temp & 0xFF]);
            last = temp;
        }
    }
}

// Walking — rotates columns
static void wave_walking(void)
{
    static int col = 128;
    col = (col + 1) % BUFF_WIDTH;

    int last1 = 128, last2 = 128;
    for (int x = 0; x < (int)BUFF_BOTTOM; x++) {
        int idx = x % BUFF_WIDTH;
        int temp = stereo[idx][0];
        do_hwave(col - (temp >> 2), col - (last1 >> 2), x, table[curtable][temp & 0xFF]);
        last1 = temp;

        temp = stereo[idx][1];
        do_hwave(col + (temp >> 2), col + (last2 >> 2), x, table[curtable][temp & 0xFF]);
        last2 = temp;
    }
}

// Falling — two audio scan lines drift through the full buffer.
// Row advances every 4 frames to match original ~15fps drift speed at 60fps.
// Writes are guarded: index 0 (black) is skipped so the flame content that
// wave_falling drifts through is never erased — only painted over with color.
static void wave_falling(void)
{
    static int row = 0;
    static int frame_count = 0;
    int half = (int)(BUFF_WIDTH / 2);

    if (++frame_count >= 4) {
        frame_count = 0;
        row = (row + 1) % ((int)BUFF_BOTTOM - 1);
    }

    for (int i = 0; i < (int)half; i++) {
        int idx2 = (i + half) % BUFF_WIDTH;
        uint8_t v;

        v = table[curtable][stereo[i][0] & 0xFF];
        if (v) buff[(row + 1) * BUFF_WIDTH + i] = v;
        v = table[curtable][stereo[i][1] & 0xFF];
        if (v) buff[(row + 1) * BUFF_WIDTH + i + half] = v;

        v = table[curtable][stereo[idx2][0] & 0xFF];
        if (v) buff[row * BUFF_WIDTH + i] = v;
        v = table[curtable][stereo[idx2][1] & 0xFF];
        if (v) buff[row * BUFF_WIDTH + i + half] = v;
    }
}

// Lissajous — L/R channels mapped to X/Y
static void wave_lissa(void)
{
    for (int x = 0; x < (int)BUFF_WIDTH; x++) {
        int temp  = stereo[x][0];
        int temp2 = stereo[x][1];
        int py = ((temp + 200 - 28) % BUFF_BOTTOM);
        int px = ((temp2 + 32) % BUFF_WIDTH);
        py = ct_clamp(py, 0, BUFF_HEIGHT - 1);
        px = ct_clamp(px, 0, BUFF_WIDTH - 1);
        buff[py * BUFF_WIDTH + px] = table[curtable][temp & 0xFF];
    }
}

// Line vertical — small amplitude
static void wave_line_vs(void)
{
    int half = BUFF_WIDTH / 2;
    int last1 = 128, last2 = 128;
    for (int x = 0; x < (int)BUFF_BOTTOM; x++) {
        int idx = x % BUFF_WIDTH;
        int temp = stereo[idx][0];
        do_hwave(half - (temp >> 2), half - (last1 >> 2), x, table[curtable][temp & 0xFF]);
        last1 = temp;

        temp = stereo[idx][1];
        do_hwave(half + (temp >> 2), half + (last2 >> 2), x, table[curtable][temp & 0xFF]);
        last2 = temp;
    }
}

// Line vertical — large amplitude
static void wave_line_vl(void)
{
    int half = BUFF_WIDTH / 2;
    int last1 = 128, last2 = 128;
    for (int x = 0; x < (int)BUFF_BOTTOM; x++) {
        int idx = x % BUFF_WIDTH;
        int temp = stereo[idx][0];
        do_hwave(half - (temp >> 1), half - (last1 >> 1), x, table[curtable][temp & 0xFF]);
        last1 = temp;

        temp = stereo[idx][1];
        do_hwave(half + (temp >> 1), half + (last2 >> 1), x, table[curtable][temp & 0xFF]);
        last2 = temp;
    }
}

// Line X — cross pattern
static void wave_line_x(void)
{
    int half = BUFF_WIDTH / 2;
    int last = 128;
    for (int x = 0; x < (int)BUFF_BOTTOM; x++) {
        int idx = x % BUFF_WIDTH;
        int temp = stereo[idx][0];
        do_hwave(half - (temp >> 2), half - (last >> 2), x, table[curtable][temp & 0xFF]);
        last = temp;
    }
    last = 128;
    for (int x = 0; x < (int)BUFF_BOTTOM; x++) {
        int idx = x % BUFF_WIDTH;
        int temp = stereo[idx][1];
        do_hwave(half - 40 + (temp >> 2), half - 40 + (last >> 2), x, table[curtable][temp & 0xFF]);
        last = temp;
    }
}

// Lightning 1
static void wave_lightning1(void)
{
    int temp, last;

    last = 100;
    for (int x = 0; x < (int)BUFF_BOTTOM; x++) {
        temp = ((stereo[x % BUFF_WIDTH][0] - 127) / 16) + last;
        temp = ct_clamp(temp, 0, BUFF_WIDTH - 1);
        do_hwave(temp, last, x, 255);
        last = temp;
    }
    last = BUFF_WIDTH * 5 / 8;
    for (int x = 0; x < (int)BUFF_BOTTOM; x++) {
        temp = ((stereo[x % BUFF_WIDTH][1] - 127) / 16) + last;
        temp = ct_clamp(temp, 0, BUFF_WIDTH - 1);
        do_hwave(temp, last, x, 255);
        last = temp;
    }
}

// Lightning 2 — tighter
static void wave_lightning2(void)
{
    int temp, last;

    last = 100;
    for (int x = 0; x < (int)BUFF_BOTTOM; x++) {
        temp = ((stereo[x % BUFF_WIDTH][0] - 127) / 32) + last;
        temp = ct_clamp(temp, 0, BUFF_WIDTH - 1);
        do_hwave(temp, last, x, 255);
        last = temp;
    }
    last = BUFF_WIDTH * 5 / 8;
    for (int x = 0; x < (int)BUFF_BOTTOM; x++) {
        temp = ((stereo[x % BUFF_WIDTH][1] - 127) / 32) + last;
        temp = ct_clamp(temp, 0, BUFF_WIDTH - 1);
        do_hwave(temp, last, x, 255);
        last = temp;
    }
}

// Dot — vertical, small
static void wave_dot_vs(void)
{
    int half = BUFF_WIDTH / 2;
    for (int x = 0; x < (int)BUFF_BOTTOM; x++) {
        int idx = x % BUFF_WIDTH;
        int temp = stereo[idx][0];
        buff[x * BUFF_WIDTH + ct_clamp(half - (temp >> 2), 0, BUFF_WIDTH - 1)] =
            table[curtable][temp & 0xFF];
        temp = stereo[idx][1];
        buff[x * BUFF_WIDTH + ct_clamp(half + (temp >> 2), 0, BUFF_WIDTH - 1)] =
            table[curtable][temp & 0xFF];
    }
}

// Fireflies — audio-steered random walk
static void wave_fireflies(void)
{
    static int xoff0 = BUFF_WIDTH / 2, yoff0 = BUFF_HEIGHT / 2;
    static int xoff1 = BUFF_WIDTH / 2, yoff1 = BUFF_HEIGHT / 2;

    xoff0 += stereo[0][0] % 9 - 4;
    yoff0 += stereo[1 % BUFF_WIDTH][0] % 9 - 4;
    xoff1 += stereo[0][1] % 9 - 4;
    yoff1 += stereo[1 % BUFF_WIDTH][1] % 9 - 4;

    if (xoff0 < 0) xoff0 += BUFF_WIDTH;
    if (yoff0 < 0) yoff0 += BUFF_BOTTOM;
    if (xoff1 < 0) xoff1 += BUFF_WIDTH;
    if (yoff1 < 0) yoff1 += BUFF_BOTTOM;

    xoff0 %= BUFF_WIDTH;  yoff0 %= BUFF_HEIGHT;
    xoff1 %= BUFF_WIDTH;  yoff1 %= BUFF_HEIGHT;

    for (int x = 0; x < (int)BUFF_WIDTH; x++) {
        int temp = stereo[x][0];
        int temp2 = stereo[(x + 80) % BUFF_WIDTH][0];
        int py = ((temp >> 2) + yoff0) % BUFF_BOTTOM;
        int px = ((temp2 >> 2) + xoff0) % BUFF_WIDTH;
        buff[py * BUFF_WIDTH + px] = table[curtable][temp & 0xFF];

        temp = stereo[x][1];
        temp2 = stereo[(x + 80) % BUFF_WIDTH][1];
        py = ((temp >> 2) + yoff1) % BUFF_BOTTOM;
        px = ((temp2 >> 2) + xoff1) % BUFF_WIDTH;
        buff[py * BUFF_WIDTH + px] = table[curtable][temp & 0xFF];
    }
}

// Pete's sine wave
static void wave_pete(void)
{
    int left = 0, right = 0;
    int half = BUFF_WIDTH / 2;

    for (int x = 0; x < (int)BUFF_WIDTH; x++) {
        left  += abs(stereo[x][0] - 128);
        right += abs(stereo[x][1] - 128);
    }
    left  = ct_min(left / half, (int)BUFF_BOTTOM);
    right = ct_min(right / half, (int)BUFF_BOTTOM);

    for (int x = 0; x < (int)half; x++) {
        int temp = stereo[x][0];
        int py = ct_clamp(BUFF_BOTTOM - (abs(left * sine_table[x]) >> 8), 0, BUFF_HEIGHT - 1);
        buff[py * BUFF_WIDTH + x] = table[curtable][temp & 0xFF];
    }
    for (int x = half; x < (int)BUFF_WIDTH; x++) {
        int temp = stereo[x][1];
        int py = ct_clamp(BUFF_BOTTOM - (abs(right * sine_table[x]) >> 8), 0, BUFF_HEIGHT - 1);
        buff[py * BUFF_WIDTH + x] = table[curtable][temp & 0xFF];
    }
}

// Pete variant 2 — sine-modulated color
static void wave_pete2(void)
{
    int half = BUFF_WIDTH / 2;
    for (int x = 0; x < (int)BUFF_BOTTOM; x++) {
        int idx = x % BUFF_WIDTH;
        int temp = stereo[idx][0];
        int px = ct_clamp(half - (temp >> 2), 0, BUFF_WIDTH - 1);
        buff[x * BUFF_WIDTH + px] = table[curtable][sine_table[temp & 0xFF] & 0xFF];

        temp = stereo[idx][1];
        px = ct_clamp(half + (temp >> 2), 0, BUFF_WIDTH - 1);
        buff[x * BUFF_WIDTH + px] = table[curtable][sine_table[temp & 0xFF] & 0xFF];
    }
}

// Zippy 1 — fractal-like random walk driven by audio derivative
static void wave_zippy1(void)
{
    static int xoff0 = BUFF_WIDTH / 2, yoff0 = BUFF_HEIGHT / 2;
    static int xoff1 = BUFF_WIDTH / 2, yoff1 = BUFF_HEIGHT / 2;

    int temp = stereo[0][0];
    for (int x = 0; x < (int)BUFF_WIDTH - 2; x += 2) {
        xoff0 += (stereo[x][0] - temp) >> 1;
        temp = stereo[x][0];
        if (xoff0 < 0) xoff0 = BUFF_WIDTH - 1;
        xoff0 %= BUFF_WIDTH;
        buff[yoff0 * BUFF_WIDTH + xoff0] = table[curtable][temp & 0xFF];

        yoff0 += (stereo[x + 1][0] - temp) >> 1;
        temp = stereo[x + 1][0];
        if (yoff0 < 0) yoff0 = BUFF_HEIGHT - 1;
        yoff0 %= BUFF_HEIGHT;
        buff[yoff0 * BUFF_WIDTH + xoff0] = table[curtable][temp & 0xFF];
    }

    temp = stereo[0][1];
    for (int x = 0; x < (int)BUFF_WIDTH - 2; x += 2) {
        xoff1 += (stereo[x][1] - temp) >> 1;
        temp = stereo[x][1];
        if (xoff1 < 0) xoff1 = BUFF_WIDTH - 1;
        xoff1 %= BUFF_WIDTH;
        buff[yoff1 * BUFF_WIDTH + xoff1] = table[curtable][temp & 0xFF];

        yoff1 -= (stereo[x + 1][1] - temp) >> 1;
        temp = stereo[x + 1][1];
        if (yoff1 < 0) yoff1 = BUFF_HEIGHT - 1;
        yoff1 %= BUFF_HEIGHT;
        buff[yoff1 * BUFF_WIDTH + xoff1] = table[curtable][temp & 0xFF];
    }
}

// Zippy 2 — like zippy1 but full derivative (no >>1)
static void wave_zippy2(void)
{
    static int xoff0 = BUFF_WIDTH / 2, yoff0 = BUFF_HEIGHT / 2;
    static int xoff1 = BUFF_WIDTH / 2, yoff1 = BUFF_HEIGHT / 2;

    int temp = stereo[0][0];
    for (int x = 0; x < (int)BUFF_WIDTH - 2; x += 2) {
        xoff0 += (stereo[x][0] - temp);
        temp = stereo[x][0];
        if (xoff0 < 0) xoff0 = BUFF_WIDTH - 1;
        xoff0 %= BUFF_WIDTH;
        buff[yoff0 * BUFF_WIDTH + xoff0] = table[curtable][temp & 0xFF];

        yoff0 += (stereo[x + 1][0] - temp);
        temp = stereo[x + 1][0];
        if (yoff0 < 0) yoff0 = BUFF_HEIGHT - 1;
        yoff0 %= BUFF_HEIGHT;
        buff[yoff0 * BUFF_WIDTH + xoff0] = table[curtable][temp & 0xFF];
    }

    temp = stereo[0][1];
    for (int x = 0; x < (int)BUFF_WIDTH - 2; x += 2) {
        xoff1 += (stereo[x][1] - temp);
        temp = stereo[x][1];
        if (xoff1 < 0) xoff1 = BUFF_WIDTH - 1;
        xoff1 %= BUFF_WIDTH;
        buff[yoff1 * BUFF_WIDTH + xoff1] = table[curtable][temp & 0xFF];

        yoff1 -= (stereo[x + 1][1] - temp);
        temp = stereo[x + 1][1];
        if (yoff1 < 0) yoff1 = BUFF_HEIGHT - 1;
        yoff1 %= BUFF_HEIGHT;
        buff[yoff1 * BUFF_WIDTH + xoff1] = table[curtable][temp & 0xFF];
    }
}

// Test wave — Pete's variant with different scaling
static void wave_test(void)
{
    int left = 0, right = 0;
    int half = BUFF_WIDTH / 2;

    for (int x = 0; x < (int)BUFF_WIDTH; x++) {
        left  += abs(stereo[x][0] - 128);
        right += abs(stereo[x][1] - 128);
    }
    left  = ct_min(left / 128, (int)BUFF_BOTTOM);
    right = ct_min(right / 128, (int)BUFF_BOTTOM);

    for (int x = 0; x < (int)half; x++) {
        int temp = stereo[x][0];
        int py = ct_clamp(BUFF_BOTTOM - (abs(left * sine_table[x]) >> 8), 0, BUFF_HEIGHT - 1);
        buff[py * BUFF_WIDTH + x] = table[curtable][temp & 0xFF];
    }
    for (int x = half; x < (int)BUFF_WIDTH; x++) {
        int temp = stereo[x][1];
        int py = ct_clamp(BUFF_BOTTOM - (abs(right * sine_table[x]) >> 8), 0, BUFF_HEIGHT - 1);
        buff[py * BUFF_WIDTH + x] = table[curtable][temp & 0xFF];
    }
}

// Moles Fractal — two particles steered by audio first-derivative (half-steps).
// Each frame processes 119 sample pairs; x and y positions shift by half the
// change between adjacent samples, scattering colored pixels anywhere in the
// buffer. Left and right channels drive independent particles; right channel's
// y-axis is mirrored so the two trails move in complementary directions.
// Port of molesFract() from Cthugha v5.3 via cthugha-js.
static void wave_moles1(void)
{
    static int xoff0 = BUFF_WIDTH  / 2, yoff0 = BUFF_HEIGHT / 2;
    static int xoff1 = BUFF_WIDTH  / 2, yoff1 = BUFF_HEIGHT / 2;
    int temp;

    temp = stereo[0][0];
    for (int x = 0; x < (int)BUFF_WIDTH - 2; x += 2) {
        xoff0 += (stereo[x    ][0] - temp) >> 1;  temp = stereo[x    ][0];
        if (xoff0 < 0) xoff0 += BUFF_WIDTH;
        xoff0 %= BUFF_WIDTH;
        buff[yoff0 * BUFF_WIDTH + xoff0] = table[curtable][temp & 0xFF];

        yoff0 += (stereo[x + 1][0] - temp) >> 1;  temp = stereo[x + 1][0];
        if (yoff0 < 0) yoff0 += BUFF_HEIGHT;
        yoff0 %= BUFF_HEIGHT;
        buff[yoff0 * BUFF_WIDTH + xoff0] = table[curtable][temp & 0xFF];
    }

    temp = stereo[0][1];
    for (int x = 0; x < (int)BUFF_WIDTH - 2; x += 2) {
        xoff1 += (stereo[x    ][1] - temp) >> 1;  temp = stereo[x    ][1];
        if (xoff1 < 0) xoff1 += BUFF_WIDTH;
        xoff1 %= BUFF_WIDTH;
        buff[yoff1 * BUFF_WIDTH + xoff1] = table[curtable][temp & 0xFF];

        yoff1 -= (stereo[x + 1][1] - temp) >> 1;  temp = stereo[x + 1][1];
        if (yoff1 < 0) yoff1 += BUFF_HEIGHT;
        yoff1 %= BUFF_HEIGHT;
        buff[yoff1 * BUFF_WIDTH + xoff1] = table[curtable][temp & 0xFF];
    }
}

// Moles Fractal 2 — same as wave_moles1 but full derivative steps (no >> 1).
// 2× more volatile: gentle audio gives tight delicate clusters, transients
// send the particle flying across the entire buffer in a single frame.
static void wave_moles2(void)
{
    static int xoff0 = BUFF_WIDTH  / 2, yoff0 = BUFF_HEIGHT / 2;
    static int xoff1 = BUFF_WIDTH  / 2, yoff1 = BUFF_HEIGHT / 2;
    int temp;

    temp = stereo[0][0];
    for (int x = 0; x < (int)BUFF_WIDTH - 2; x += 2) {
        xoff0 += stereo[x    ][0] - temp;  temp = stereo[x    ][0];
        if (xoff0 < 0) xoff0 += BUFF_WIDTH;
        xoff0 %= BUFF_WIDTH;
        buff[yoff0 * BUFF_WIDTH + xoff0] = table[curtable][temp & 0xFF];

        yoff0 += stereo[x + 1][0] - temp;  temp = stereo[x + 1][0];
        if (yoff0 < 0) yoff0 += BUFF_HEIGHT;
        yoff0 %= BUFF_HEIGHT;
        buff[yoff0 * BUFF_WIDTH + xoff0] = table[curtable][temp & 0xFF];
    }

    temp = stereo[0][1];
    for (int x = 0; x < (int)BUFF_WIDTH - 2; x += 2) {
        xoff1 += stereo[x    ][1] - temp;  temp = stereo[x    ][1];
        if (xoff1 < 0) xoff1 += BUFF_WIDTH;
        xoff1 %= BUFF_WIDTH;
        buff[yoff1 * BUFF_WIDTH + xoff1] = table[curtable][temp & 0xFF];

        yoff1 -= stereo[x + 1][1] - temp;  temp = stereo[x + 1][1];
        if (yoff1 < 0) yoff1 += BUFF_HEIGHT;
        yoff1 %= BUFF_HEIGHT;
        buff[yoff1 * BUFF_WIDTH + xoff1] = table[curtable][temp & 0xFF];
    }
}

function_opt wavearray[] = {
    { wave_dot_hs,     WHEN_ALWAYS, "Dot HS"      },
    { wave_dot_hl,     WHEN_ALWAYS, "Dot HL"      },
    { wave_line_vw,    WHEN_ALWAYS, "Line VW"     },
    { wave_spike_s,    WHEN_ALWAYS, "Spike S"     },
    { wave_spike_l,    WHEN_ALWAYS, "Spike L"     },
    { wave_line_hs,    WHEN_ALWAYS, "Line HS"     },
    { wave_line_hl,    WHEN_ALWAYS, "Line HL"     },
    { wave_dot_vl,     WHEN_ALWAYS, "Dot VL"      },
    { wave_spike,      WHEN_ALWAYS, "Spike"       },
    { wave_walking,    WHEN_ALWAYS, "Walking"     },
    { wave_falling,    WHEN_ALWAYS, "Falling"     },
    { wave_lissa,      WHEN_ALWAYS, "Lissa"       },
    { wave_line_vs,    WHEN_ALWAYS, "Line VS"     },
    { wave_line_vl,    WHEN_ALWAYS, "Line VL"     },
    { wave_line_x,     WHEN_ALWAYS, "Line X"      },
    { wave_lightning1, WHEN_ALWAYS, "Lightning1"  },
    { wave_lightning2, WHEN_ALWAYS, "Lightning2"  },
    { wave_dot_vs,     WHEN_ALWAYS, "Dot VS"      },
    { wave_fireflies,  WHEN_ALWAYS, "FireFlies"   },
    { wave_pete,       WHEN_ALWAYS, "Pete"        },
    { wave_pete2,      WHEN_ALWAYS, "Pete2"       },
    { wave_zippy1,     WHEN_ALWAYS, "Zippy 1"     },
    { wave_zippy2,     WHEN_ALWAYS, "Zippy 2"     },
    { wave_test,       WHEN_ALWAYS, "Zaph Test"   },
    { wave_moles1,     WHEN_ALWAYS, "Moles 1"     },
    { wave_moles2,     WHEN_ALWAYS, "Moles 2"     },
    { NULL,            WHEN_NEVER,  "<END>"       }
};

void change_wave(int wavenum)
{
    if (numwaves < 0) {
        numwaves = 0;
        while (wavearray[numwaves].function != NULL)
            numwaves++;
    }
    if (numwaves == 0) return;

    usewave = ((wavenum % numwaves) + numwaves) % numwaves;
    wave = wavearray[usewave].function;
}

void next_wave(void)
{
    change_wave((usewave + 1) % numwaves);
}
