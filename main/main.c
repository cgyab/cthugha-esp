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
#include "sdcard_record.h"

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
static int startup_hold   = 900;  // frames to hold lm on boot before randomizer
static int lm_timer = 0;    // frames remaining in a touch-triggered lm display

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

// Enforce exclusive-wave constraints: when the active wave requires an upright,
static void apply_wave_constraints(void)
{
    if (wave_is_exclusive())
        translate_idx = 0;  // no spatial warps — they distort the logo
}

static void randomize_full(void)
{
    static const int speeds[] = {5, 10, 20};
    if (!lock_flame)   curflame = change_flame(esp_random());
    if (!lock_wave) {
        change_wave(esp_random() % numwaves_random);
        apply_wave_constraints();
    }
    if (!lock_palette) fill_lut_buffer(esp_random() % numluts);
    if (!wave_is_exclusive()) {
        if (!lock_display) curdisplay = change_display(esp_random() % numdisplays);
        if (!lock_translate && nrtrans && !(esp_random() % 5))
            translate_idx = esp_random() % nrtrans;
    }
    curtable      = esp_random() % NUMTABLES;
    use_fft       = !(esp_random() % 4);
    use_pal_cycle = !(esp_random() % 4);
    if (use_pal_cycle) pal_cycle_speed = speeds[esp_random() % 3];
    use_alignment = !(esp_random() % 2);
    if (!lock_boom) boom_boxes_randomize();
}

