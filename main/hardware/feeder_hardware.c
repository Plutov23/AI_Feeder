#include "feeder_hardware.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "feeder_board_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hx711.h"
#include "ir_sensor.h"
#include "oled_ssd1306.h"
#include "servo.h"
#include "esp_log.h"

static const char *TAG = "FEEDER_HW";
static feeder_hardware_snapshot_t current_snapshot;
static SemaphoreHandle_t snapshot_mutex;
static bool test_task_started;

#if FEEDER_ENABLE_HX711
static hx711_t scale_device;
#endif

#if FEEDER_ENABLE_DS1302
static ds1302_t rtc_device;
#endif

#if FEEDER_ENABLE_IR_SENSOR
static ir_sensor_t infrared_sensor;
#endif

#if FEEDER_ENABLE_OLED
static oled_ssd1306_t oled_display;
#endif

#if FEEDER_ENABLE_SERVO
static servo_t feeder_servo;
#endif

static void snapshot_lock(void)
{
    if (snapshot_mutex != NULL) {
        xSemaphoreTake(snapshot_mutex, portMAX_DELAY);
    }
}

static void snapshot_unlock(void)
{
    if (snapshot_mutex != NULL) {
        xSemaphoreGive(snapshot_mutex);
    }
}

#if FEEDER_ENABLE_OLED
static void update_oled(void)
{
    char line[22];
    feeder_hardware_snapshot_t snapshot;
    if (feeder_hardware_get_snapshot(&snapshot) != ESP_OK) {
        return;
    }

    oled_ssd1306_clear(&oled_display);
    oled_ssd1306_write_text(&oled_display, 0, 0, "AI FEEDER");

    if (snapshot.weight_valid) {
        snprintf(line, sizeof(line), "WT:%0.1fG", snapshot.weight_g);
    } else {
        snprintf(line, sizeof(line), "WT:---");
    }
    oled_ssd1306_write_text(&oled_display, 2, 0, line);

    snprintf(
        line,
        sizeof(line),
        "IR:%" PRIu32,
        snapshot.feeding_event_count
    );
    oled_ssd1306_write_text(&oled_display, 4, 0, line);

    if (snapshot.rtc_valid) {
        snprintf(
            line,
            sizeof(line),
            "%02u:%02u:%02u",
            snapshot.time.hour,
            snapshot.time.minute,
            snapshot.time.second
        );
    } else {
        snprintf(line, sizeof(line), "TIME:--:--:--");
    }
    oled_ssd1306_write_text(&oled_display, 6, 0, line);
}
#endif

static void read_hardware_once(void)
{
#if FEEDER_ENABLE_HX711
    float weight_g = 0.0f;
    esp_err_t weight_result = hx711_read_weight_g(&scale_device, &weight_g);
    snapshot_lock();
    current_snapshot.weight_valid = weight_result == ESP_OK;
    if (weight_result == ESP_OK) {
        current_snapshot.weight_g = weight_g < 0.0f ? 0.0f : weight_g;
    }
    snapshot_unlock();

    if (weight_result == ESP_OK) {
        ESP_LOGI(TAG, "HX711 weight: %.1f g", weight_g);
    } else {
        ESP_LOGW(TAG, "HX711 read failed: %s", esp_err_to_name(weight_result));
    }
#endif

#if FEEDER_ENABLE_DS1302
    ds1302_datetime_t time = {0};
    esp_err_t rtc_result = ds1302_get_time(&rtc_device, &time);
    snapshot_lock();
    current_snapshot.rtc_valid = rtc_result == ESP_OK;
    if (rtc_result == ESP_OK) {
        current_snapshot.time = time;
    }
    snapshot_unlock();

    if (rtc_result == ESP_OK) {
        ESP_LOGI(
            TAG,
            "DS1302 time: 20%02u-%02u-%02u %02u:%02u:%02u",
            time.year,
            time.month,
            time.day,
            time.hour,
            time.minute,
            time.second
        );
    } else {
        ESP_LOGW(TAG, "DS1302 time is not set or invalid");
    }
#endif

#if FEEDER_ENABLE_OLED
    update_oled();
#endif
}

