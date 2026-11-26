#include "feeder_hx711_test.h"

#include "feeder_board_config.h"

#if FEEDER_HX711_TEST_ONLY

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "feeder_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hx711.h"
#include "sdkconfig.h"

#define HX711_STARTUP_MS 500
#define HX711_TEST_TIMEOUT_MS 300
#define HX711_REPORT_MS 1000
#define HX711_REPORT_SAMPLES 3
#define HX711_ZERO_BAND_G 0.5f
#define HX711_COMMAND_LENGTH 32

static esp_err_t read_test_average(hx711_t *scale, uint8_t samples, int32_t *raw)
{
    esp_err_t result = hx711_read_average(scale, samples, raw);
    if (result == ESP_OK && (*raw == -8388608 || *raw == 8388607)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return result;
}

static esp_err_t tare_scale(hx711_t *scale)
{
    int32_t raw = 0;
    esp_err_t result = read_test_average(scale, FEEDER_HX711_SAMPLE_COUNT, &raw);
    if (result == ESP_OK) {
        hx711_set_offset(scale, raw);
        FEEDER_LOGI(HX711, "去皮完成 | 当前重量 0.00 g");
    } else {
        FEEDER_LOGW(HX711, "去皮失败 | %s, 检查供电、DOUT/SCK 和称重传感器接线",
                    esp_err_to_name(result));
    }
    return result;
}

static void handle_command(hx711_t *scale, const char *line, bool *calibrated)
{
    if (strcmp(line, "t") == 0) {
        tare_scale(scale);
        return;
    }

    if (line[0] != 'c' || !isspace((unsigned char)line[1])) {
        FEEDER_LOGW(HX711, "命令无效 | t 空秤去皮，c <克数> 标定");
        return;
    }

    const char *argument = line + 2;
    char *end = NULL;
    float grams = strtof(argument, &end);
    if (end == argument || !isfinite(grams) || grams <= 0.0f) {
        FEEDER_LOGW(HX711, "标定失败 | 请输入正的已知重量，例如 c 100");
        return;
    }
    while (isspace((unsigned char)*end)) end++;
    if (*end != '\0') {
        FEEDER_LOGW(HX711, "标定失败 | 克数后只能有空格");
        return;
    }

    int32_t raw = 0;
    esp_err_t result = read_test_average(scale, FEEDER_HX711_SAMPLE_COUNT, &raw);
    if (result != ESP_OK) {
        FEEDER_LOGW(HX711, "标定采样失败 | %s", esp_err_to_name(result));
        return;
    }
    const float counts_per_gram = (float)(raw - scale->offset) / grams;
    if (!isfinite(counts_per_gram) || counts_per_gram == 0.0f) {
        FEEDER_LOGW(HX711, "标定失败 | 原始值未有效变化，放好已知重量再试");
        return;
    }

    hx711_set_scale(scale, counts_per_gram);
    *calibrated = true;
    FEEDER_LOGI(HX711, "标定完成 | 参考重量=%.2f g, 系数=%.6f counts/g",
                grams, counts_per_gram);
}

void feeder_hx711_test_run(void)
{
    const hx711_config_t config = {
        .dout_gpio = FEEDER_HX711_DOUT_GPIO,
        .sck_gpio = FEEDER_HX711_SCK_GPIO,
        .ready_timeout_ms = HX711_TEST_TIMEOUT_MS,
        .scale = FEEDER_HX711_SCALE,
    };
    hx711_t scale = {0};
    esp_err_t result = hx711_init(&scale, &config);
    if (result != ESP_OK) {
        FEEDER_LOGE(HX711, "初始化 GPIO 失败 | %s", esp_err_to_name(result));
        return;
    }
    FEEDER_LOGI(HX711, "独立测试启动 | DOUT=GPIO%d, SCK=GPIO%d, 请保持空秤",
                config.dout_gpio, config.sck_gpio);

    const uart_port_t console = CONFIG_ESP_CONSOLE_UART_NUM;
    result = uart_is_driver_installed(console) ? ESP_OK
        : uart_driver_install(console, 256, 0, 0, NULL, 0);
    const bool input_ready = result == ESP_OK;
    if (!input_ready) {
        FEEDER_LOGE(HX711, "串口输入初始化失败 | %s", esp_err_to_name(result));
    }

    vTaskDelay(pdMS_TO_TICKS(HX711_STARTUP_MS));
    while (tare_scale(&scale) != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(HX711_REPORT_MS));
    }
    FEEDER_LOGI(HX711, "初始化完成 | 每秒输出克数，默认系数=%.2f counts/g",
                scale.config.scale);

    bool calibrated = false;
    bool line_overflow = false;
    size_t line_length = 0;
    char line[HX711_COMMAND_LENGTH];
    TickType_t last_report = xTaskGetTickCount();
    while (true) {
        uint8_t character;
        if (input_ready && uart_read_bytes(console, &character, 1, 2) == 1) {
            if (character == '\r' || character == '\n') {
                if (line_overflow) {
                    FEEDER_LOGW(HX711, "命令过长 | t 或 c <克数>");
                } else if (line_length > 0) {
                    line[line_length] = '\0';
                    handle_command(&scale, line, &calibrated);
                }
                line_length = 0;
                line_overflow = false;
            } else if (character == '\b' || character == 127) {
                if (line_length > 0) line_length--;
            } else if (line_length < sizeof(line) - 1U) {
                line[line_length++] = (char)character;
            } else {
                line_overflow = true;
            }
        } else if (!input_ready) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        TickType_t now = xTaskGetTickCount();
        if (now - last_report < pdMS_TO_TICKS(HX711_REPORT_MS)) continue;
        last_report = now;
        int32_t raw = 0;
        result = read_test_average(&scale, HX711_REPORT_SAMPLES, &raw);
        if (result != ESP_OK) {
            FEEDER_LOGW(HX711, "采样失败 | %s", esp_err_to_name(result));
        } else {
            float weight_g = (raw - scale.offset) / scale.config.scale;
            if (fabsf(weight_g) < HX711_ZERO_BAND_G) weight_g = 0.0f;
            FEEDER_LOGI(HX711, "当前重量 | %.2f g%s", weight_g,
                        calibrated ? "" : "（估算）");
        }
    }
}

#endif
