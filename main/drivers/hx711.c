#include "hx711.h"

#include <limits.h>

#include "esp_check.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static portMUX_TYPE hx711_spinlock = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t hx711_wait_ready(hx711_t *device)
{
    int64_t start_time_us = esp_timer_get_time();
    int64_t timeout_us = (int64_t)device->config.ready_timeout_ms * 1000;

    while (gpio_get_level(device->config.dout_gpio) != 0) {
        if (esp_timer_get_time() - start_time_us >= timeout_us) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    return ESP_OK;
}

esp_err_t hx711_init(hx711_t *device, const hx711_config_t *config)
{
    if (device == NULL || config == NULL || config->scale == 0.0f) {
        return ESP_ERR_INVALID_ARG;
    }

    gpio_config_t output_config = {
        .pin_bit_mask = 1ULL << config->sck_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config_t input_config = {
        .pin_bit_mask = 1ULL << config->dout_gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&output_config), "HX711", "SCK GPIO");
    ESP_RETURN_ON_ERROR(gpio_config(&input_config), "HX711", "DOUT GPIO");
    ESP_RETURN_ON_ERROR(
        gpio_set_level(config->sck_gpio, 0),
        "HX711",
        "SCK level"
    );

    device->config = *config;
    device->offset = 0;
    device->initialized = true;
    return ESP_OK;
}

esp_err_t hx711_read_raw(hx711_t *device, int32_t *raw_value)
{
    if (device == NULL || raw_value == NULL || !device->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(hx711_wait_ready(device), "HX711", "not ready");

    uint32_t value = 0;

    taskENTER_CRITICAL(&hx711_spinlock);
    for (uint8_t bit = 0; bit < 24; bit++) {
        gpio_set_level(device->config.sck_gpio, 1);
        esp_rom_delay_us(1);
        value = (value << 1) |
                (uint32_t)gpio_get_level(device->config.dout_gpio);
        gpio_set_level(device->config.sck_gpio, 0);
        esp_rom_delay_us(1);
    }

    /* Pulse 25 selects channel A with gain 128 for the next conversion. */
    gpio_set_level(device->config.sck_gpio, 1);
    esp_rom_delay_us(1);
    gpio_set_level(device->config.sck_gpio, 0);
    esp_rom_delay_us(1);
    taskEXIT_CRITICAL(&hx711_spinlock);

    if ((value & 0x00800000U) != 0U) {
        value |= 0xFF000000U;
    }

    *raw_value = (int32_t)value;
    return ESP_OK;
}

esp_err_t hx711_read_average(
    hx711_t *device,
    uint8_t sample_count,
    int32_t *average
)
{
    if (device == NULL || average == NULL || sample_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int64_t sum = 0;
    for (uint8_t sample = 0; sample < sample_count; sample++) {
        int32_t raw_value = 0;
        ESP_RETURN_ON_ERROR(
            hx711_read_raw(device, &raw_value),
            "HX711",
            "read sample"
        );
        sum += raw_value;
    }

    int64_t result = sum / sample_count;
    if (result > INT32_MAX || result < INT32_MIN) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *average = (int32_t)result;
    return ESP_OK;
}

esp_err_t hx711_tare(hx711_t *device, uint8_t sample_count)
{
    int32_t offset = 0;
    ESP_RETURN_ON_ERROR(
        hx711_read_average(device, sample_count, &offset),
        "HX711",
        "tare"
    );
    device->offset = offset;
    return ESP_OK;
}

esp_err_t hx711_read_weight_g(hx711_t *device, float *weight_g)
{
    if (device == NULL || weight_g == NULL || !device->initialized ||
        device->config.scale == 0.0f) {
        return ESP_ERR_INVALID_STATE;
    }

    int32_t raw_value = 0;
    ESP_RETURN_ON_ERROR(
        hx711_read_raw(device, &raw_value),
        "HX711",
        "weight"
    );

    *weight_g = (raw_value - device->offset) / device->config.scale;
    return ESP_OK;
}

void hx711_set_scale(hx711_t *device, float scale)
{
    if (device != NULL && scale != 0.0f) {
        device->config.scale = scale;
    }
}

void hx711_set_offset(hx711_t *device, int32_t offset)
{
    if (device != NULL) {
        device->offset = offset;
    }
}
