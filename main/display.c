//
// Cthugha ESP32-P4 Port — ST7703 MIPI-DSI LCD display driver
// Scales the 240x240 8-bit indexed buffer to 720x720 RGB565 via 3x nearest-neighbor
//

#include <string.h>
#include "logo_data.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_st7703.h"
#include "driver/gpio.h"
#include "cthugha.h"
#include "display.h"

static const char *TAG = "cthugha_display";

#define MIPI_DSI_PHY_PWR_LDO_CHAN        3
#define MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV  2500
#define LCD_BK_LIGHT_ON_LEVEL            0

static esp_lcd_panel_handle_t panel_handle = NULL;
static esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
static esp_lcd_panel_io_handle_t mipi_dbi_io = NULL;
static esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
static SemaphoreHandle_t refresh_finish = NULL;
static uint16_t *lcd_fb[2] = {NULL, NULL};
int logo_overlay_enabled = 0;
static int cur_fb = 0;

static uint16_t pal_lut[256];

static inline uint16_t rgb888_to_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint16_t)(r >> 3) << 11) |
           ((uint16_t)(g >> 2) << 5)  |
           ((uint16_t)(b >> 3));
}

static void update_palette_lut(void)
{
    for (int i = 0; i < 256; i++) {
        int idx = use_pal_cycle ? (i + pal_cycle_offset) & 0xFF : i;
        pal_lut[i] = rgb888_to_rgb565(LUTbuffer[idx * 3 + 0],
                                      LUTbuffer[idx * 3 + 1],
                                      LUTbuffer[idx * 3 + 2]);
    }
    if (use_pal_cycle) {
        static int cycle_counter = 0;
        if (++cycle_counter >= pal_cycle_speed) {
            cycle_counter = 0;
            pal_cycle_offset = (pal_cycle_offset + 1) & 0xFF;
        }
    }
}

// Display effect function pointers (ported from DISPLAY.C)
void (*display_effect)(void);
int numdisplays = -1;
int curdisplay = 0;

static void display_upwards(void)
{
}

// Scroll entire buffer up one row per frame — content flows upward
static void display_shift_up(void)
{
    memmove(buff, buff + BUFF_WIDTH, (BUFF_HEIGHT - 1) * BUFF_WIDTH);
    memset(buff + (BUFF_HEIGHT - 1) * BUFF_WIDTH, 0, BUFF_WIDTH);
}

// Scroll entire buffer down one row per frame — content flows downward
static void display_shift_down(void)
{
    memmove(buff + BUFF_WIDTH, buff, (BUFF_HEIGHT - 1) * BUFF_WIDTH);
    memset(buff, 0, BUFF_WIDTH);
}

static void display_downwards(void)
{
    for (unsigned int y = 0; y < BUFF_HEIGHT; y++)
        memcpy(shadow + y * BUFF_WIDTH,
               buff + (BUFF_HEIGHT - 1 - y) * BUFF_WIDTH, BUFF_WIDTH);
    flip_screens();
}

static void display_hor_split_out(void)
{
    unsigned int mid = BUFF_HEIGHT / 2;
    for (unsigned int y = 0; y < mid; y++)
        memcpy(shadow + y * BUFF_WIDTH,
               buff + (mid - 1 - y + mid) * BUFF_WIDTH, BUFF_WIDTH);
    memcpy(shadow + mid * BUFF_WIDTH,
           buff + mid * BUFF_WIDTH, mid * BUFF_WIDTH);
    flip_screens();
}

static void display_hor_split_in(void)
{
    unsigned int mid = BUFF_HEIGHT / 2;
    memcpy(shadow, buff + mid * BUFF_WIDTH, mid * BUFF_WIDTH);
    for (unsigned int y = 0; y < mid; y++)
        memcpy(shadow + (mid + y) * BUFF_WIDTH,
               buff + (mid + mid - 1 - y) * BUFF_WIDTH, BUFF_WIDTH);
    flip_screens();
}

static void display_kaleidoscope(void)
{
    unsigned int mid_x = BUFF_WIDTH / 2;
    unsigned int mid_y = BUFF_HEIGHT / 2;
    for (unsigned int y = 0; y < mid_y; y++) {
        memcpy(shadow + y * BUFF_WIDTH, buff + (y + mid_y) * BUFF_WIDTH, mid_x);
        for (unsigned int x = 0; x < mid_x; x++)
            shadow[y * BUFF_WIDTH + mid_x + x] = buff[(y + mid_y) * BUFF_WIDTH + mid_x - 1 - x];
    }
    for (unsigned int y = 0; y < mid_y; y++)
        memcpy(shadow + (mid_y + y) * BUFF_WIDTH,
               shadow + (mid_y - 1 - y) * BUFF_WIDTH, BUFF_WIDTH);
    flip_screens();
}

