[![Build](https://github.com/cgyab/cthugha-esp/actions/workflows/build.yml/badge.svg)](https://github.com/cgyab/cthugha-esp/actions/workflows/build.yml)

# Cthugha ESP32-P4

An ESP32-P4 port of **Cthugha v5.3** — the classic 1993 real-time audio
visualizer ("An Oscilloscope on Acid") by Zaph / Digital Aasvogel Group /
Torps Productions.

Captures audio from an onboard MEMS microphone, processes it through the
original flame, wave, and translation effects, and renders to a 720×720
MIPI-DSI touchscreen at ~60 fps.

---

## Touch Controls

### Unlocked mode

| Gesture | Action |
|---------|--------|
| Tap | Cycle wave renderer |
| Swipe right | Next flame effect |
| Swipe left | Next color palette |
| Swipe up | Next display mode |
| Swipe down | Next translation effect |
| Double tap | Randomize all effects |
| Two-finger tap | Toggle boom boxes on/off |
| Three-finger tap | Toggle pseudo-FFT palette morph on/off |
| Four-finger tap | **Home** — canonical Cthugha preset + lock |
| Long press | **Lock** — freeze current combo and enter lock-config mode |

### Locked mode

When locked, the same gestures **configure per-axis locks** instead of
cycling effects. A locked axis is skipped by the auto-randomizer, so you
can hold a favourite palette (or flame, wave, etc.) while everything else
continues to change.

| Gesture | Axis toggled |
|---------|-------------|
| Tap | Wave lock |
| Swipe right | Flame lock |
| Swipe left | Palette lock |
| Swipe up | Display lock |
| Swipe down | Translate lock |
| Two-finger tap | Boom box lock |
| Three-finger tap | Toggle FFT (same as unlocked) |
| Four-finger tap | **Home** — canonical Cthugha preset + lock (same as unlocked) |
| Double tap | **Nuclear** — clear all locks + full randomize |
| Long press (all axes locked) | **Unlock all** + full randomize |
| Long press (some axes unlocked) | **Resume** — restart auto-timer respecting current locks |

**Four-finger tap** returns to the canonical Cthugha preset from anywhere
(Up Slow flame, Line HS wave, Fire palette, no transforms) and locks it —
a known-good starting point you can always return to.

**Typical workflow:**
1. A great palette appears → **long press** to freeze everything
2. **Swipe left** → unlock palette axis (palette will now stay fixed when timer resumes)
3. **Long press** → resume; only wave/flame/display/translate/boom randomize, palette holds
4. Want full chaos again? **Double tap** from locked mode clears all locks and randomizes everything

The serial monitor (115200 baud) prints the full effect state on lock-entry
and the per-axis lock vector after every locked-mode gesture.

---

## Effects

**Total distinct combinations: ~339 million**

15 flames × 28 waves × 10 display modes × 8 translate states × 9 palettes ×
10 wave tables × 2 (FFT) × 4 (palette cycle off/slow/medium/fast) ×
2 (alignment) × 7 (boom off, or on × 2 color modes × 3 scales)
= **339,148,800**

The auto-randomizer changes the full combination every 3–16 seconds.
Touch gestures cycle each numbered axis independently.

### Flames (15) — `flame=N`

| # | Name | # | Name | # | Name |
|---|------|---|------|---|------|
| 0 | Slow Left | 5 | Up Fast | 10 | Water Subtle |
| 1 | Left Subtle | 6 | Right Slow | 11 | Skyline |
| 2 | Left Fast | 7 | Right Subtle | 12 | Weird |
| 3 | Up Slow | 8 | Right Fast | 13 | Zzz |
| 4 | Up Subtle | 9 | Water | 14 | Fade |

### Waves (28) — `wave=N`

| # | Name | # | Name | # | Name |
|---|------|---|------|---|------|
| 0 | Dot HS | 9 | Walking | 18 | FireFlies |
| 1 | Dot HL | 10 | Falling | 19 | Pete |
| 2 | Line VW | 11 | Lissa | 20 | Pete 2 |
| 3 | Spike S | 12 | Line VS | 21 | Zippy 1 |
| 4 | Spike L | 13 | Line VL | 22 | Zippy 2 |
| 5 | Line HS | 14 | Line X | 23 | Zaph Test |
| 6 | Line HL | 15 | Lightning 1 | 24 | Moles 1 |
| 7 | Dot VL | 16 | Lightning 2 | 25 | Moles 2 |
| 8 | Spike | 17 | Dot VS | 26 | Raindrops |
| | | | | 27 | Claude |

**Wave 10 — Falling:** two audio scan lines drift through the full
240-row buffer at ~15 rows/second, painting colored pixels over flame
content without erasing it (black writes are skipped).

**Waves 24–25 — Moles 1 / Moles 2:** two particles (one per stereo
channel) wander the entire buffer driven by the *derivative* of the audio
signal — change between adjacent samples, not amplitude. Dots appear
anywhere in the frame, causing the flame to sprout from interior points
rather than rising from the bottom. Moles 2 uses full derivative steps
(2× more volatile than Moles 1). Right-channel y-axis is mirrored so the
two trails move in complementary directions.

**Wave 26 — Raindrops:** expanding ring impulses at random positions
across the buffer. Each drop seeds a Bresenham circle per frame; the flame
engine propagates the rings into expanding glowing halos. Up to 8
simultaneous rings, each growing 1 px/frame and retiring at radius 60
(~1 s lifetime). Spawn rate scales with audio energy: ~3 drops/s at
silence, ~15 drops/s at loud. Drop positions are pseudorandom, stirred by
incoming audio samples each frame.

**Wave 27 — Claude:** Lorenz strange attractor with audio-modulated chaos
parameter. Integrates the Lorenz system 40 steps/frame (σ=10, β=8/3),
seeding one pixel per step. Quiet audio holds ρ≈28 — the classic
butterfly: two lobes the system orbits and occasionally flips between.
Loud audio pushes ρ toward 50, destabilizing the attractor; lobes merge,
the trace sprawls across more of the buffer, and at peak levels it goes
fully chaotic. The z coordinate maps to the color table so hue shifts as
the system moves through the attractor's altitude. The flame turns the
particle trail into glowing incandescent plasma.

### Display Modes (10) — `disp=N`

| # | Name | Notes |
|---|------|-------|
| 0 | Upwards | Pass-through (no transform) |
| 1 | Shift Up | Scroll all content up 1 row/frame |
| 2 | Shift Down | Scroll all content down 1 row/frame |
| 3 | Downwards | Vertical flip |
| 4 | Hor. Split Out | Top/bottom halves split outward |
| 5 | Hor. Split In | Top/bottom halves split inward |
| 6 | Kaleidoscope | 4-quadrant mirror |
| 7 | 90° Rot. Mirror | 90° rotation + mirror |
| 8 | 90° Rot. Mirror 2 | 90° rotation + mirror variant |
| 9 | 90° Kaleidoscope | Rotated kaleidoscope |

### Translations (8 states) — `trans=N`

| # | Name | Description |
|---|------|-------------|
| 0 | None | No spatial remap |
| 1 | Swirl | Single-center rotation, angle ∝ 1/distance |
| 2 | Tunnel | Zoom-toward-center barrel effect |
| 3 | Fisheye | Outward fisheye — near-center reads from outer regions |
| 4 | Ripple | Radial sine-wave displacement |
| 5 | Moles | Dual counter-rotating vortices at W/4 and 3W/4 |
| 6 | Downspiral | 45°-phase spiral pull, strongest at edges |
| 7 | Half-wheel | Asymmetric sweep around off-screen pivot at (0.4W, top) |

All maps are procedurally generated at startup (~115 KB each, ~800 KB total).

### Palettes (9) — `pal=N`

| # | Name | Character |
|---|------|-----------|
| 0 | Royal Purple | Dark → purple → white → gold |
| 1 | Fire | Black → red → yellow → white |
| 2 | Ocean | Black → deep blue → cyan → white |
| 3 | Acid | Black → green → lime → white |
| 4 | Sunset | Black → magenta → orange → pale yellow |
| 5 | Neon | Three isolated arcs: rosy-red / pure green / yellow, separated by black |
| 6 | Rainbow | Full ROYGBIV spectrum |
| 7 | Fire Storm | Full hue-wheel: purple → red → yellow → green → cyan → blue |
| 8 | Volcano | Black → gray → red → yellow → white → descending orange back to dark red |

Palettes 5, 7, and 8 are original Cthugha v5.3 `.MAP` files.

### Wave Tables (10) — `table=N`

Maps each audio sample value (0–255) to a palette index, controlling the
color relationship between audio amplitude and drawn pixels.

| # | Shape | Character |
|---|-------|-----------|
| 0 | V-shape ×2 | Bright at extremes, black at midpoint |
| 1 | Inv. V ×2 | Bright at midpoint, black at extremes |
| 2 | Linear ramp | Quiet = dark, loud = bright |
| 3 | Inverse ramp | Quiet = bright, loud = dark |
| 4 | V + floor | V-shape with minimum at palette mid-range |
| 5 | W-shape | Extremes high, midpoint wraps to ~index 126 |
| 6 | Random noise | 256 random palette indices, seeded at boot |
| 7 | Bimodal | Flat at 255 in center band (±64), steep ramp outside |
| 8 | Half V | V-shape, half amplitude |
| 9 | Inv. half V | Inverted V with high floor (~127–255) |

### Special Modes (randomized each cycle, not a numbered axis)

**Pseudo-FFT palette morph** (`fft=N`) — analyzes zero-crossing run lengths
and peak-to-peak amplitudes each frame and blends palette entries toward
adjacent palettes by 0–6 steps. Percussive audio produces sudden
full-palette surges; steady tones give a gentle drift. 25% chance per
cycle. Toggle manually with **three-finger tap**. Ported from `FFT()` in
the original Cthugha v5.3 `PETE.C` (disabled there with `#if 0`).

**Palette cycling** (`palcyc=N`) — rotates the 256-entry palette by one
step every N frames, causing colors to swim through the visual content
independently of audio. 25% chance per cycle. Speed is also randomized:

| Speed | Frames/step | Full 256-entry cycle |
|-------|-------------|----------------------|
| Fast  | 5  | ~21 s |
| Medium | 10 | ~43 s |
| Slow  | 20 | ~86 s |

**Zero-crossing alignment** (`align=N`) — before each frame, the audio
buffer is rotated so it starts at the first upward zero-crossing of each
stereo channel. Gives line-based and spike-based waves a stable phase
reference rather than swimming left/right each frame. 50% chance per
cycle.

**Boom boxes** (`boom=N tblclr=N scale=N`) — two colored squares (one per
stereo channel) bounce around the buffer seeding pixels that the flame
propagates. ~40% chance of being active. When active:

- *Scale* (1×/2×/3×, equal chance each): multiplies audio-reactive box
  size. At 3× peak amplitude the box is ~37×37 px (~5% of the buffer).
- *Table color* (50/50): when on, the color counter is mapped through the
  current wave table before use, so color response follows the table's
  curve rather than a simple sweep.

---

## How It Works

The rendering pipeline runs as a FreeRTOS task pinned to core 0:

```
┌─────────────┐   ┌──────────────┐   ┌──────────┐   ┌───────────┐   ┌─────────┐   ┌─────────────┐
│  Translate  │──▶│    Flame     │──▶│   Wave   │──▶│  Display  │──▶│  Boom   │──▶│ LCD Output  │
│ (spatial    │   │ (scroll/blur)│   │ (draw    │   │ (mirror/  │   │  Boxes  │   │ (palette +  │
│  remap)     │   │              │   │  audio)  │   │  rotate)  │   │ (paint) │   │  3× scale)  │
└─────────────┘   └──────────────┘   └──────────┘   └───────────┘   └─────────┘   └─────────────┘
                        ▲
                  ┌─────┴──────┐
                  │   Audio    │
                  │ (I2S mic + │
                  │  AGC +     │
                  │  align)    │
                  └────────────┘
240×240 @ 8-bit indexed ──────────────────────────────────────────────────────▶ 720×720 RGB565
```

The internal 240×240 framebuffer uses 8-bit indexed color. The LCD output
stage looks up each pixel in the current palette (optionally with a
rotating offset for palette cycling), converts to RGB565, and replicates
each pixel 3× in both axes to fill the 720×720 display.

Audio is captured from the ES7210 ADC codec via I2S, normalized to 0–255
with MAV-based AGC, then optionally zero-crossing aligned per channel
before the wave renderer runs.

---

## Target Hardware

**Board:** ESP32-P4-WIFI6-Touch-LCD-4B (Waveshare)

| Component | Spec |
|-----------|------|
| MCU | ESP32-P4, dual-core RISC-V @ 360 MHz |
| Display | 4" 720×720 ST7703, MIPI-DSI |
| Touch | GT911 capacitive multi-touch, I2C |
| Microphone | ES7210 ADC codec, I2S |
| RAM | 32 MB PSRAM |
| Flash | 32 MB |

### GPIO Assignments

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

---

## Build & Flash

### Prerequisites

Install **ESP-IDF v5.5.1 or later** for ESP32-P4:

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v5.5.1 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32p4 && source export.sh
```

Windows: use the ESP-IDF Tools Installer from https://dl.espressif.com/dl/esp-idf/

### Configure (optional)

```bash
idf.py set-target esp32p4   # creates sdkconfig from sdkconfig.defaults
idf.py menuconfig           # adjust GPIO pins under "Cthugha Configuration"
```

### Build

```bash
idf.py build
```

First build downloads `waveshare/esp_lcd_st7703` and
`espressif/esp_lcd_touch_gt911` from the Espressif Component Registry.

### Flash & monitor

```bash
idf.py -p PORT flash monitor   # PORT = /dev/ttyUSB0, /dev/cu.usbserial-*, COM3, etc.
```

Press `Ctrl+]` to exit the monitor.

---

## Project Structure

```
cthugha_esp/
├── CMakeLists.txt
├── sdkconfig.defaults          # ESP32-P4 defaults (360 MHz, PSRAM, MIPI-DSI)
└── main/
    ├── cthugha.h               # Core types, buffer constants, shared externs
    ├── main.c                  # Render loop, randomizer, per-axis locks, touch
    ├── flames.c                # 15 flame effects (ported from x86 ASM)
    ├── waves.c                 # 28 wave renderers (ported from MODES.C + PETE.C)
    ├── palettes.c/h            # 9 palettes (procedural + original v5.3 .MAP)
    ├── translate.c             # 7 spatial remap effects (procedural generation)
    ├── display.c/h             # ST7703 MIPI-DSI driver, 10 display modes, 3× scale
    ├── audio_capture.c/h       # I2S capture, MAV AGC, zero-crossing alignment
    ├── boom_box.c/h            # Audio-reactive bouncing colored squares
    ├── touch_input.c/h         # GT911 touch + gesture detection
    ├── Kconfig.projbuild       # Menuconfig GPIO/I2S options
    └── idf_component.yml       # Managed component dependencies
```

---

## Credits & Attributions

**Cthugha v5.3** — Zaph / Digital Aasvogel Group / Torps Productions, 1993–1995.
Flame effects, wave renderers, palette system, wave tables, translation maps,
and the core audio-seeded framebuffer pipeline. Non-commercial open source —
see the original `CTHUGHA.H` header for terms.

**cthugha-js** — Delaney Parker, https://github.com/delaneyparker/cthugha-js
Boom boxes feature, three original `.MAP` palettes (Neon, Fire Storm, Volcano),
and Moles translate/wave generators sourced from this TypeScript port.

**ESP-IDF** — https://github.com/espressif/esp-idf

**waveshare/esp_lcd_st7703** and **espressif/esp_lcd_touch_gt911** via
ESP Component Registry.

**Waveshare ESP32-P4-WIFI6-Touch-LCD-4B** — https://www.waveshare.com
