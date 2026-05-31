#include "lcd_st7789.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"

#define LCD_HOST        SPI2_HOST

// ===== ESP32-S3 wiring =====
// TFT SCL/SCK/CLK -> GPIO12
// TFT SDA/MOSI/DIN -> GPIO11
// TFT CS -> GPIO10
// TFT DC/A0 -> GPIO9
// TFT RES/RST -> GPIO8
// TFT BL/LED -> GPIO7
#define LCD_MOSI        11
#define LCD_SCLK        12
#define LCD_CS          10
#define LCD_DC          9
#define LCD_RST         8
#define LCD_BL          7

#define LCD_X_GAP       0
#define LCD_Y_GAP       0
#define LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000)

static esp_lcd_panel_handle_t s_panel = NULL;

static void lcd_lvgl_flush_cb(lv_disp_drv_t *drv,
                              const lv_area_t *area,
                              lv_color_t *color_map)
{
    esp_lcd_panel_draw_bitmap(s_panel,
                              area->x1 + LCD_X_GAP,
                              area->y1 + LCD_Y_GAP,
                              area->x2 + LCD_X_GAP + 1,
                              area->y2 + LCD_Y_GAP + 1,
                              color_map);

    lv_disp_flush_ready(drv);
}

void lcd_st7789_init(void)
{
    gpio_config_t bk_gpio_config = {
        .pin_bit_mask = 1ULL << LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));
    gpio_set_level(LCD_BL, 0);

    spi_bus_config_t buscfg = {
        .sclk_io_num = LCD_SCLK,
        .mosi_io_num = LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 40 * sizeof(lv_color_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_DC,
        .cs_gpio_num = LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                             &io_config,
                                             &io_handle));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

    // If white becomes black, change true to false.
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));

    // Portrait: 240 x 320
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, false, false));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, false));

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    gpio_set_level(LCD_BL, 1);
}

void lcd_st7789_attach_lvgl(lv_disp_drv_t *disp_drv)
{
    disp_drv->hor_res = LCD_H_RES;
    disp_drv->ver_res = LCD_V_RES;
    disp_drv->flush_cb = lcd_lvgl_flush_cb;
}
