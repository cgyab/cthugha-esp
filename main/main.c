//
// Cthugha ESP32-P4 Port — Main application
// Audio-seeded real-time visualization
//
// Original: Zaph, Digital Aasvogel Group, Torps Productions 1993-1995
// Port: ESP32-P4 with 720x720 MIPI-DSI LCD and I2S MEMS microphone
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "cthugha.h"
#include "display.h"
#include "audio_capture.h"
#include "touch_input.h"
#include "boom_box.h"

static const char *TAG = "cthugha";

// --- Global state ---
static uint8_t buff_a[BUFF_SIZE];
static uint8_t buff_b[BUFF_SIZE];
uint8_t *buff   = buff_a;
uint8_t *shadow = buff_b;

int table[NUMTABLES][256];
int curtable = 0;
int min_time = 200;
int rand_time = 750;
int curflame = 0;

static int locked = 0;

// Per-axis locks — respected by randomize_all() and the auto-timer.
// Set to 1 = that axis won't change during randomization.
// Configured by gestures while globally locked (long press).
static int lock_flame     = 0;
static int lock_wave      = 0;
static int lock_palette   = 0;
static int lock_display   = 0;
static int lock_translate = 0;
static int lock_boom      = 0;

static int all_axes_locked(void) {
    return lock_flame && lock_wave && lock_palette
        && lock_display && lock_translate && lock_boom;
}
static void set_all_axis_locks(int v) {
    lock_flame = lock_wave = lock_palette =
    lock_display = lock_translate = lock_boom = v;
}
static int quiet_change = 1;
static int was_quiet = 0;
static int use_fft = 0; // palette-morph mode: blends adjacent palettes by audio rhythm

// --- Initialization ---

void init_tables(void)
{
    for (int j = 0; j < NUMTABLES; j++) {
        for (int i = 0; i < 256; i++) {
            switch (j) {
                default:
                case 0: table[j][i] = abs(128 - i) * 2;        break;
                case 1: table[j][i] = 255 - abs(128 - i) * 2;  break;
                case 2: table[j][i] = i;                        break;
                case 3: table[j][i] = 255 - i;                  break;
                case 4: table[j][i] = abs(128 - i) + 127;       break;
                case 5: table[j][i] = (255 - abs(128 - i) + 127) & 0xFF; break;
                case 6: table[j][i] = esp_random() & 0xFF;            break;
                case 7:
                    table[j][i] = (abs(128 - i) < 64) ? 255 : (abs(128 - i) * 4);
                    break;
                case 8: table[j][i] = abs(128 - i);             break;
                case 9: table[j][i] = 255 - abs(128 - i);       break;
            }
            table[j][i] = ct_clamp(table[j][i], 0, 255);
        }
    }
}

// Pseudo-FFT from original PETE.C: analyzes zero-crossing run lengths and
// peak-to-peak amplitudes in stereo[], builds a histogram (slab[]), then
// blends LUTbuffer entries toward adjacent palettes by 0-6 steps based on
// the histogram. display_render() picks up the modified LUTbuffer each frame.
// The result is a palette that morphs in response to audio rhythm and texture.
static void apply_fft(void)
{
    static int slab[64]; // only indices 0..31 are used; 64 for safety
    int i, got, dir, curr_dir, last_dir, last_got, lens, last_cap, next_one, amt;

    for (i = 0; i < 64; i++) slab[i] = 0;

    for (int ch = 0; ch < 2; ch++) {
        last_got  = 128;
        last_dir  = 1;
        last_cap  = 128;
        next_one  = 1;
        lens      = 0;

        for (i = 0; i < (int)BUFF_WIDTH; i++) {
            got = stereo[i][ch];
            dir = got - last_got;

            if      (dir >  1) curr_dir =  1;
            else if (dir < -1) curr_dir = -1;
            else               curr_dir = last_dir;

            if (curr_dir != last_dir) {
                if (lens > 255) lens = 255;
                slab[lens >> 3] += (lens >> 1);
                lens = 0;

                if (next_one) {
                    next_one = 0;
                    last_cap = got;
                } else {
                    next_one = 1;
                    amt = abs(last_cap - got);
                    if (amt > 127) amt = 127;
                    slab[amt >> 2]++;
                }
            } else {
                lens++;
            }

            last_dir = curr_dir;
            last_got = got;
        }
    }

    // Blend each LUTbuffer entry toward adjacent palettes by the histogram value
    uint8_t *p = LUTbuffer;
    for (i = 0; i < 256; i++) {
        int temp = slab[(255 - i) >> 3];
        if (temp > 6) temp = 6;
        const uint8_t *q = LUTfiles[(curpal + temp) % numluts] + i * 3;
        *p++ = q[0];
        *p++ = q[1];
        *p++ = q[2];
    }
    // pal_lut[] is rebuilt from LUTbuffer at the start of each display_render()
}

