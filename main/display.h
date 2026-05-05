#pragma once

#include <stdint.h>
#include "cthugha.h"

#define LCD_H_RES    720
#define LCD_V_RES    720
#define LCD_BPP      16
#define SCALE_FACTOR (LCD_H_RES / BUFF_WIDTH)

void display_init(void);
void display_render(void);

extern int logo_overlay_enabled; // set 1 to stamp logo outline over every frame
