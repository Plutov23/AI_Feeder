#include "ir_sensor.h"

#include "esp_check.h"
#include "esp_timer.h"

esp_err_t ir_sensor_init(
    ir_sensor_t *sensor,
    const ir_sensor_config_t *config
)
{
    if (sensor == NULL || config == NULL || config->active_level > 1U) {
        return ESP_ERR_INVALID_ARG;
    }

    gpio_config_t gpio_config_options = {
        .pin_bit_mask = 1ULL << config->gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = config->enable_pull_up
            ? GPIO_PULLUP_ENABLE
            : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(
        gpio_config(&gpio_config_options),
        "IR_SENSOR",
        "GPIO config"
    );

    bool active = gpio_get_level(config->gpio) == config->active_level;
    sensor->config = *config;
    sensor->initialized = true;
    sensor->last_raw_active = active;
    sensor->stable_active = active;
    sensor->last_change_us = esp_timer_get_time();
    sensor->event_count = 0;
    return ESP_OK;
}

esp_err_t ir_sensor_poll(ir_sensor_t *sensor, bool *new_event)
{
    if (sensor == NULL || new_event == NULL || !sensor->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    *new_event = false;
    bool raw_active =
        gpio_get_level(sensor->config.gpio) == sensor->config.active_level;
    int64_t now_us = esp_timer_get_time();

    if (raw_active != sensor->last_raw_active) {
        sensor->last_raw_active = raw_active;
        sensor->last_change_us = now_us;
    }

    int64_t debounce_us = (int64_t)sensor->config.debounce_ms * 1000;
    if (raw_active != sensor->stable_active &&
        now_us - sensor->last_change_us >= debounce_us) {
        sensor->stable_active = raw_active;
        if (raw_active) {
            sensor->event_count++;
            *new_event = true;
        }
    }

    return ESP_OK;
}

bool ir_sensor_is_active(const ir_sensor_t *sensor)
{
    return sensor != NULL && sensor->initialized && sensor->stable_active;
}

uint32_t ir_sensor_get_event_count(const ir_sensor_t *sensor)
{
    return sensor != NULL ? sensor->event_count : 0;
}

void ir_sensor_reset_event_count(ir_sensor_t *sensor)
{
    if (sensor != NULL) {
        sensor->event_count = 0;
    }
}