static void randomize_all(void)
{
    if (!lock_flame)     curflame   = change_flame(esp_random());
    if (!lock_wave)      change_wave(esp_random() % numwaves);
    if (!lock_palette)   fill_lut_buffer(esp_random() % numluts);
    if (!lock_display)   curdisplay = change_display(esp_random() % numdisplays);
    if (!lock_translate && nrtrans && !(esp_random() % 5))
        translate_idx = esp_random() % nrtrans;

    // These flags are not per-axis locked — always re-rolled
    curtable      = esp_random() % NUMTABLES;
    use_fft       = !(esp_random() % 4);
    use_pal_cycle = !(esp_random() % 4);
    if (use_pal_cycle) {
        static const int speeds[] = {5, 10, 20};
        pal_cycle_speed = speeds[esp_random() % 3];
    }
    use_alignment = !(esp_random() % 2);

    if (!lock_boom) boom_boxes_randomize();
}

// --- Touch gesture handling ---

static void handle_touch(touch_gesture_t gesture)
{
    if (locked) {
        // In locked mode gestures configure per-axis locks rather than
        // cycling effects.  The axis<→gesture mapping mirrors the unlocked
        // cycling mapping so the controls stay intuitive.
        switch (gesture) {
            case TOUCH_TAP:          lock_wave      = !lock_wave;      break;
            case TOUCH_SWIPE_RIGHT:  lock_flame     = !lock_flame;     break;
            case TOUCH_SWIPE_LEFT:   lock_palette   = !lock_palette;   break;
            case TOUCH_SWIPE_UP:     lock_display   = !lock_display;   break;
            case TOUCH_SWIPE_DOWN:   lock_translate = !lock_translate; break;
            case TOUCH_TWO_FINGER_TAP: lock_boom    = !lock_boom;      break;

            case TOUCH_LONG_PRESS: {
                // If all axes locked: full unlock + randomize.
                // If some axes unlocked: unlock global, resume with current locks.
                locked = 0;
                if (all_axes_locked()) set_all_axis_locks(0);
                randomize_all();
                break;
            }

            case TOUCH_DOUBLE_TAP:
                // Nuclear option: clear all locks and full randomize.
                set_all_axis_locks(0);
                randomize_all();
                break;

            case TOUCH_THREE_FINGER_TAP:
                use_fft = !use_fft;
                break;

            default: break;
        }

        // Log the lock state after any locked-mode gesture
        ESP_LOGI(TAG, "LOCKS [%s]: flame=%d wave=%d pal=%d disp=%d trans=%d boom=%d",
                 locked ? "locked" : "UNLOCKED",
                 lock_flame, lock_wave, lock_palette,
                 lock_display, lock_translate, lock_boom);

    } else {
        // Normal unlocked mode: gestures cycle effects.
        switch (gesture) {
            case TOUCH_TAP:
                next_wave();
                break;

            case TOUCH_SWIPE_RIGHT:
                curflame = change_flame(curflame + 1);
                break;

            case TOUCH_SWIPE_LEFT:
                fill_lut_buffer((curpal + 1) % numluts);
                break;

            case TOUCH_SWIPE_UP:
                curdisplay = change_display((curdisplay + 1) % numdisplays);
                break;

            case TOUCH_SWIPE_DOWN:
                if (nrtrans > 0)
                    translate_idx = (translate_idx + 1) % (nrtrans + 1);
                break;

            case TOUCH_LONG_PRESS: {
                // Lock everything — user configures per-axis locks from here.
                locked = 1;
                set_all_axis_locks(1);
                static const char *pal_names[] = {
                    "Royal Purple","Fire","Ocean","Acid","Sunset",
                    "Neon","Rainbow","Fire Storm","Volcano"
                };
                static const char *trans_names[] = {
                    "None","Swirl","Tunnel","Fisheye","Ripple","Moles",
                    "Downspiral","HalfWheel"
                };
                int tidx = ct_clamp(translate_idx, 0, 7);
                ESP_LOGI(TAG, "LOCKED: flame=%d(%s) wave=%d(%s) disp=%d(%s) "
                         "pal=%d(%s) trans=%d(%s) fft=%d palcyc=%d align=%d "
                         "boom=%d tblclr=%d scale=%d",
                         curflame,      flamearray[curflame].name,
                         usewave,       wavearray[usewave].name,
                         curdisplay,    disparray[curdisplay].name,
                         curpal,        curpal < 9 ? pal_names[curpal] : "?",
                         translate_idx, trans_names[tidx],
                         use_fft, use_pal_cycle, use_alignment,
                         boom_boxes_active, boom_table_color, boom_scale);
                break;
            }

            case TOUCH_DOUBLE_TAP:
                randomize_all();
                break;

            case TOUCH_TWO_FINGER_TAP:
                boom_boxes_active = !boom_boxes_active;
                break;

            case TOUCH_THREE_FINGER_TAP:
                use_fft = !use_fft;
                ESP_LOGI(TAG, "FFT %s", use_fft ? "on" : "off");
                break;

            case TOUCH_FOUR_FINGER_TAP:
                // Canonical Cthugha: upward fire with stereo oscilloscope lines.
                // Locks everything so the classic combo stays until long-pressed.
                curflame     = change_flame(3);   // Up Slow
                change_wave(5);                   // Line HS
                curdisplay   = change_display(0); // Upwards (no transform)
                translate_idx = 0;                // None
                fill_lut_buffer(1);               // Fire palette
                curtable     = 0;                 // V-shape table
                use_fft      = 0;
                use_pal_cycle = 0;
                use_alignment = 1;
                boom_boxes_active = 0;
                locked = 1;
                set_all_axis_locks(1);
                ESP_LOGI(TAG, "HOME: canonical Cthugha locked");
                break;

            default:
                break;
        }
    }
}