static void display_rot90_mirror(void)
{
    // Fix: read from rows (BUFF_WIDTH-1-x) so the left half covers rows 239..120
    // (bottom half, where wave seeds) rather than rows 0..119 (dark top half).
    // Right half mirrors: reads rows 120..239. Both halves include the seeded area.
    for (unsigned int y = 0; y < BUFF_HEIGHT; y++) {
        for (unsigned int x = 0; x < BUFF_WIDTH / 2; x++)
            shadow[y * BUFF_WIDTH + x] = buff[(BUFF_WIDTH - 1 - x) * BUFF_WIDTH + y];
        for (unsigned int x = BUFF_WIDTH / 2; x < BUFF_WIDTH; x++)
            shadow[y * BUFF_WIDTH + x] = buff[x * BUFF_WIDTH + y];
    }
    flip_screens();
}

static void display_rot90_mirror2(void)
{
    for (unsigned int y = 0; y < BUFF_HEIGHT; y++) {
        unsigned int src_col = BUFF_HEIGHT - 1 - y;
        for (unsigned int x = 0; x < BUFF_WIDTH / 2; x++)
            shadow[y * BUFF_WIDTH + x] = buff[(BUFF_WIDTH - 1 - x) * BUFF_WIDTH + src_col];
        for (unsigned int x = BUFF_WIDTH / 2; x < BUFF_WIDTH; x++)
            shadow[y * BUFF_WIDTH + x] = buff[x * BUFF_WIDTH + src_col];
    }
    flip_screens();
}

static void display_rot90_kaleidoscope(void)
{
    unsigned int mid_x = BUFF_WIDTH / 2;
    unsigned int mid_y = BUFF_HEIGHT / 2;
    for (unsigned int y = 0; y < mid_y; y++) {
        for (unsigned int x = 0; x < mid_x; x++) {
            uint8_t pixel = buff[(BUFF_WIDTH - 1 - x) * BUFF_WIDTH + y];
            shadow[y * BUFF_WIDTH + x] = pixel;
            shadow[(BUFF_HEIGHT - 1 - y) * BUFF_WIDTH + x] = pixel;
        }
        for (unsigned int x = mid_x; x < BUFF_WIDTH; x++) {
            uint8_t pixel = buff[x * BUFF_WIDTH + y];
            shadow[y * BUFF_WIDTH + x] = pixel;
            shadow[(BUFF_HEIGHT - 1 - y) * BUFF_WIDTH + x] = pixel;
        }
    }
    flip_screens();
}

function_opt disparray[] = {
    { display_upwards,           WHEN_ALWAYS, "Upwards"             },
    { display_shift_up,          WHEN_ALWAYS, "Shift Up"            },
    { display_shift_down,        WHEN_ALWAYS, "Shift Down"          },
    { display_downwards,         WHEN_ALWAYS, "Downwards"           },
    { display_hor_split_out,     WHEN_ALWAYS, "Hor. Split out"      },
    { display_hor_split_in,      WHEN_ALWAYS, "Hor. Split in"       },
    { display_kaleidoscope,      WHEN_ALWAYS, "Kaleidoscope"        },
    { display_rot90_mirror,      WHEN_ALWAYS, "90deg rot. mirror"   },
    { display_rot90_mirror2,     WHEN_ALWAYS, "90deg rot. mirror 2" },
    { display_rot90_kaleidoscope,WHEN_ALWAYS, "90deg Kaleidoscope"  },
    { NULL,                      WHEN_NEVER,  "<END>"               }
};

int change_display(int disp)
{
    if (numdisplays < 0) {
        numdisplays = 0;
        while (disparray[numdisplays].function != NULL)
            numdisplays++;
    }
    if (numdisplays == 0) return 0;

    disp = ((disp % numdisplays) + numdisplays) % numdisplays;
    display_effect = disparray[disp].function;
    return disp;
}

void flip_screens(void)
{
    uint8_t *temp = buff;
    buff = shadow;
    shadow = temp;
}

static IRAM_ATTR bool on_color_trans_done(esp_lcd_panel_handle_t panel,
                                           esp_lcd_dpi_panel_event_data_t *edata,
                                           void *user_ctx)
{
    BaseType_t need_yield = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)user_ctx, &need_yield);
    return (need_yield == pdTRUE);
}

