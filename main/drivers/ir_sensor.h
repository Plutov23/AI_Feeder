#ifndef FEEDER_IR_SENSOR_H
#define FEEDER_IR_SENSOR_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

typedef struct {
    gpio_num_t gpio;
    uint8_t active_level;
    uint32_t debounce_ms;
    bool enable_pull_up;
} ir_sensor_config_t;

typedef struct {
    ir_sensor_config_t config;
    bool initialized;
    bool last_raw_active;
    bool stable_active;
    int64_t last_change_us;
    uint32_t event_count;
} ir_sensor_t;

esp_err_t ir_sensor_init(
    ir_sensor_t *sensor,
    const ir_sensor_config_t *config
);
esp_err_t ir_sensor_poll(ir_sensor_t *sensor, bool *new_event);
bool ir_sensor_is_active(const ir_sensor_t *sensor);
uint32_t ir_sensor_get_event_count(const ir_sensor_t *sensor);
void ir_sensor_reset_event_count(ir_sensor_t *sensor);

#endif
