#ifndef FEEDER_SERVO_H
#define FEEDER_SERVO_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"

typedef struct {
    gpio_num_t gpio;
    ledc_mode_t speed_mode;
    ledc_timer_t timer;
    ledc_channel_t channel;
    uint16_t min_pulse_us;
    uint16_t max_pulse_us;
    float min_angle;
    float max_angle;
} servo_config_t;

typedef struct {
    servo_config_t config;
    bool initialized;
    float current_angle;
} servo_t;

esp_err_t servo_init(servo_t *servo, const servo_config_t *config);
esp_err_t servo_set_angle(servo_t *servo, float angle);
esp_err_t servo_dispense(
    servo_t *servo,
    float open_angle,
    float closed_angle,
    uint32_t hold_ms
);

#endif
