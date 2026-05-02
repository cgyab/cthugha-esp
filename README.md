[![Build](https://github.com/cgyab/cthugha-esp/actions/workflows/build.yml/badge.svg)](https://github.com/cgyab/cthugha-esp/actions/workflows/build.yml)

# Cthugha ESP32-P4

An ESP32-P4 port of **Cthugha v5.3** — the classic 1993 real-time audio
visualizer ("An Oscilloscope on Acid") by Zaph / Digital Aasvogel Group /
Torps Productions.

Captures audio from an onboard MEMS microphone, processes it through the
original flame, wave, and translation effects, and renders to a 720x720
MIPI-DSI touchscreen at full framerate.

## Target Hardware

**Board:** ESP32-P4-WIFI6-Touch-LCD-4B (Waveshare)

| Component | Spec |
|-----------|------|
| MCU | ESP32-P4, dual-core RISC-V @ 360 MHz |
| Display | 4" 720x720 ST7703, MIPI-DSI interface |
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

Open menuconfig:

```bash
idf.py menuconfig
```

Navigate to **Cthugha Configuration** and verify/adjust the GPIO
assignments for your specific board revision:

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

Build output goes to `build/`. The firmware binary is
`build/cthugha_esp.bin`.

## Flash & Monitor

Connect the board via USB and run:

```bash
idf.py -p PORT flash monitor
```

Replace `PORT` with the serial port for your board:
- **Linux:** `/dev/ttyUSB0` or `/dev/ttyACM0`
- **macOS:** `/dev/cu.usbserial-*` or `/dev/cu.usbmodem*`
- **Windows:** `COM3` (check Device Manager)

To exit the serial monitor, press `Ctrl+]`.

### Flash only (no monitor)

```bash
idf.py -p PORT flash
```

### Monitor only (no flash)

```bash
idf.py -p PORT monitor
```

## Touch Controls

Touch gestures replace the original keyboard controls:

| Gesture | Action |
|---------|--------|
| Tap | Cycle wave renderer |
| Swipe right | Next flame effect |
| Swipe left | Next color palette |
| Swipe up | Next display mode (mirror/kaleidoscope) |
| Swipe down | Next translation effect (swirl/tunnel/etc.) |
| Long press | Toggle lock (freezes current effect combination) |

## How It Works

The rendering pipeline runs as a FreeRTOS task at ~60 fps:

```
┌─────────┐   ┌──────────┐   ┌──────┐   ┌───────────┐   ┌─────────┐   ┌─────────────┐
│  Flame  │──▶│  Audio   │──▶│ Wave │──▶│ Translate │──▶│ Display │──▶│ LCD Output  │
│ (scroll/│   │ (I2S mic │   │(draw │   │ (spatial  │   │ (mirror/│   │ (palette +  │
│  blur)  │   │  capture)│   │ audio│   │  remap)   │   │  rotate)│   │  3x scale)  │
└─────────┘   └──────────┘   │ data)│   └───────────┘   └─────────┘   └─────────────┘
                              └──────┘
240x240 @ 8-bit indexed ──────────────────────────────────────────────▶ 720x720 RGB565
```

The internal 240x240 framebuffer uses 8-bit indexed color with a 256-entry
palette. The LCD output stage looks up each pixel in the current palette
to get RGB888, converts to RGB565, and replicates each pixel in a 3x3
block to fill the 720x720 display via the ST7703 MIPI-DSI panel driver.

## Effects

Total combinations: 15 × 24 × 8 × 5 × 8 = 115,200 (× 2 with Boom Boxes on/off).
Touch gestures cycle each axis independently (see Touch Controls above).
The `BLANK` diagnostic log line reports the active combination when the
screen goes dark for more than 2 seconds.

### Flames (15) — index reported in BLANK log as `flame=N`

| # | Name | # | Name | # | Name |
|---|------|---|------|---|------|
| 0 | Slow Left | 5 | Up Fast | 10 | Water Subtle |
| 1 | Left Subtle | 6 | Right Slow | 11 | Skyline |
| 2 | Left Fast | 7 | Right Subtle | 12 | Weird |
| 3 | Up Slow | 8 | Right Fast | 13 | Zzz |
| 4 | Up Subtle | 9 | Water | 14 | Fade |

### Waves (24) — index reported as `wave=N`

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

### Display Modes (8) — index reported as `disp=N`

| # | Name | Notes |
|---|------|-------|
| 0 | Upwards | Pass-through (no transform) |
| 1 | Downwards | Vertical flip |
| 2 | Hor. Split Out | Top/bottom split outward |
| 3 | Hor. Split In | Top/bottom split inward |
| 4 | Kaleidoscope | 4-quadrant mirror |
| 5 | 90° Rot. Mirror | Rotated + mirrored |
| 6 | 90° Rot. Mirror 2 | Rotated + mirrored variant |
| 7 | 90° Kaleidoscope | Rotated kaleidoscope |

### Translations (5 states) — index reported as `trans=N`

| # | Name |
|---|------|
| 0 | None |
| 1 | Swirl |
| 2 | Tunnel |
| 3 | Fisheye |
| 4 | Ripple |

### Palettes (8) — index reported as `pal=N`

| # | Name | # | Name |
|---|------|---|------|
| 0 | Royal Purple | 4 | Sunset |
| 1 | Fire | 5 | Ice |
| 2 | Ocean | 6 | Rainbow |
| 3 | Acid | 7 | Hot Metal |

### Boom Boxes — randomly active, not a numbered axis

Two colored squares (one per stereo mic channel) bounce around the
framebuffer seeding pixels that the flame then propagates. Size is
audio-reactive (1–6 px). Activated with ~40% probability each time
effects are randomized; occasionally reset to a new random position and
velocity. Inspired by [cthugha-js](https://github.com/delaneyparker/cthugha-js)
— not present in the original v5.3 DOS source.

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
│   ├── main.c              # Entry point, FreeRTOS render task, touch handling
│   ├── flames.c            # 15 flame effects (ported from x86 ASM)
│   ├── waves.c             # 24 wave renderers (ported from MODES.C + PETE.C)
│   ├── palettes.c/h        # 8 procedural color palettes
│   ├── translate.c         # 4 spatial remap effects
│   ├── display.c/h         # ST7703 MIPI-DSI driver, display modes, 3x scaling
│   ├── audio_capture.c/h   # I2S MEMS microphone capture
│   ├── boom_box.c/h         # Bouncing audio-reactive colored squares
│   └── touch_input.c/h     # GT911 capacitive touch with gesture detection
```

## Credits & Attributions

### Original Cthugha
**Cthugha v5.3** by Zaph / Digital Aasvogel Group / Torps Productions, 1993–1995.
The flame effects, wave renderers, palette system, and core audio-seeded
framebuffer pipeline are ported from this source. Released under a
non-commercial open source license — see the original `CTHUGHA.H` header
for terms.

### JavaScript Port
**cthugha-js** by Delaney Parker —
https://github.com/delaneyparker/cthugha-js

A TypeScript/PIXI.js adaptation of Cthugha. The **Boom Boxes** feature
(bouncing audio-reactive colored squares) is derived from this port and
was not present in the original v5.3 DOS source.

### ESP-IDF & Espressif Components
**ESP-IDF** — https://github.com/espressif/esp-idf
Framework, FreeRTOS integration, I2S, I2C, GPIO, and DMA drivers.

**waveshare/esp_lcd_st7703** (via ESP Component Registry)
MIPI-DSI panel driver for the ST7703 720×720 display used on the
Waveshare ESP32-P4-WIFI6-Touch-LCD-4B board.

**espressif/esp_lcd_touch_gt911** (via ESP Component Registry)
Capacitive touch driver for the GT911 controller.

**espressif/esp_codec_dev** — ES7210 ADC codec driver (mic input) and
ES8311 DAC codec driver extracted from this component's device drivers.

### Hardware
**Waveshare ESP32-P4-WIFI6-Touch-LCD-4B** development board —
https://www.waveshare.com

ESP32-P4 dual-core RISC-V @ 360 MHz, 32 MB PSRAM, 32 MB flash,
720×720 MIPI-DSI display, GT911 touch, ES7210 mic ADC, ES8311 DAC.
