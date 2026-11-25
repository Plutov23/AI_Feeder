#include "servo.h"

#include <math.h>

#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SERVO_FREQUENCY_HZ 50
#define SERVO_PERIOD_US 20000
#define SERVO_DUTY_RESOLUTION LEDC_TIMER_14_BIT

static uint32_t servo_pulse_to_duty(uint16_t pulse_us)
{
    const uint32_t maximum_duty = (1U << 14U) - 1U;
    return (uint32_t)(((uint64_t)pulse_us * maximum_duty) /
                      SERVO_PERIOD_US);
}

esp_err_t servo_init(servo_t *servo, const servo_config_t *config)
{
    if (servo == NULL || config == NULL ||
        config->min_pulse_us >= config->max_pulse_us ||
        config->min_angle >= config->max_angle) {
        return ESP_ERR_INVALID_ARG;
    }

    ledc_timer_config_t timer_config = {
        .speed_mode = config->speed_mode,
        .duty_resolution = SERVO_DUTY_RESOLUTION,
        .timer_num = config->timer,
        .freq_hz = SERVO_FREQUENCY_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(
        ledc_timer_config(&timer_config),
        "SERVO",
        "timer config"
    );

    ledc_channel_config_t channel_config = {
        .gpio_num = config->gpio,
        .speed_mode = config->speed_mode,
        .channel = config->channel,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = config->timer,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = 0,
    };
    ESP_RETURN_ON_ERROR(
        ledc_channel_config(&channel_config),
        "SERVO",
        "channel config"
    );

    servo->config = *config;
    servo->initialized = true;
    servo->current_angle = config->min_angle;
    return servo_set_angle(servo, config->min_angle);
}

esp_err_t servo_set_angle(servo_t *servo, float angle)
{
    if (servo == NULL || !servo->initialized || !isfinite(angle)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (angle < servo->config.min_angle) {
        angle = servo->config.min_angle;
    } else if (angle > servo->config.max_angle) {
        angle = servo->config.max_angle;
    }

    float ratio = (angle - servo->config.min_angle) /
                  (servo->config.max_angle - servo->config.min_angle);
    uint16_t pulse_us = (uint16_t)(
        servo->config.min_pulse_us +
        ratio * (servo->config.max_pulse_us - servo->config.min_pulse_us)
    );
    uint32_t duty = servo_pulse_to_duty(pulse_us);

    ESP_RETURN_ON_ERROR(
        ledc_set_duty(
            servo->config.speed_mode,
            servo->config.channel,
            duty
        ),
        "SERVO",
        "set duty"
    );
    ESP_RETURN_ON_ERROR(
        ledc_update_duty(
            servo->config.speed_mode,
            servo->config.channel
        ),
        "SERVO",
        "update duty"
    );

    servo->current_angle = angle;
    return ESP_OK;
}

esp_err_t servo_dispense(
    servo_t *servo,
    float open_angle,
    float closed_angle,
    uint32_t hold_ms
)
{
    ESP_RETURN_ON_ERROR(
        servo_set_angle(servo, open_angle),
        "SERVO",
        "open"
    );
    vTaskDelay(pdMS_TO_TICKS(hold_ms));
    return servo_set_angle(servo, closed_angle);
}
