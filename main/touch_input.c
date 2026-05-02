//
// Cthugha ESP32-P4 Port — Capacitive touch input
// GT911 over I2C — maps gestures to Cthugha mode changes
//

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "display.h"
#include "touch_input.h"

static const char *TAG = "cthugha_touch";

static esp_lcd_touch_handle_t touch_handle = NULL;
static i2c_master_bus_handle_t s_i2c_bus = NULL;

i2c_master_bus_handle_t touch_get_i2c_bus(void)
{
    return s_i2c_bus;
}

// Gesture detection state
static int last_x = -1, last_y = -1;
static int start_x = -1, start_y = -1;
static TickType_t press_start = 0;
static bool was_pressed = false;
static uint8_t max_fingers = 0; // peak finger count during current press

#define SWIPE_THRESHOLD  60
#define LONG_PRESS_MS   800

void touch_input_init(void)
{
    ESP_LOGI(TAG, "Initializing GT911 touch on I2C%d (SDA=%d, SCL=%d)",
             CONFIG_CTHUGHA_TOUCH_I2C_NUM,
             CONFIG_CTHUGHA_TOUCH_I2C_SDA_GPIO,
             CONFIG_CTHUGHA_TOUCH_I2C_SCL_GPIO);

    // I2C master bus
    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port = CONFIG_CTHUGHA_TOUCH_I2C_NUM,
        .sda_io_num = (gpio_num_t)CONFIG_CTHUGHA_TOUCH_I2C_SDA_GPIO,
        .scl_io_num = (gpio_num_t)CONFIG_CTHUGHA_TOUCH_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &s_i2c_bus));
    i2c_master_bus_handle_t i2c_bus = s_i2c_bus;

    // GT911 touch panel
    esp_lcd_panel_io_handle_t tp_io;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg =
        ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_cfg, &tp_io));

    esp_lcd_touch_config_t touch_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = (gpio_num_t)CONFIG_CTHUGHA_TOUCH_RST_GPIO,
        .int_gpio_num = (gpio_num_t)CONFIG_CTHUGHA_TOUCH_INT_GPIO,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(tp_io, &touch_cfg, &touch_handle));

    ESP_LOGI(TAG, "Touch initialized");
}

touch_gesture_t touch_input_poll(void)
{
    if (!touch_handle)
        return TOUCH_NONE;

    esp_lcd_touch_read_data(touch_handle);

    uint16_t x[3], y[3];
    uint16_t strength[3];
    uint8_t count = 0;
    bool pressed = esp_lcd_touch_get_coordinates(touch_handle,
                                                  x, y, strength, &count, 3);

    touch_gesture_t gesture = TOUCH_NONE;

    if (pressed && count > 0) {
        if (!was_pressed) {
            start_x = x[0];
            start_y = y[0];
            press_start = xTaskGetTickCount();
            max_fingers = 0;
        }
        if (count > max_fingers) max_fingers = count;
        last_x = x[0];
        last_y = y[0];
        was_pressed = true;
    } else if (was_pressed) {
        // Released — multi-finger taps take priority over single-finger gestures
        was_pressed = false;
        TickType_t duration = xTaskGetTickCount() - press_start;
        int dx = last_x - start_x;
        int dy = last_y - start_y;

        if (max_fingers >= 3) {
            gesture = TOUCH_THREE_FINGER_TAP;
        } else if (max_fingers == 2) {
            gesture = TOUCH_TWO_FINGER_TAP;
        } else if (duration > pdMS_TO_TICKS(LONG_PRESS_MS)) {
            gesture = TOUCH_LONG_PRESS;
        } else if (abs(dx) > SWIPE_THRESHOLD || abs(dy) > SWIPE_THRESHOLD) {
            if (abs(dx) > abs(dy)) {
                gesture = (dx > 0) ? TOUCH_SWIPE_RIGHT : TOUCH_SWIPE_LEFT;
            } else {
                gesture = (dy > 0) ? TOUCH_SWIPE_DOWN : TOUCH_SWIPE_UP;
            }
        } else {
            gesture = TOUCH_TAP;
        }
    }

    return gesture;
}