static void randomize_all(void)
{
    // Tiered randomization — keeps a partial theme alive between full overhauls.
    // roll 0-3 (40%): full — change everything
    // roll 4-5 (20%): color refresh — flame + palette + wave table + pal-cycle
    // roll 6-7 (20%): content refresh — wave + boom + fft + alignment
    // roll 8-9 (20%): geometry refresh — display + translation
    static const int speeds[] = {5, 10, 20};
    int roll = (int)(esp_random() % 10);

    if (roll < 4) {
        randomize_full();

    } else if (roll < 6) {
        // Color refresh: flame + palette + wave table + pal-cycle
        if (!lock_flame)   curflame = change_flame(esp_random());
        if (!lock_palette) fill_lut_buffer(esp_random() % numluts);
        curtable      = esp_random() % NUMTABLES;
        use_pal_cycle = !(esp_random() % 4);
        if (use_pal_cycle) pal_cycle_speed = speeds[esp_random() % 3];

    } else if (roll < 8) {
        // Content refresh: wave + boom + fft + alignment
        if (!lock_wave) {
            change_wave(esp_random() % numwaves_random);
            apply_wave_constraints();
        }
        if (!lock_boom) boom_boxes_randomize();
        use_fft       = !(esp_random() % 4);
        use_alignment = !(esp_random() % 2);

    } else {
        // Geometry refresh: display mode + translation
        if (!wave_is_exclusive()) {
            if (!lock_display) curdisplay = change_display(esp_random() % numdisplays);
            if (!lock_translate && nrtrans && !(esp_random() % 5))
                translate_idx = esp_random() % nrtrans;
        }
    }
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

            case TOUCH_TWO_FINGER_SWIPE_DOWN:
                sdcard_record_toggle();
                break;

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

            case TOUCH_FIVE_FINGER_TAP:
                // Falls through to shared lm handler below.
                goto lm_trigger;

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
                change_wave((usewave + 1) % numwaves_random);
                apply_wave_constraints();
                break;

            case TOUCH_SWIPE_RIGHT:
                curflame = change_flame(curflame + 1);
                break;

            case TOUCH_SWIPE_LEFT:
                fill_lut_buffer((curpal + 1) % numluts);
                break;

            case TOUCH_SWIPE_UP:
                if (!wave_is_exclusive())
                    curdisplay = change_display((curdisplay + 1) % numdisplays);
                break;

            case TOUCH_SWIPE_DOWN:
                if (!wave_is_exclusive() && nrtrans > 0)
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

            case TOUCH_TWO_FINGER_SWIPE_DOWN:
                sdcard_record_toggle();
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
                apply_wave_constraints();
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

            case TOUCH_FIVE_FINGER_TAP:
            lm_trigger:
                // lm: show the logo for 15 s then randomize_all().
                // Works from any state — unlocks, overrides startup hold.
                locked        = 0;
                startup_hold  = 0;
                curflame      = change_flame(4);      // Up Subtle
                change_wave(28);                      // lm
                apply_wave_constraints();             // force trans=0
                curdisplay    = change_display(2);    // Shift Down
                boom_boxes_randomize();
                boom_boxes_active = 1;
                lm_timer = 900;                 // ~15 s @ 60 fps
                ESP_LOGI(TAG, "lm: 15 s display triggered");
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

    while (1) {
        // Auto-change timer.
        // startup_hold: suppresses randomizer on first boot (~15 s).
        // lm_timer: suppresses randomizer during a touch-triggered lm
        //   display; fires randomize_all() when it expires.
        if (startup_hold > 0) {
            if (--startup_hold == 0) {
                randomize_full();
                count = (esp_random() % rand_time) + min_time;
            }
        } else if (lm_timer > 0) {
            if (--lm_timer == 0)
                randomize_full();
        } else if (count <= 0 && !locked) {
            randomize_all();
            count = (esp_random() % rand_time) + min_time;
        }
        if (!locked) count--;

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
                if (startup_hold <= 0) count = 0; // trigger change (not during startup)
            }
            quiet = 0;
        } else {
            if (quiet < 10000) quiet++;
            // Reseed a row every ~3s of silence so the visualization doesn't fade to black.
            // Skip during exclusive waves (e.g. lm) — the bottom stripe would
            // show as an artifact against the black background.
            if (quiet % 90 == 0 && !wave_is_exclusive()) {
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
        // frames where the max pixel index in buff is below the darkness threshold,
        // BUT ONLY when audio is present (quiet < 60). Silence naturally fades the buffer
        // to black; that's expected and not a render issue. Only log when audio was
        // present within the last second so the log reflects genuine rendering problems.
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
                if (blank_frames == 30 && quiet < 60 && !boom_boxes_active && !wave_is_exclusive()) {
                    boom_boxes_randomize();
                    boom_boxes_active = 1;
                    ESP_LOGI(TAG, "BLANK %d frames: auto-enabled boom boxes", blank_frames);
                }
                if (blank_frames == 60 && quiet < 60) {
                    int tidx = ct_clamp(translate_idx, 0, 7);
                    ESP_LOGW(TAG, "BLANK %d frames (quiet=%d): flame=%d(%s) wave=%d(%s) "
                             "disp=%d(%s) pal=%d(%s) trans=%d(%s) fft=%d palcyc=%d align=%d",
                             blank_frames, quiet,
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

        // Capture frame to SD card if recording is active
        sdcard_record_frame(buff);

        // Scale and send to LCD
        display_render();

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

    // Start in lm mode: logo pulse seeding flame edges.
    // The randomizer is suppressed for 15 s (900 frames @ ~60 fps) so the
    // logo runs exclusively on startup. After that normal randomization takes
    // over and lm becomes one wave in the rotation.
    curflame   = change_flame(4);        // Up Subtle — gentle upward wisps from edge seeds
    change_wave(28);                     // lm
    apply_wave_constraints();            // force trans=0
    curdisplay = change_display(2);      // Shift Down — drip effect with upward flame
    fill_lut_buffer(2);                  // Ocean palette — black → deep blue → cyan → white
    boom_boxes_randomize();
    boom_boxes_active = 1;               // always on for startup

    // Initialize hardware — touch first, audio second (audio shares the I2C bus)
    display_init();
    touch_input_init();
    audio_capture_init();
    sdcard_record_init();

    ESP_LOGI(TAG, "Starting render loop");

    // Run render on core 0, audio capture runs inline
    xTaskCreatePinnedToCore(render_task, "render", 8192, NULL, 5,
                            NULL, 0);
}
