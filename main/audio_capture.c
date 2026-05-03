//
// Cthugha ESP32-P4 Port — I2S audio capture via ES7210 ADC codec
//
// Board has two audio chips:
//   ES8311 (0x18) — DAC/speaker only, no ADC
//   ES7210 (0x40) — 4-channel ADC/microphone codec (this one)
//
// ES7210 is initialized in I2S slave mode with MIC1+MIC2 active.
// ESP32 I2S is master (generates MCLK, BCLK, WS). Stereo: L=MIC1, R=MIC2.
//

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "cthugha.h"
#include "audio_capture.h"
#include "touch_input.h"

static const char *TAG = "cthugha_audio";

static i2s_chan_handle_t tx_handle = NULL;
static i2s_chan_handle_t rx_handle = NULL;
static i2c_master_dev_handle_t es7210_dev = NULL;

// Stereo interleaved [L0,R0,L1,R1,...] — 240 pairs = 480 int16 = 960 bytes
// L=MIC1, R=MIC2
static int16_t raw_samples[BUFF_WIDTH * 2];

int stereo[BUFF_WIDTH][2];
int minnoise = 2;
int use_alignment = 1;
int mic_amplify = 200; // AGC-computed each frame; readable externally for diagnostics

// --- ES7210 ADC codec via new I2C master API ---
// AD1=AD0=0 → 7-bit address 0x40 (stored as 0x80 in 8-bit convention)
#define ES7210_ADDR 0x40

static esp_err_t e7_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(es7210_dev, buf, 2, pdMS_TO_TICKS(100));
}

static esp_err_t e7_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(es7210_dev, &reg, 1, val, 1, pdMS_TO_TICKS(100));
}

// Read-modify-write: (reg & ~mask) | (mask & val)
static void e7_update(uint8_t reg, uint8_t mask, uint8_t val)
{
    uint8_t cur = 0;
    e7_read(reg, &cur);
    e7_write(reg, (cur & ~mask) | (mask & val));
}

