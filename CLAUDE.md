# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

ESP32-P4 port of Cthugha v5.3, a real-time audio visualization program ("An Oscilloscope on Acid"). Originally a DOS program by Zaph / Digital Aasvogel Group / Torps Productions (1993-1995), ported to run on the **ESP32-P4-WIFI6-Touch-LCD-4B** (Waveshare) development board.

The original DOS source (v5.3) lives at `../cthug53s` for reference.

## Target Board

**ESP32-P4-WIFI6-Touch-LCD-4B** — dual-core RISC-V @ 360 MHz, 32 MB PSRAM, 32 MB flash.

| Function | GPIO | Notes |
|----------|------|-------|
| LCD Backlight | 26 | **Active LOW** (0 = on) |
| LCD Reset | 27 | ST7703 hardware reset |
| I2S MCLK | 13 | Master clock, 384× sample rate |
| I2S BCLK | 12 | Bit clock |
| I2S WS | 10 | Word select |
| I2S DIN | 11 | Mic data in |
| I2S DOUT | 9 | Speaker data out (unused) |
| PA Enable | 53 | Speaker amp enable (unused) |
| I2C SDA | 7 | GT911 touch + other I2C |
| I2C SCL | 8 | |
| Touch RST | 5 | GT911 reset |
| Touch INT | 6 | GT911 interrupt |

## Build

Requires **ESP-IDF v5.5.1+** targeting ESP32-P4.

```bash
idf.py set-target esp32p4
idf.py build
idf.py -p PORT flash monitor
```

GPIO pins are configurable via `idf.py menuconfig` → "Cthugha Configuration", but defaults match this board.

First build downloads managed components automatically: `waveshare/esp_lcd_st7703`, `espressif/esp_lcd_touch_gt911`.

## Architecture

### Rendering Pipeline

Internal framebuffer is 240×240 @ 8-bit indexed color (57,600 bytes), scaled 3× to 720×720 RGB565 for the LCD. Each frame in the render loop (`render_task` in main.c, pinned to core 0):

1. **Translation** (translate.c) — optional spatial remapping via precomputed uint16_t lookup tables. 7 procedural effects.
2. **Flame** (flames.c) — scrolls/blurs the buffer via directional pixel averaging through `divsub[]` lookup table. 15 effects ported from original x86 inline assembly.
3. **Audio + Wave** (audio_capture.c, waves.c) — reads stereo I2S from ES7210 ADC codec into `stereo[240][2]` as 0–255 normalized samples, then maps audio data onto the buffer as visual patterns. 28 randomizable wave renderers.
4. **Boom Boxes** (boom_box.c) — two colored squares bounce around the buffer seeding pixels that flame propagates. Audio-reactive size. Randomly activated by `randomize_all()`. Novel feature from the JS port; not in original v5.3.
5. **Display effect** (display.c) — buffer transforms before output (mirror, rotate, kaleidoscope). 10 modes from DISPLAY.C.
6. **LCD output** (display.c `display_render()`) — palette lookup into `pal_lut[256]` (RGB565), 3× nearest-neighbor scaling, written directly into DPI panel framebuffer, then `draw_bitmap` to swap buffers on vsync.

### Display Driver (display.c)

Uses `waveshare/esp_lcd_st7703` managed component. Key initialization sequence:

1. LDO channel 3 @ 2500 mV powers MIPI DSI PHY
2. DSI bus via `ST7703_PANEL_BUS_DSI_2CH_CONFIG()` macro
3. DBI command IO via `ST7703_PANEL_IO_DBI_CONFIG()` macro
4. DPI config via `ST7703_720_720_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB565)`
5. Panel created with `esp_lcd_new_panel_st7703()`, vendor config has `.use_mipi_interface = 1`
6. Vsync sync via `on_color_trans_done` ISR callback → semaphore
7. Framebuffers obtained via `esp_lcd_dpi_panel_get_frame_buffer()` (DMA-capable memory, **not** PSRAM — PSRAM buffers cause draw_bitmap to hang because DMA can't reach them)

The render loop writes directly into the back framebuffer, then calls `draw_bitmap` with that pointer — the DPI panel recognizes its own buffer and swaps without copying.

### Audio Capture (audio_capture.c)

The board has **two audio codec chips** sharing the same I2S bus:
- **ES8311** (I2C addr 0x18) — DAC/speaker output only. Has no real ADC. Do not attempt to record from it; all reads return zeros.
- **ES7210** (I2C addr 0x40, 7-bit) — 4-channel ADC/microphone codec. This is the mic source.

Both are initialized via the **new I2C master API** (`driver/i2c_master.h`) and share the I2C bus already created by `touch_input_init()`. The bus handle is exposed via `touch_get_i2c_bus()`.

I2S runs **full-duplex stereo**, 16-bit, 16 kHz. MCLK multiply = 384×. TX drives DOUT → ES8311 DAC (silence). RX reads DIN ← ES7210 SDOUT (MIC1=L, MIC2=R). Uses `i2s_channel_init_std_mode()` (the `_rx_mode` variant was removed in ESP-IDF v5.5).

A **MAV-based AGC** (mean absolute value, not peak) tracks average signal energy with fast attack (α=0.3) and slow decay (α=0.002 ≈ 10s half-life), targeting output MAV ≈ 50 counts from the 0–255 midpoint.

### Touch Input (touch_input.c)

GT911 capacitive touch over I2C master bus, up to 5 simultaneous touch points. Gesture detection: tap, swipe (L/R/U/D), long press, two/three/four/five-finger taps. Gestures mapped to mode changes in `handle_touch()` in main.c.

### Key Data Structures

- `buff` / `shadow` — two 240×240 uint8_t framebuffers, swapped via `flip_screens()`
- `divsub[1024]` — flame fade LUT: `divsub[i] = max(0, i/4 - 1)`. "slow" flames sum 4 neighbors (0–1020 range), "subtle" variants use wrapping uint8_t sums (0–255)
- `table[10][256]` — audio-to-color mapping curves (V-shape, ramp, inverse, threshold, etc.)
- `stereo[240][2]` — per-pixel audio amplitude, 0–255
- `LUTbuffer[768]` — current palette (256 × RGB888)
- `pal_lut[256]` — palette converted to RGB565 for LCD output
- `sine_table[240]` — precomputed sine values for wave effects
- `lcd_fb[2]` — DPI panel's own double-buffered 720×720 RGB565 framebuffers
- `boom_boxes[2]` — bouncing colored square state (position, velocity, color, size per box)
- `boom_boxes_active` — whether boom boxes are currently painting into buff this cycle

### Effect Selection

All effects use the `function_opt` struct pattern: array of `{function_ptr, flag, name}` terminated by a NULL entry. Function pointer tables: `flamearray[]`, `wavearray[]`, `disparray[]`. Modes cycle via `change_flame()`, `change_wave()`, `change_display()`.

Wave entries carry a `flag_when` field: `WHEN_ALWAYS` (eligible for randomizer and tap cycling) or `WHEN_NEVER` (startup/recall only — unreachable by randomizer or tap). `change_wave()` initializes two counts lazily on first call:
- `numwaves` — total non-null entries in `wavearray[]`
- `numwaves_random` — count of `WHEN_ALWAYS` entries only

Both `randomize_all()` and the tap cycling handler use `numwaves_random` as the modulus, ensuring `WHEN_NEVER` waves are never accidentally selected.

`wave_is_exclusive()` returns true when the active wave requires the upright full-frame view (no display transforms, no translation). `randomize_all()` and the silence reseeder both check this before touching display/translate state.

### Startup Sequence and Timers

On boot, `app_main` sets a specific startup configuration before starting the render task. Two file-scope counters in the render loop gate the randomizer:

- `startup_hold` — counts down from 900 (~15 s at 60 fps). While positive, the randomizer is suppressed so the startup effect runs uninterrupted.
- `lm_timer` — set to 900 by the five-finger tap gesture. When it expires, `randomize_all()` fires. Only one of startup_hold/lm_timer is active at a time.

### Blank Screen Recovery

The render loop samples the maximum pixel value in `buff` each frame. If the max stays below 10 for 30 consecutive frames while audio is present and boom boxes are off (and not in an exclusive wave), boom boxes are automatically enabled as a lightweight self-healing step. If the screen remains blank at 60 frames, a `BLANK` warning is logged with the full effect state for diagnostics.

## Key Constants (cthugha.h)

- `BUFF_WIDTH` = 240, `BUFF_HEIGHT` = 240, `BUFF_SIZE` = 57600, `BUFF_BOTTOM` = 236
- `NUMTABLES` = 10, `LUTSIZE` = 768, `MAXLUTS` = 30

## Key Constants (display.h)

- `LCD_H_RES` = 720, `LCD_V_RES` = 720, `LCD_BPP` = 16, `SCALE_FACTOR` = 3

## sdkconfig.defaults

ESP32-P4 @ 360 MHz, SPIRAM @ 200 MHz, `IDF_EXPERIMENTAL_FEATURES` enabled (required for MIPI-DSI), task watchdog disabled, FreeRTOS @ 1000 Hz, perf-optimized compiler.

## Pitfalls / Lessons Learned

- **PSRAM buffers + DPI draw_bitmap**: Allocating the LCD render buffer from PSRAM (`MALLOC_CAP_SPIRAM`) causes `draw_bitmap` to never complete — the DMA transfer-done callback never fires and the render loop blocks forever on `xSemaphoreTake`. Always use the panel's own framebuffers via `esp_lcd_dpi_panel_get_frame_buffer()`.
- **I2S API v5.5**: `i2s_channel_init_std_rx_mode()` was removed in ESP-IDF v5.5 — use `i2s_channel_init_std_mode()`.
- **Backlight polarity**: LCD backlight is active LOW on this board (GPIO 26 = 0 to turn on).
- **Kconfig vs hardcoded**: LCD resolution (720×720) and MIPI-DSI timing are hardcoded via ST7703 macros, not Kconfig — the panel driver handles all timing. Only GPIO pins and I2S sample rate are in Kconfig.
- **sdkconfig v5.5.1 incompatibilities**: `SPIRAM_MODE_QUAD`, `SPIRAM_FETCH_INSTRUCTIONS`, `SPIRAM_RODATA` don't exist on this target/version. CPU max is 360 MHz (not 400).
- **Two audio codecs, one I2S bus**: ES8311 (0x18) is DAC-only — reading from it yields all zeros. ES7210 (0x40) is the mic ADC. Both share GPIO 7/8 I2C bus with GT911 touch. Must use new I2C master API (`driver/i2c_master.h`) for both; the old `driver/i2c.h` API cannot share the same port.
- **Touch init must precede audio init**: `audio_capture_init()` calls `touch_get_i2c_bus()` to attach ES7210 to the shared bus. If audio init runs before touch init, the bus handle is NULL and the ES7210 attach will abort.
- **GT911 max touch points**: The GT911 supports 5 simultaneous points. Arrays and the `max_points` argument to `esp_lcd_touch_get_coordinates()` must be size 5 to detect five-finger gestures; size 4 silently drops the fifth touch.
- **Translation effects are not ported from original**: The original Cthugha v5.3 loaded precomputed `.tab` binary files (320×200 pixel maps). We have no `.tab` files; all 7 spatial effects are procedurally generated from scratch for the 240×240 square buffer.
- **Fisheye was barrel distortion**: The original `gen_fisheye` used `nr = r*r` which pulls all content toward the center (dark area) — a barrel distortion. Fixed to `nr = sqrtf(r)` which reads from outer/bottom areas where flame content lives.
- **90° rotation effects read from wrong half of buffer**: The original port sampled `buff[x*W+y]` for x=0..W/2-1, covering only rows 0–119 (top half, where content is dark/old). Wave seeds rows 236–239 (bottom). Fixed to `buff[(W-1-x)*W+y]` so both halves read from rows 120–239, including the seeded area.
- **WHEN_NEVER waves reachable via tap cycling if using numwaves**: The tap gesture and `next_wave()` must use `numwaves_random` as the modulus, not `numwaves`. Using `numwaves` allows the index to reach WHEN_NEVER entries at the end of the array.
- **Long-run integer safety**: Counters that increment or decrement every frame can overflow on multi-month runs. `rain_frame` in `wave_raindrops` must be `uint32_t` (signed overflow → negative → OOB array index crash). The render-loop `count` must only decrement when `!locked` (unbounded decrement when locked → signed overflow UB). The `quiet` counter is capped at 10000 to prevent `quiet % 90` going negative post-overflow.
