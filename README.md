[![Build](https://github.com/cgyab/cthugha-esp/actions/workflows/build.yml/badge.svg)](https://github.com/cgyab/cthugha-esp/actions/workflows/build.yml)

# Cthugha ESP32-P4

An ESP32-P4 port of **Cthugha v5.3** — the classic 1993 real-time audio
visualizer ("An Oscilloscope on Acid") by Zaph / Digital Aasvogel Group /
Torps Productions.

Captures audio from an onboard MEMS microphone, processes it through the
original flame, wave, and translation effects, and renders to a 720×720
MIPI-DSI touchscreen at full framerate.

## Target Hardware

**Board:** ESP32-P4-WIFI6-Touch-LCD-4B (Waveshare)

| Component | Spec |
|-----------|------|
| MCU | ESP32-P4, dual-core RISC-V @ 360 MHz |
| Display | 4" 720×720 ST7703, MIPI-DSI interface |
| Touch | Capacitive multi-touch (GT911, I2C) |
| Microphone | Onboard SMD MEMS mic (I2S) |
| RAM | 32 MB PSRAM |
| Flash | 32 MB |

### Board GPIO Assignments

| Function | GPIO |
|----------|------|
| LCD Backlight | 26 (active LOW) |
| LCD Reset | 27 |
| I2S MCLK | 13 |
| I2S BCLK | 12 |
| I2S WS | 10 |
| I2S DIN (mic) | 11 |
| I2C SDA | 7 |
| I2C SCL | 8 |
| Touch RST | 5 |
| Touch INT | 6 |

## Prerequisites

### ESP-IDF Toolchain

Install **ESP-IDF v5.5.1 or later**.

Follow the official guide for your OS:
https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/get-started/

**Linux / macOS:**

```bash
mkdir -p ~/esp
cd ~/esp
git clone -b v5.5.1 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32p4
source export.sh
```

**Windows (ESP-IDF PowerShell):**

Use the ESP-IDF Tools Installer from
https://dl.espressif.com/dl/esp-idf/ — it sets up Python, CMake, Ninja,
and the RISC-V cross-compiler. After installation, open the
"ESP-IDF 5.5 PowerShell" shortcut.

### Verify Installation

```bash
idf.py --version          # should print 5.5.x or later
riscv32-esp-elf-gcc -v    # RISC-V cross-compiler for ESP32-P4
```

## Configure

### 1. Set the target chip

```bash
idf.py set-target esp32p4
```

This creates `sdkconfig` from `sdkconfig.defaults` with the correct CPU,
PSRAM, and peripheral settings.

### 2. Board-specific pin configuration (optional)

```bash
idf.py menuconfig
```

Navigate to **Cthugha Configuration** and verify/adjust GPIO assignments:

**LCD Display**

| Setting | Default | Description |
|---------|---------|-------------|
| Backlight GPIO | 26 | LCD backlight enable pin (active LOW) |
| LCD reset GPIO | 27 | ST7703 hardware reset pin |

**I2S Microphone**

| Setting | Default | Description |
|---------|---------|-------------|
| I2S peripheral number | 0 | I2S port (0 or 1) |
| I2S master clock GPIO | 13 | MCLK pin |
| I2S bit clock GPIO | 12 | BCK / SCK pin |
| I2S word select GPIO | 10 | WS / LRCK pin |
| I2S data in GPIO | 11 | SD / DOUT pin (mic output → ESP input) |
| Sample rate (Hz) | 16000 | Audio capture rate |

**I2C Touch**

| Setting | Default | Description |
|---------|---------|-------------|
| I2C peripheral number | 0 | I2C port (0 or 1) |
| I2C SDA GPIO | 7 | Data line |
| I2C SCL GPIO | 8 | Clock line |
| Touch reset GPIO | 5 | GT911 reset pin |
| Touch interrupt GPIO | 6 | GT911 interrupt pin |

## Build

```bash
idf.py build
```

The first build downloads managed components (`waveshare/esp_lcd_st7703`,
`espressif/esp_lcd_touch_gt911`) automatically from the Espressif
Component Registry.

## Flash & Monitor

```bash
idf.py -p PORT flash monitor
```

Replace `PORT` with your board's serial port (`/dev/ttyUSB0`, `/dev/cu.usbserial-*`, `COM3`, etc.).
Press `Ctrl+]` to exit the monitor.

## Touch Controls

| Gesture | Action |
|---------|--------|
| Tap | Cycle wave renderer |
| Swipe right | Next flame effect |
| Swipe left | Next color palette |
| Swipe up | Next display mode |
| Swipe down | Next translation effect |
| Long press | Toggle lock + print full state to serial |
| Double tap | Randomize all effects |
| Two-finger tap | Toggle boom boxes on/off |
| Three-finger tap | Toggle pseudo-FFT palette morph on/off |

## How It Works

The rendering pipeline runs as a FreeRTOS task pinned to core 0 at ~60 fps:

```
┌─────────────┐   ┌──────────────┐   ┌──────────┐   ┌───────────┐   ┌─────────┐   ┌─────────────┐
│  Translate  │──▶│    Flame     │──▶│   Wave   │──▶│  Display  │──▶│  Boom   │──▶│ LCD Output  │
│  (spatial   │   │ (scroll/blur)│   │ (draw    │   │  (mirror/ │   │  Boxes  │   │ (palette +  │
│   remap)    │   │              │   │  audio)  │   │   rotate) │   │ (paint) │   │  3× scale)  │
└─────────────┘   └──────────────┘   └──────────┘   └───────────┘   └─────────┘   └─────────────┘
                        ▲
                  ┌─────┴──────┐
                  │  Audio     │
                  │ (I2S mic + │
                  │  AGC +     │
                  │  align)    │
                  └────────────┘
240×240 @ 8-bit indexed ──────────────────────────────────────────────────────▶ 720×720 RGB565
```

The internal 240×240 framebuffer uses 8-bit indexed color with a 256-entry
palette. The LCD output stage looks up each pixel in the current palette
(optionally with a rotating offset for palette cycling), converts to RGB565,
and replicates each pixel in a 3×3 block to fill the 720×720 display.

Audio is captured from the ES7210 ADC codec via I2S, normalized to 0–255
with MAV-based AGC, then optionally zero-crossing aligned before being handed
to the wave renderer. The left channel drives `boom_boxes[0]`, right channel
drives `boom_boxes[1]`.

## Effects

**Total distinct combinations: ~290 million**

Computed as:
15 flames × 24 waves × 10 display modes × 8 translate states × 9 palettes ×
10 wave tables × 2 (FFT on/off) × 4 (palette cycle off / slow / medium / fast) ×
2 (alignment on/off) × 7 (boom off, or on × 2 color modes × 3 scales)
= **290,304,000**

Touch gestures cycle each numbered axis independently. The auto-randomizer
changes the full combination every 3–16 seconds. The `BLANK` diagnostic log
line reports the active state whenever the screen goes dark for >2 seconds.

### Flames (15) — `flame=N` in log

| # | Name | # | Name | # | Name |
|---|------|---|------|---|------|
| 0 | Slow Left | 5 | Up Fast | 10 | Water Subtle |
| 1 | Left Subtle | 6 | Right Slow | 11 | Skyline |
| 2 | Left Fast | 7 | Right Subtle | 12 | Weird |
| 3 | Up Slow | 8 | Right Fast | 13 | Zzz |
| 4 | Up Subtle | 9 | Water | 14 | Fade |

### Waves (24) — `wave=N` in log

| # | Name | # | Name | # | Name |
|---|------|---|------|---|------|
| 0 | Dot HS | 8 | Spike | 16 | Lightning 2 |
| 1 | Dot HL | 9 | Walking | 17 | Dot VS |
| 2 | Line VW | 10 | Falling | 18 | FireFlies |
| 3 | Spike S | 11 | Lissa | 19 | Pete |
| 4 | Spike L | 12 | Line VS | 20 | Pete 2 |
| 5 | Line HS | 13 | Line VL | 21 | Zippy 1 |
| 6 | Line HL | 14 | Line X | 22 | Zippy 2 |
| 7 | Dot VL | 15 | Lightning 1 | 23 | Zaph Test |

**Wave 10 — Falling:** two audio scan lines drift through the entire
240-row buffer at ~15 rows/second, painting colored pixels over flame
content without erasing it (black writes are skipped). The slow sweep
gives a "falling data" feel across the whole frame.

### Display Modes (10) — `disp=N` in log

| # | Name | Notes |
|---|------|-------|
| 0 | Upwards | Pass-through (no transform) |
| 1 | Shift Up | Scroll content up 1 row/frame — slow drift at 60fps |
| 2 | Shift Down | Scroll content down 1 row/frame |
| 3 | Downwards | Vertical flip |
| 4 | Hor. Split Out | Top/bottom halves split outward |
| 5 | Hor. Split In | Top/bottom halves split inward |
| 6 | Kaleidoscope | 4-quadrant mirror |
| 7 | 90° Rot. Mirror | 90° rotation + mirror |
| 8 | 90° Rot. Mirror 2 | 90° rotation + mirror variant |
| 9 | 90° Kaleidoscope | Rotated kaleidoscope |

### Translations (8 states) — `trans=N` in log

| # | Name | Description |
|---|------|-------------|
| 0 | None | No spatial remap |
| 1 | Swirl | Single-center rotation, angle proportional to 1/distance |
| 2 | Tunnel | Zoom-toward-center barrel effect |
| 3 | Fisheye | Outward fisheye — near-center reads from outer regions |
| 4 | Ripple | Radial sine-wave displacement |
| 5 | Moles | Dual counter-rotating vortices (left at W/4, right at 3W/4) |
| 6 | Downspiral | 45°-phase spiral pull, strongest at edges |
| 7 | Half-wheel | Asymmetric sweep around off-screen pivot at (0.4W, top) |

All translation maps are procedurally generated at startup (~115 KB each, 7 × ~800 KB total in heap).

### Palettes (9) — `pal=N` in log

| # | Name | Character |
|---|------|-----------|
| 0 | Royal Purple | Dark → purple → white → gold |
| 1 | Fire | Black → red → yellow → white |
| 2 | Ocean | Black → deep blue → cyan → white |
| 3 | Acid | Black → green → lime → white |
| 4 | Sunset | Black → magenta → orange → pale yellow |
| 5 | Neon | Three isolated arcs: rosy-red, pure green, yellow — separated by black |
| 6 | Rainbow | Full ROYGBIV spectrum |
| 7 | Fire Storm | Full hue-wheel: purple → red → yellow → green → cyan → blue |
| 8 | Volcano | Black → gray → red → yellow → white → descending orange back to dark red |

Palettes 5 (Neon), 7 (Fire Storm), and 8 (Volcano) are original Cthugha v5.3
`.MAP` files transcribed from the [cthugha-js](https://github.com/delaneyparker/cthugha-js) port.

### Wave Tables (10) — `table=N` in log

The wave table maps each audio sample value (0–255) to a palette index,
controlling the color relationship between audio amplitude and drawn pixels.

| # | Shape | Character |
|---|-------|-----------|
| 0 | V-shape × 2 | Bright at extremes, black at midpoint |
| 1 | Inverted V × 2 | Bright at midpoint, black at extremes |
| 2 | Linear ramp | Quiet = dark, loud = bright |
| 3 | Inverse ramp | Quiet = bright, loud = dark |
| 4 | V-shape + floor | V-shape with minimum at palette mid-range |
| 5 | W-shape (wrapped) | Extremes high, midpoint wraps to ~palette index 126 |
| 6 | Random noise | 256 random palette indices, seeded at boot — unique each run |
| 7 | Bimodal | Flat at 255 in center band (±64), steep ramp outside |
| 8 | Half V-shape | V-shape, half amplitude of table 0 |
| 9 | Inverted half V | Inverted V with high floor (~127–255) |

### Pseudo-FFT Palette Morph — `fft=N` in log

When active, analyzes zero-crossing run lengths and peak-to-peak amplitudes
in the audio each frame and blends palette entries toward adjacent palettes
by 0–6 steps. Percussive or rhythmic audio produces sudden full-palette
surges; steady tones give a gentle drift. Most dramatic with **Rainbow** or
**Fire Storm** palettes.

Toggle with **three-finger tap**. 25% random chance per randomize cycle.
Ported from `FFT()` in the original Cthugha v5.3 `PETE.C` (was disabled
with `#if 0`, likely too slow for the original 486).

### Palette Cycling — `palcyc=N` in log

Rotates the 256-entry palette by one step every N frames, causing colors to
swim through the visual content independently of audio. Speed is randomized
each cycle:

| Speed | Frames/step | Full cycle |
|-------|-------------|------------|
| Fast  | 5 | ~21 seconds |
| Medium | 10 | ~43 seconds |
| Slow  | 20 | ~86 seconds |

25% random chance per randomize cycle. Resets on palette change.

### Zero-Crossing Alignment — `align=N` in log

When active, each audio frame is scanned for the first upward zero-crossing
(transition from below 128 to 128+) in each stereo channel independently.
The buffer is rotated to start at that crossing point, giving line-based and
spike-based wave effects a stable phase reference rather than swimming
left/right each frame.

50% random chance per randomize cycle. Most beneficial for waves 3–6
(Spike S/L, Line HS/HL) and wave 10 (Falling).

### Boom Boxes — `boom=N tblclr=N scale=N` in log

Two colored squares (one per stereo channel) bounce around the framebuffer
seeding pixels that the flame propagates. Both their size and color have
been extended well beyond the original JS implementation:

**Size:** base audio-reactive range of 1–6 px is multiplied by a scale
factor randomized each cycle:

| Scale | Size range | Box footprint at peak |
|-------|-----------|----------------------|
| 1× | 3–13 px | ~0.5% of buffer |
| 2× | 5–25 px | ~2% of buffer |
| 3× | 7–37 px | ~5% of buffer |

**Color mode:**
- *Direct* (`tblclr=0`): color counter cycles 0–255, used directly as
  palette index. Produces a smooth rainbow sweep.
- *Table-driven* (`tblclr=1`): color counter is mapped through the current
  wave table before use. With table 0 (V-shape) the box dims to near-black
  at midpoint and peaks at extremes; with table 6 (random) it jumps
  unpredictably; with table 7 (bimodal) it flickers between 255 and a ramp.

~40% chance of being active per randomize cycle. When active, scale and
color mode are each independently randomized. Inspired by
[cthugha-js](https://github.com/delaneyparker/cthugha-js) — not in the
original v5.3 DOS source.

## Project Structure

```
cthugha_esp/
├── CMakeLists.txt          # Top-level ESP-IDF project file
├── sdkconfig.defaults      # Default SDK configuration for ESP32-P4
├── main/
│   ├── CMakeLists.txt      # Component registration
│   ├── idf_component.yml   # Managed component dependencies
│   ├── Kconfig.projbuild   # Menuconfig options (GPIO pins, etc.)
│   ├── cthugha.h           # Core types, buffer constants, externs
│   ├── main.c              # Render loop, randomizer, touch handling
│   ├── flames.c            # 15 flame effects (ported from x86 ASM)
│   ├── waves.c             # 24 wave renderers (ported from MODES.C + PETE.C)
│   ├── palettes.c/h        # 9 color palettes (procedural + original .MAP)
│   ├── translate.c         # 7 spatial remap effects (procedurally generated)
│   ├── display.c/h         # ST7703 MIPI-DSI driver, 10 display modes, 3× scaling
│   ├── audio_capture.c/h   # I2S capture, MAV AGC, zero-crossing alignment
│   ├── boom_box.c/h        # Bouncing audio-reactive colored squares
│   └── touch_input.c/h     # GT911 capacitive touch + gesture detection
```

## Credits & Attributions

### Original Cthugha
**Cthugha v5.3** by Zaph / Digital Aasvogel Group / Torps Productions, 1993–1995.
The flame effects, wave renderers, palette system, wave tables, translation maps,
and core audio-seeded framebuffer pipeline are ported from this source.
Released under a non-commercial open source license — see the original
`CTHUGHA.H` header for terms.

### JavaScript Port
**cthugha-js** by Delaney Parker —
https://github.com/delaneyparker/cthugha-js

A TypeScript/PIXI.js adaptation of Cthugha. The **Boom Boxes** feature
and three original `.MAP` palettes (Neon, Fire Storm, Volcano) are sourced
from this port.

### ESP-IDF & Espressif Components
**ESP-IDF** — https://github.com/espressif/esp-idf

**waveshare/esp_lcd_st7703** — MIPI-DSI panel driver for the ST7703
720×720 display (via ESP Component Registry).

**espressif/esp_lcd_touch_gt911** — GT911 capacitive touch driver
(via ESP Component Registry).

### Hardware
**Waveshare ESP32-P4-WIFI6-Touch-LCD-4B** —
https://www.waveshare.com

ESP32-P4 dual-core RISC-V @ 360 MHz, 32 MB PSRAM, 32 MB flash,
720×720 MIPI-DSI display, GT911 touch, ES7210 mic ADC, ES8311 DAC.