// --- Main render loop ---

static void render_task(void *arg)
{
    ESP_LOGI(TAG, "Render task started on core %d", xPortGetCoreID());

    // Seed the buffer so the visualization starts without audio
    for (int y = BUFF_BOTTOM - 4; y < (int)BUFF_HEIGHT; y++)
        for (int x = 0; x < (int)BUFF_WIDTH; x++)
            buff[y * BUFF_WIDTH + x] = 100 + (uint8_t)((x * 2 + y) & 0x7F);

    int count = 0;
    int quiet = 0;
    int frame = 0;

    while (1) {
        // Auto-change timer
        if (count <= 0 && !locked) {
            randomize_all();
            count = (esp_random() % rand_time) + min_time;
        }
        count--;

        // Apply translation if active
        if (translate_idx > 0)
            translate_screen();

        // Flame effect — scrolls/blurs the buffer
        flame();

        // Clear the bottom rows (wave seeding area)
        memset(buff + BUFF_BOTTOM * BUFF_WIDTH, 0, (BUFF_HEIGHT - BUFF_BOTTOM) * BUFF_WIDTH);

        // Audio capture and wave rendering
        if (get_stereo_data()) {
            if (use_fft)
                apply_fft();
            wave();
            if (quiet_change && quiet > quiet_change) {
                was_quiet = 1;
                quiet = 0;
            } else if (was_quiet) {
                was_quiet = 0;
                count = 0; // trigger change
            }
            quiet = 0;
        } else {
            quiet++;
            // Reseed a row every ~3s of silence so the visualization doesn't fade to black
            if (quiet % 90 == 0) {
                uint8_t *seed = buff + (BUFF_BOTTOM - 2) * BUFF_WIDTH;
                for (int x = 0; x < (int)BUFF_WIDTH; x++)
                    seed[x] = 80 + (uint8_t)(esp_random() & 0x7F);
            }
        }

        // Boom boxes: paint bouncing colored squares into buff (audio-reactive size)
        boom_boxes_update();

        // Apply display effect (mirroring/rotation)
        display_effect();

        // Blank screen detection — log the active effect combination after 60 consecutive
        // frames where the max pixel index in buff is below the darkness threshold.
        // Threshold of 10 catches buffers collapsed to zero/near-zero palette indices.
        {
            static const char *pal_names[] = {
                "Royal Purple","Fire","Ocean","Acid","Sunset",
                "Neon","Rainbow","Fire Storm","Volcano"
            };
            static const char *trans_names[] = {
                "None","Swirl","Tunnel","Fisheye","Ripple","Moles",
                "Downspiral","HalfWheel"
            };
            static int blank_frames = 0;
            static bool was_blank = false;
            uint8_t max_px = 0;
            for (int i = 0; i < BUFF_SIZE && max_px < 10; i++)
                if (buff[i] > max_px) max_px = buff[i];

            if (max_px < 10) {
                blank_frames++;
                if (blank_frames == 60) {
                    int tidx = ct_clamp(translate_idx, 0, 7);
                    ESP_LOGW(TAG, "BLANK %d frames: flame=%d(%s) wave=%d(%s) "
                             "disp=%d(%s) pal=%d(%s) trans=%d(%s) fft=%d palcyc=%d align=%d",
                             blank_frames,
                             curflame,      flamearray[curflame].name,
                             usewave,       wavearray[usewave].name,
                             curdisplay,    disparray[curdisplay].name,
                             curpal,        curpal < 9 ? pal_names[curpal] : "?",
                             translate_idx, trans_names[tidx],
                             use_fft, use_pal_cycle, use_alignment);
                    was_blank = true;
                }
            } else {
                if (was_blank) {
                    ESP_LOGI(TAG, "BLANK resolved after %d frames", blank_frames);
                    was_blank = false;
                }
                blank_frames = 0;
            }
        }

        // Scale and send to LCD
        display_render();

        frame++;
        // if (frame % 120 == 0)
        //     ESP_LOGI(TAG, "frame %d quiet=%d", frame, quiet);

        // Touch input polling
        touch_gesture_t gesture = touch_input_poll();
        if (gesture != TOUCH_NONE)
            handle_touch(gesture);

        // Yield to other tasks
        vTaskDelay(1);
    }
}

// --- Entry point ---

void app_main(void)
{
    ESP_LOGI(TAG, "Cthugha ESP32-P4 — An Oscilloscope on Acid");
    ESP_LOGI(TAG, "Original by Zaph / Digital Aasvogel Group / Torps Productions 1993-1995");

    // Seed RNG
    srand(esp_random());

    // Initialize core systems
    memset(buff_a, 0, BUFF_SIZE);
    memset(buff_b, 0, BUFF_SIZE);

    init_divsub();
    init_tables();
    init_sine();
    init_palettes();
    init_translate();
    boom_boxes_init();

    // Set initial random modes
    curflame = change_flame(esp_random());
    change_wave(esp_random() % 24);
    curdisplay = change_display(esp_random() % 8);
    fill_lut_buffer(esp_random() % numluts);

    // Initialize hardware — touch first, audio second (audio shares the I2C bus)
    display_init();
    touch_input_init();
    audio_capture_init();

    ESP_LOGI(TAG, "Starting render loop");

    // Run render on core 0, audio capture runs inline
    xTaskCreatePinnedToCore(render_task, "render", 8192, NULL, 5,
                            NULL, 0);
}