static void hardware_test_task(void *argument)
{
    (void)argument;
    TickType_t last_report_tick = xTaskGetTickCount();
#if FEEDER_ENABLE_IR_SENSOR
    bool infrared_status_reported = false;
    bool last_infrared_active = false;
#endif

    while (true) {
#if FEEDER_ENABLE_IR_SENSOR
        bool new_event = false;
        if (ir_sensor_poll(&infrared_sensor, &new_event) == ESP_OK) {
            bool infrared_active = ir_sensor_is_active(&infrared_sensor);
            uint32_t event_count = ir_sensor_get_event_count(&infrared_sensor);
            snapshot_lock();
            current_snapshot.infrared_active = infrared_active;
            current_snapshot.feeding_event_count = event_count;
            snapshot_unlock();

            if (!infrared_status_reported ||
                infrared_active != last_infrared_active) {
                ESP_LOGI(
                    TAG,
                    "红外 GPIO%d: %s (level=%d)",
                    FEEDER_IR_GPIO,
                    infrared_active ? "有" : "无",
                    infrared_active ? FEEDER_IR_ACTIVE_LEVEL
                                    : !FEEDER_IR_ACTIVE_LEVEL
                );
                infrared_status_reported = true;
                last_infrared_active = infrared_active;
            }
        }
#endif

        TickType_t now = xTaskGetTickCount();
        if (now - last_report_tick >=
            pdMS_TO_TICKS(FEEDER_HARDWARE_REPORT_MS)) {
            last_report_tick = now;
            read_hardware_once();

#if FEEDER_ENABLE_SERVO && FEEDER_SERVO_AUTO_TEST
            static uint32_t report_count;
            report_count++;
            if (report_count % 5U == 0U) {
                ESP_LOGI(TAG, "Running servo dispense test");
                feeder_hardware_dispense_once();
            }
#endif
        }

        vTaskDelay(pdMS_TO_TICKS(FEEDER_HARDWARE_POLL_MS));
    }
}