static void es7210_codec_init(void)
{
    i2c_master_bus_handle_t bus = touch_get_i2c_bus();

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = ES7210_ADDR,
        .scl_speed_hz    = 100000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &es7210_dev));

    // Probe
    uint8_t r00 = 0;
    if (e7_read(0x00, &r00) != ESP_OK) {
        ESP_LOGE(TAG, "ES7210 not found at 0x%02X", ES7210_ADDR);
        return;
    }
    ESP_LOGI(TAG, "ES7210 found at 0x%02X, REG00=%02X", ES7210_ADDR, r00);

    // Reset and enable
    ESP_ERROR_CHECK(e7_write(0x00, 0xFF)); // reset
    ESP_ERROR_CHECK(e7_write(0x00, 0x41)); // deassert
    ESP_ERROR_CHECK(e7_write(0x01, 0x3F)); // all clocks on

    // Timing
    ESP_ERROR_CHECK(e7_write(0x09, 0x30));
    ESP_ERROR_CHECK(e7_write(0x0A, 0x30));

    // HPF
    ESP_ERROR_CHECK(e7_write(0x23, 0x2A));
    ESP_ERROR_CHECK(e7_write(0x22, 0x0A));
    ESP_ERROR_CHECK(e7_write(0x20, 0x0A));
    ESP_ERROR_CHECK(e7_write(0x21, 0x2A));

    // I2S slave mode
    e7_update(0x08, 0x01, 0x00);

    // Analog power, MIC bias 2.87V
    ESP_ERROR_CHECK(e7_write(0x40, 0x43));
    ESP_ERROR_CHECK(e7_write(0x41, 0x70));
    ESP_ERROR_CHECK(e7_write(0x42, 0x70));

    // OSR=32, clock divider init
    ESP_ERROR_CHECK(e7_write(0x07, 0x20));
    ESP_ERROR_CHECK(e7_write(0x02, 0xC1));

    // Mic select: disable all, then enable MIC1+MIC2 with 30dB gain
    e7_update(0x43, 0x10, 0x00); // MIC1 disable
    e7_update(0x44, 0x10, 0x00); // MIC2 disable
    e7_update(0x45, 0x10, 0x00); // MIC3 disable
    e7_update(0x46, 0x10, 0x00); // MIC4 disable
    e7_write(0x4B, 0xFF);        // MIC12 power down
    e7_write(0x4C, 0xFF);        // MIC34 power down
    e7_update(0x01, 0x0B, 0x00); // enable clocks for MIC1/2 path
    e7_write(0x4B, 0x00);        // power up MIC12
    e7_update(0x43, 0x1F, 0x1E); // MIC1 on, 37.5dB max (bit4=1, gain=0x0E)
    e7_update(0x44, 0x1F, 0x1E); // MIC2 on, 37.5dB max

    // Serial port: 16-bit I2S (bits[7:5]=0b011, bits[1:0]=0b00)
    ESP_ERROR_CHECK(e7_write(0x11, 0x60));
    // Non-TDM for 2 mics
    ESP_ERROR_CHECK(e7_write(0x12, 0x00));

    // Start ADC — sequence from es7210_start()
    ESP_ERROR_CHECK(e7_write(0x01, 0x34)); // clock state: MIC1/2 on, MIC3/4 off
    ESP_ERROR_CHECK(e7_write(0x06, 0x00)); // power down off (= powered up)
    ESP_ERROR_CHECK(e7_write(0x40, 0x43));
    ESP_ERROR_CHECK(e7_write(0x47, 0x08)); // MIC1 normal power
    ESP_ERROR_CHECK(e7_write(0x48, 0x08)); // MIC2 normal power
    ESP_ERROR_CHECK(e7_write(0x49, 0x08)); // MIC3 normal power
    ESP_ERROR_CHECK(e7_write(0x4A, 0x08)); // MIC4 normal power
    // mic_select again (matches es7210_start)
    e7_update(0x43, 0x10, 0x00);
    e7_update(0x44, 0x10, 0x00);
    e7_update(0x45, 0x10, 0x00);
    e7_update(0x46, 0x10, 0x00);
    e7_write(0x4B, 0xFF);
    e7_write(0x4C, 0xFF);
    e7_update(0x01, 0x0B, 0x00);
    e7_write(0x4B, 0x00);
    e7_update(0x43, 0x1F, 0x1A);
    e7_update(0x44, 0x1F, 0x1A);
    e7_write(0x12, 0x00);
    ESP_ERROR_CHECK(e7_write(0x40, 0x43));
    ESP_ERROR_CHECK(e7_write(0x00, 0x71)); // start ADC
    ESP_ERROR_CHECK(e7_write(0x00, 0x41));

    ESP_LOGI(TAG, "ES7210 mic codec initialized (MIC1=L, MIC2=R, 30dB gain)");
}

void audio_capture_init(void)
{
    ESP_LOGI(TAG, "Initializing I2S+ES7210 on I2S%d", CONFIG_CTHUGHA_I2S_NUM);

    // Full duplex (TX+RX): TX drives DOUT to ES8311 DAC (silence), RX reads ES7210 ADC
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(
        CONFIG_CTHUGHA_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(CONFIG_CTHUGHA_I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)CONFIG_CTHUGHA_I2S_MCLK_GPIO,
            .bclk = (gpio_num_t)CONFIG_CTHUGHA_I2S_BCK_GPIO,
            .ws   = (gpio_num_t)CONFIG_CTHUGHA_I2S_WS_GPIO,
            .dout = (gpio_num_t)CONFIG_CTHUGHA_I2S_DOUT_GPIO,
            .din  = (gpio_num_t)CONFIG_CTHUGHA_I2S_DIN_GPIO,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));

    // Codec initialized after I2S clocks are running (ES7210 uses MCLK)
    es7210_codec_init();

    ESP_LOGI(TAG, "Audio ready (rate=%d stereo MIC1+MIC2)", CONFIG_CTHUGHA_I2S_SAMPLE_RATE);
}

