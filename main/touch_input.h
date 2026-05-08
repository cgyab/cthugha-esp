#pragma once

#include <stdbool.h>
#include "driver/i2c_master.h"

typedef enum {
    TOUCH_NONE = 0,
    TOUCH_TAP,
    TOUCH_SWIPE_LEFT,
    TOUCH_SWIPE_RIGHT,
    TOUCH_SWIPE_UP,
    TOUCH_SWIPE_DOWN,
    TOUCH_LONG_PRESS,
    TOUCH_DOUBLE_TAP,
    TOUCH_TWO_FINGER_TAP,
    TOUCH_TWO_FINGER_SWIPE_DOWN,
    TOUCH_THREE_FINGER_TAP,
    TOUCH_FOUR_FINGER_TAP,
    TOUCH_FIVE_FINGER_TAP,
} touch_gesture_t;

void touch_input_init(void);
touch_gesture_t touch_input_poll(void);
i2c_master_bus_handle_t touch_get_i2c_bus(void);