esp_err_t feeder_hardware_init(void)
{
    memset(&current_snapshot, 0, sizeof(current_snapshot));
    snapshot_mutex = xSemaphoreCreateMutex();
    if (snapshot_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t first_error = ESP_OK;

#if FEEDER_ENABLE_HX711
    const hx711_config_t hx711_config = {
        .dout_gpio = FEEDER_HX711_DOUT_GPIO,
        .sck_gpio = FEEDER_HX711_SCK_GPIO,
        .ready_timeout_ms = FEEDER_HX711_TIMEOUT_MS,
        .scale = FEEDER_HX711_SCALE,
    };
    esp_err_t hx711_result = hx711_init(&scale_device, &hx711_config);
    if (hx711_result == ESP_OK) {
        ESP_LOGI(TAG, "HX711 initialized; tare is available on demand");
    } else {
        ESP_LOGE(
            TAG,
            "HX711 init failed: %s",
            esp_err_to_name(hx711_result)
        );
        first_error = hx711_result;
    }
#endif

#if FEEDER_ENABLE_DS1302
    const ds1302_config_t ds1302_config = {
        .ce_gpio = FEEDER_DS1302_CE_GPIO,
        .sclk_gpio = FEEDER_DS1302_SCLK_GPIO,
        .io_gpio = FEEDER_DS1302_IO_GPIO,
    };
    esp_err_t ds1302_result = ds1302_init(&rtc_device, &ds1302_config);
    if (ds1302_result == ESP_OK) {
        ESP_LOGI(TAG, "DS1302 initialized");
    } else {
        ESP_LOGE(
            TAG,
            "DS1302 init failed: %s",
            esp_err_to_name(ds1302_result)
        );
        if (first_error == ESP_OK) {
            first_error = ds1302_result;
        }
    }
#endif

#if FEEDER_ENABLE_IR_SENSOR
    const ir_sensor_config_t ir_config = {
        .gpio = FEEDER_IR_GPIO,
        .active_level = FEEDER_IR_ACTIVE_LEVEL,
        .debounce_ms = FEEDER_IR_DEBOUNCE_MS,
        .enable_pull_up = true,
    };
    esp_err_t ir_result = ir_sensor_init(&infrared_sensor, &ir_config);
    if (ir_result == ESP_OK) {
        ESP_LOGI(TAG, "Infrared sensor initialized");
    } else {
        ESP_LOGE(
            TAG,
            "Infrared init failed: %s",
            esp_err_to_name(ir_result)
        );
        if (first_error == ESP_OK) {
            first_error = ir_result;
        }
    }
#endif

#if FEEDER_ENABLE_OLED
    const oled_ssd1306_config_t oled_config = {
        .i2c_port = FEEDER_OLED_I2C_PORT,
        .sda_gpio = FEEDER_OLED_SDA_GPIO,
        .scl_gpio = FEEDER_OLED_SCL_GPIO,
        .frequency_hz = FEEDER_OLED_I2C_FREQUENCY_HZ,
        .address = FEEDER_OLED_I2C_ADDRESS,
    };
    esp_err_t oled_result = oled_ssd1306_init(&oled_display, &oled_config);
    if (oled_result == ESP_OK) {
        oled_ssd1306_write_text(&oled_display, 0, 0, "AI FEEDER");
        oled_ssd1306_write_text(&oled_display, 2, 0, "OLED READY");
        ESP_LOGI(TAG, "OLED initialized");
    } else {
        ESP_LOGE(
            TAG,
            "OLED init failed: %s",
            esp_err_to_name(oled_result)
        );
        if (first_error == ESP_OK) {
            first_error = oled_result;
        }
    }
#endif

#if FEEDER_ENABLE_SERVO
    const servo_config_t servo_config = {
        .gpio = FEEDER_SERVO_GPIO,
        .speed_mode = FEEDER_SERVO_LEDC_MODE,
        .timer = FEEDER_SERVO_LEDC_TIMER,
        .channel = FEEDER_SERVO_LEDC_CHANNEL,
        .min_pulse_us = FEEDER_SERVO_MIN_PULSE_US,
        .max_pulse_us = FEEDER_SERVO_MAX_PULSE_US,
        .min_angle = 0.0f,
        .max_angle = 180.0f,
    };
    esp_err_t servo_result = servo_init(&feeder_servo, &servo_config);
    if (servo_result == ESP_OK) {
        servo_set_angle(&feeder_servo, FEEDER_SERVO_CLOSED_ANGLE);
        ESP_LOGI(TAG, "Servo initialized at closed angle");
    } else {
        ESP_LOGE(
            TAG,
            "Servo init failed: %s",
            esp_err_to_name(servo_result)
        );
        if (first_error == ESP_OK) {
            first_error = servo_result;
        }
    }
#endif

    if (!FEEDER_ANY_MODULE_ENABLED) {
        ESP_LOGI(
            TAG,
            "Hardware modules are disabled in feeder_board_config.h"
        );
    }

    return first_error;
}

esp_err_t feeder_hardware_start_test_task(void)
{
#if !FEEDER_ENABLE_HARDWARE_TEST_TASK
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!FEEDER_ANY_MODULE_ENABLED) {
        return ESP_OK;
    }
    if (test_task_started) {
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t task_result = xTaskCreate(
        hardware_test_task,
        "hardware_test",
        4096,
        NULL,
        4,
        NULL
    );
    if (task_result != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    test_task_started = true;
    return ESP_OK;
#endif
}

esp_err_t feeder_hardware_get_snapshot(feeder_hardware_snapshot_t *snapshot)
{
    if (snapshot == NULL || snapshot_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    snapshot_lock();
    *snapshot = current_snapshot;
    snapshot_unlock();
    return ESP_OK;
}

esp_err_t feeder_hardware_tare_scale(void)
{
#if FEEDER_ENABLE_HX711
    esp_err_t result = hx711_tare(
        &scale_device,
        FEEDER_HX711_SAMPLE_COUNT
    );
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "HX711 tare complete");
    }
    return result;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t feeder_hardware_set_time(const ds1302_datetime_t *time)
{
#if FEEDER_ENABLE_DS1302
    return ds1302_set_time(&rtc_device, time);
#else
    (void)time;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t feeder_hardware_dispense_once(void)
{
#if FEEDER_ENABLE_SERVO
    return servo_dispense(
        &feeder_servo,
        FEEDER_SERVO_OPEN_ANGLE,
        FEEDER_SERVO_CLOSED_ANGLE,
        FEEDER_SERVO_HOLD_MS
    );
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