int audio_capture_read(void)
{
    size_t bytes_read = 0;
    // 240 stereo pairs × 4 bytes = 960 bytes; L=MIC1, R=MIC2
    esp_err_t err = i2s_channel_read(rx_handle, raw_samples,
                                     BUFF_WIDTH * 2 * sizeof(int16_t),
                                     &bytes_read, pdMS_TO_TICKS(50));

    if (err != ESP_OK || bytes_read == 0)
        return 0;

    int pairs = (int)(bytes_read / (2 * sizeof(int16_t)));

    // MAV-based AGC (Mean Absolute Value — tracks average energy, not peaks)
    // Peak-following AGC suppresses gain after every transient; MAV rides the
    // mean level so typical amplitude stays high even after a loud spike.
    // Attack: fast (α=0.3) — gain drops quickly if average level rises
    // Decay:  slow (α=0.002) — gain recovers over ~10s as room gets quieter
    // Floor:  5 raw counts — prevents runaway gain in silence
    static float agc_level = 30.0f;
    long sum_abs = 0;
    for (int i = 0; i < pairs; i++) {
        sum_abs += abs((int)raw_samples[i * 2]) + abs((int)raw_samples[i * 2 + 1]);
    }
    float frame_mav = (float)sum_abs / (pairs * 2);

    if (frame_mav > agc_level)
        agc_level = agc_level * 0.7f + frame_mav * 0.3f;  // fast attack
    else
        agc_level = agc_level * 0.998f + frame_mav * 0.002f;  // slow decay
    if (agc_level < 5.0f) agc_level = 5.0f;

    // Target: output MAV ≈ 50 counts from midpoint 128 (~39% of half-range on average)
    // Peaks ride 3-5× above MAV so will hit 150-250 — vivid with occasional clipping.
    // 50 * 256 = 12800
    mic_amplify = ct_clamp((int)(12800.0f / agc_level), 1, 512);

    // static int dbg = 0;
    // if (++dbg >= 300) {
    //     dbg = 0;
    //     ESP_LOGI(TAG, "AGC: mav=%.1f amplify=%d  L[0]=%d R[0]=%d",
    //              agc_level, mic_amplify, raw_samples[0], raw_samples[1]);
    // }

    for (int i = 0; i < (int)BUFF_WIDTH; i++) {
        int16_t l = (i < pairs) ? raw_samples[i * 2]     : 0;
        int16_t r = (i < pairs) ? raw_samples[i * 2 + 1] : 0;
        stereo[i][0] = ct_clamp(128 + (int)l * mic_amplify / 256, 0, 255);
        stereo[i][1] = ct_clamp(128 + (int)r * mic_amplify / 256, 0, 255);
    }

    // Zero-crossing alignment: optional — disabled gives the "swimming" wave look
    if (!use_alignment) return 1;

    // Find first upward crossing (prev<128 → curr>=128)
    // in each channel and rotate the buffer so the display window starts there.
    // Search limit = BUFF_WIDTH/2 so there are always at least 120 samples after
    // the crossing — avoids wrapping discontinuities in line-drawing wave effects.
    static int align_tmp[BUFF_WIDTH];
    for (int ch = 0; ch < 2; ch++) {
        int a = 0;
        int prev = stereo[0][ch];
        for (int i = 1; i < (int)BUFF_WIDTH / 2; i++) {
            if (stereo[i][ch] >= 128 && prev < 128) { a = i; break; }
            prev = stereo[i][ch];
        }
        if (a > 0) {
            for (int i = 0; i < (int)BUFF_WIDTH; i++)
                align_tmp[i] = stereo[(i + a) % BUFF_WIDTH][ch];
            for (int i = 0; i < (int)BUFF_WIDTH; i++)
                stereo[i][ch] = align_tmp[i];
        }
    }

    return 1;
}

int get_stereo_data(void)
{
    if (!audio_capture_read())
        return 0;

    int noisy = 0;
    for (int ch = 0; ch < 2; ch++) {
        int lo = 255, hi = 0;
        for (int x = 0; x < (int)BUFF_WIDTH; x++) {
            if (stereo[x][ch] > hi) hi = stereo[x][ch];
            if (stereo[x][ch] < lo) lo = stereo[x][ch];
        }
        if ((hi - lo) > minnoise)
            noisy = 1;
    }

    return noisy;
}
