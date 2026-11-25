#ifndef FEEDER_HX711_H
#define FEEDER_HX711_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

typedef struct {
    gpio_num_t dout_gpio;
    gpio_num_t sck_gpio;
    uint32_t ready_timeout_ms;
    float scale;
} hx711_config_t;

typedef struct {
    hx711_config_t config;
    int32_t offset;
    bool initialized;
} hx711_t;

esp_err_t hx711_init(hx711_t *device, const hx711_config_t *config);
esp_err_t hx711_read_raw(hx711_t *device, int32_t *raw_value);
esp_err_t hx711_read_average(
    hx711_t *device,
    uint8_t sample_count,
    int32_t *average
);
esp_err_t hx711_tare(hx711_t *device, uint8_t sample_count);
esp_err_t hx711_read_weight_g(hx711_t *device, float *weight_g);
void hx711_set_scale(hx711_t *device, float scale);
void hx711_set_offset(hx711_t *device, int32_t offset);

#endif
