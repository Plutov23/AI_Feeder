#ifndef FEEDER_DS1302_H
#define FEEDER_DS1302_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

typedef struct {
    gpio_num_t ce_gpio;
    gpio_num_t sclk_gpio;
    gpio_num_t io_gpio;
} ds1302_config_t;

typedef struct {
    uint8_t year;
    uint8_t month;
    uint8_t day;
    uint8_t day_of_week;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} ds1302_datetime_t;

typedef struct {
    ds1302_config_t config;
    bool initialized;
} ds1302_t;

esp_err_t ds1302_init(ds1302_t *device, const ds1302_config_t *config);
esp_err_t ds1302_get_time(ds1302_t *device, ds1302_datetime_t *time);
esp_err_t ds1302_set_time(
    ds1302_t *device,
    const ds1302_datetime_t *time
);
bool ds1302_time_is_valid(const ds1302_datetime_t *time);

#endif