void display_init(void)
{
    ESP_LOGI(TAG, "Initializing ST7703 MIPI-DSI display %dx%d", LCD_H_RES, LCD_V_RES);

    // Backlight (active LOW on this board)
    gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << CONFIG_CTHUGHA_LCD_BL_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl_cfg);
    gpio_set_level(CONFIG_CTHUGHA_LCD_BL_GPIO, LCD_BK_LIGHT_ON_LEVEL);

    // Power the MIPI DSI PHY via internal LDO
    ESP_LOGI(TAG, "MIPI DSI PHY power on");
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = MIPI_DSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo_mipi_phy));

    // MIPI-DSI bus (2 data lanes, config from ST7703 macro)
    ESP_LOGI(TAG, "Initialize MIPI DSI bus");
    esp_lcd_dsi_bus_config_t bus_config = ST7703_PANEL_BUS_DSI_2CH_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus));

    // DBI command interface for panel IC initialization
    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_dbi_io_config_t dbi_config = ST7703_PANEL_IO_DBI_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &mipi_dbi_io));

    // ST7703 panel driver with DPI pixel output
    ESP_LOGI(TAG, "Install ST7703 LCD driver");
    esp_lcd_dpi_panel_config_t dpi_config = ST7703_720_720_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB565);
    dpi_config.num_fbs = 2;
    dpi_config.flags.use_dma2d = true;

    st7703_vendor_config_t vendor_config = {
        .flags = {
            .use_mipi_interface = 1,
        },
        .mipi_config = {
            .dsi_bus = mipi_dsi_bus,
            .dpi_config = &dpi_config,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = CONFIG_CTHUGHA_LCD_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BPP,
        .vendor_config = &vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7703(mipi_dbi_io, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    // Vsync / transfer-done callback
    refresh_finish = xSemaphoreCreateBinary();
    esp_lcd_dpi_panel_event_callbacks_t cbs = {
        .on_color_trans_done = on_color_trans_done,
    };
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_register_event_callbacks(panel_handle, &cbs, refresh_finish));

    // Get the DPI panel's own framebuffers (allocated in DMA-capable memory)
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(panel_handle, 2,
                    (void **)&lcd_fb[0], (void **)&lcd_fb[1]));
    memset(lcd_fb[0], 0, LCD_H_RES * LCD_V_RES * sizeof(uint16_t));
    memset(lcd_fb[1], 0, LCD_H_RES * LCD_V_RES * sizeof(uint16_t));

    ESP_LOGI(TAG, "Display initialized. FB0=%p FB1=%p", lcd_fb[0], lcd_fb[1]);
}

void display_render(void)
{
    update_palette_lut();

    uint16_t *fb = lcd_fb[cur_fb];

    // Scale 240x240 → 720x720 via 3x nearest-neighbor with palette lookup
    for (int sy = 0; sy < (int)BUFF_HEIGHT; sy++) {
        int dy_base = sy * SCALE_FACTOR;
        const uint8_t *src_row = buff + sy * BUFF_WIDTH;

        uint16_t *dst_row0 = fb + dy_base * LCD_H_RES;
        for (int sx = 0; sx < (int)BUFF_WIDTH; sx++) {
            uint16_t color = pal_lut[src_row[sx]];
            int dx_base = sx * SCALE_FACTOR;
            for (int rx = 0; rx < SCALE_FACTOR; rx++)
                dst_row0[dx_base + rx] = color;
        }

        for (int ry = 1; ry < SCALE_FACTOR; ry++)
            memcpy(fb + (dy_base + ry) * LCD_H_RES,
                   dst_row0, LCD_H_RES * sizeof(uint16_t));
    }

    // Logo edge overlay — stamp only outline pixels (not fill) so the fire
    // animation shows through the interior. Uses a bright palette color so
    // the outline integrates with whatever palette is active.
    if (logo_overlay_enabled) {
        uint16_t outline_color = pal_lut[220];
        for (int i = 0; i < LOGO_EDGE_COUNT; i++) {
            int sx = logo_edge_x[i];
            int sy = logo_edge_y[i];
            int dx_base = sx * SCALE_FACTOR;
            int dy_base = sy * SCALE_FACTOR;
            for (int ry = 0; ry < SCALE_FACTOR; ry++) {
                uint16_t *row = fb + (dy_base + ry) * LCD_H_RES;
                for (int rx = 0; rx < SCALE_FACTOR; rx++)
                    row[dx_base + rx] = outline_color;
            }
        }
    }

    // When source is the panel's own framebuffer, draw_bitmap swaps without copying
    esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, LCD_H_RES, LCD_V_RES, fb);
    if (xSemaphoreTake(refresh_finish, pdMS_TO_TICKS(500)) != pdTRUE)
        ESP_LOGW(TAG, "vsync timeout — on_color_trans_done never fired");
    cur_fb = 1 - cur_fb;
}
