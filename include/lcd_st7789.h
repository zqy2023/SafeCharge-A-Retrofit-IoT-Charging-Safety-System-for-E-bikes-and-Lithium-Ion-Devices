#pragma once

#include "lvgl.h"

#define LCD_H_RES 240
#define LCD_V_RES 320

void lcd_st7789_init(void);
void lcd_st7789_attach_lvgl(lv_disp_drv_t *disp_drv);
