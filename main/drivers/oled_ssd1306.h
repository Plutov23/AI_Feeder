#ifndef FEEDER_OLED_SSD1306_H
#define FEEDER_OLED_SSD1306_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_err.h"

#define OLED_SSD1306_WIDTH 128
#define OLED_SSD1306_HEIGHT 64
#define OLED_SSD1306_PAGE_COUNT 8

typedef struct {
    i2c_port_t i2c_port;
    gpio_num_t sda_gpio;
    gpio_num_t scl_gpio;
    uint32_t frequency_hz;
    uint8_t address;
} oled_ssd1306_config_t;

typedef struct {
    oled_ssd1306_config_t config;
    bool initialized;
} oled_ssd1306_t;

esp_err_t oled_ssd1306_init(
    oled_ssd1306_t *display,
    const oled_ssd1306_config_t *config
);
esp_err_t oled_ssd1306_clear(oled_ssd1306_t *display);
esp_err_t oled_ssd1306_write_text(
    oled_ssd1306_t *display,
    uint8_t page,
    uint8_t column,
    const char *text
);

#endif
