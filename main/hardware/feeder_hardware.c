#include "feeder_hardware.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "feeder_board_config.h"
#include "feeder_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hx711.h"
#include "ir_sensor.h"
#include "oled_ssd1306.h"
#include "servo.h"

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
#if FEEDER_SERVO_AUTO_TEST
static const float servo_test_angles[] = {45.0f, 80.0f, 45.0f, 10.0f};
#endif

static void log_servo_target(const char *action)
{
    FEEDER_LOGI(SERVO, "%s | SG90, GPIO%d, 目标角度=%.0f°, 脉宽=%u us",
                action, FEEDER_SERVO_GPIO, feeder_servo.current_angle,
                (unsigned)feeder_servo.current_pulse_us);
}
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

#if FEEDER_ENABLE_DS1302 && FEEDER_DS1302_SET_BUILD_TIME_ON_BOOT
static esp_err_t seed_ds1302_from_build_time(void)
{
    static const char *const months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    char month_name[4];
    int year;
    struct tm build_time = { .tm_isdst = -1 };
    if (sscanf(__DATE__ " " __TIME__, "%3s %d %d %d:%d:%d",
               month_name, &build_time.tm_mday, &year, &build_time.tm_hour,
               &build_time.tm_min, &build_time.tm_sec) != 6 ||
        year < 2000 || year > 2099) {
        return ESP_ERR_INVALID_ARG;
    }

    while (build_time.tm_mon < 12 &&
           strcmp(month_name, months[build_time.tm_mon]) != 0) {
        build_time.tm_mon++;
    }
    build_time.tm_year = year - 1900;
    if (build_time.tm_mon == 12 || mktime(&build_time) == (time_t)-1) {
        return ESP_ERR_INVALID_ARG;
    }

    const ds1302_datetime_t initial_time = {
        .year = (uint8_t)(year - 2000),
        .month = (uint8_t)(build_time.tm_mon + 1),
        .day = (uint8_t)build_time.tm_mday,
        .day_of_week = (uint8_t)(build_time.tm_wday + 1),
        .hour = (uint8_t)build_time.tm_hour,
        .minute = (uint8_t)build_time.tm_min,
        .second = (uint8_t)build_time.tm_sec,
    };
    esp_err_t result = ds1302_set_time(&rtc_device, &initial_time);
    if (result != ESP_OK) {
        return result;
    }

    ds1302_datetime_t readback = {0};
    result = ds1302_get_time(&rtc_device, &readback);
    if (result != ESP_OK) {
        return result;
    }
    return memcmp(&initial_time, &readback, sizeof(initial_time)) == 0
        ? ESP_OK
        : ESP_ERR_INVALID_RESPONSE;
}
#endif

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
        FEEDER_LOGI(HX711, "读取重量 | %.1f g", weight_g);
    } else {
        FEEDER_LOGW(HX711, "读取重量失败 | %s", esp_err_to_name(weight_result));
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
        FEEDER_LOGI(
            DS1302,
            "读取时间 | 20%02u-%02u-%02u %02u:%02u:%02u",
            time.year,
            time.month,
            time.day,
            time.hour,
            time.minute,
            time.second
        );
    } else {
        FEEDER_LOGW(DS1302, "读取时间失败 | %s", esp_err_to_name(rtc_result));
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
#if FEEDER_ENABLE_SERVO && FEEDER_SERVO_AUTO_TEST
    size_t next_servo_test_angle = 1;
    const size_t servo_test_angle_count =
        sizeof(servo_test_angles) / sizeof(servo_test_angles[0]);
#endif
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
                FEEDER_LOGI(
                    IR,
                    "检测状态变化 | GPIO%d, %s, 电平=%d, 累计=%" PRIu32,
                    FEEDER_IR_GPIO,
                    infrared_active ? "有" : "无",
                    infrared_active ? FEEDER_IR_ACTIVE_LEVEL
                                    : !FEEDER_IR_ACTIVE_LEVEL,
                    event_count
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
            if (feeder_servo.initialized) {
                float angle = servo_test_angles[next_servo_test_angle];
                esp_err_t result = servo_set_angle(&feeder_servo, angle);
                if (result == ESP_OK) {
                    log_servo_target("转角测试");
                    next_servo_test_angle =
                        (next_servo_test_angle + 1) % servo_test_angle_count;
                } else {
                    FEEDER_LOGW(SERVO, "转角测试失败 | 目标=%.0f°, %s",
                                angle, esp_err_to_name(result));
                }
            }
#endif
        }

        vTaskDelay(pdMS_TO_TICKS(FEEDER_HARDWARE_POLL_MS));
    }
}

esp_err_t feeder_hardware_init(void)
{
    FEEDER_LOGI(HW, "模块配置 | 舵机=%d, 红外=%d, DS1302=%d, HX711=%d, OLED=%d",
                FEEDER_ENABLE_SERVO, FEEDER_ENABLE_IR_SENSOR,
                FEEDER_ENABLE_DS1302, FEEDER_ENABLE_HX711, FEEDER_ENABLE_OLED);
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
        FEEDER_LOGI(HX711, "初始化完成 | DOUT=GPIO%d, SCK=GPIO%d",
                    FEEDER_HX711_DOUT_GPIO, FEEDER_HX711_SCK_GPIO);
    } else {
        FEEDER_LOGE(HX711, "初始化失败 | %s", esp_err_to_name(hx711_result));
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
#if FEEDER_DS1302_SET_BUILD_TIME_ON_BOOT
    if (ds1302_result == ESP_OK) {
        ds1302_result = seed_ds1302_from_build_time();
        if (ds1302_result == ESP_OK) {
            FEEDER_LOGI(DS1302, "开机校时并回读成功 | 编译时间=%s %s",
                        __DATE__, __TIME__);
        } else {
            FEEDER_LOGE(DS1302, "开机校时或回读失败 | %s",
                        esp_err_to_name(ds1302_result));
        }
    }
#endif
    if (ds1302_result == ESP_OK) {
        FEEDER_LOGI(DS1302, "初始化完成 | CE=GPIO%d, CLK=GPIO%d, DAT=GPIO%d",
                    FEEDER_DS1302_CE_GPIO, FEEDER_DS1302_SCLK_GPIO,
                    FEEDER_DS1302_IO_GPIO);
    } else {
        FEEDER_LOGE(DS1302, "模块启动失败 | %s", esp_err_to_name(ds1302_result));
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
        FEEDER_LOGI(IR, "初始化完成 | GPIO%d, 有效电平=%d, 防抖=%u ms",
                    FEEDER_IR_GPIO, FEEDER_IR_ACTIVE_LEVEL,
                    (unsigned)FEEDER_IR_DEBOUNCE_MS);
    } else {
        FEEDER_LOGE(IR, "初始化失败 | %s", esp_err_to_name(ir_result));
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
        FEEDER_LOGI(OLED, "初始化完成 | I2C 地址=0x%02X",
                    (unsigned)FEEDER_OLED_I2C_ADDRESS);
    } else {
        FEEDER_LOGE(OLED, "初始化失败 | %s", esp_err_to_name(oled_result));
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
        .max_angle = FEEDER_SERVO_MAX_ANGLE,
#if FEEDER_SERVO_AUTO_TEST
        .initial_angle = servo_test_angles[0],
#else
        .initial_angle = FEEDER_SERVO_CLOSED_ANGLE,
#endif
    };
    esp_err_t servo_result = servo_init(&feeder_servo, &servo_config);
    if (servo_result == ESP_OK) {
        log_servo_target("初始化完成");
    } else {
        FEEDER_LOGE(SERVO, "初始化失败 | %s", esp_err_to_name(servo_result));
        if (first_error == ESP_OK) {
            first_error = servo_result;
        }
    }
#endif

    if (!FEEDER_ANY_MODULE_ENABLED) {
        FEEDER_LOGI(HW, "所有外设已停用");
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
    FEEDER_LOGI(HW, "调试任务已启动 | 采样=%u ms, 报告=%u ms",
                (unsigned)FEEDER_HARDWARE_POLL_MS,
                (unsigned)FEEDER_HARDWARE_REPORT_MS);
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
        FEEDER_LOGI(HX711, "去皮完成 | 样本=%u", (unsigned)FEEDER_HX711_SAMPLE_COUNT);
    } else {
        FEEDER_LOGW(HX711, "去皮失败 | %s", esp_err_to_name(result));
    }
    return result;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t feeder_hardware_set_time(const ds1302_datetime_t *time)
{
#if FEEDER_ENABLE_DS1302
    esp_err_t result = ds1302_set_time(&rtc_device, time);
    if (result == ESP_OK) {
        FEEDER_LOGI(DS1302, "设置时间成功 | 20%02u-%02u-%02u %02u:%02u:%02u",
                    time->year, time->month, time->day,
                    time->hour, time->minute, time->second);
    } else {
        FEEDER_LOGW(DS1302, "设置时间失败 | %s", esp_err_to_name(result));
    }
    return result;
#else
    (void)time;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t feeder_hardware_dispense_once(void)
{
#if FEEDER_ENABLE_SERVO
    FEEDER_LOGI(FEED, "开始单次出粮动作 | 开启=%.0f°, 保持=%u ms, 关闭=%.0f°",
                FEEDER_SERVO_OPEN_ANGLE, (unsigned)FEEDER_SERVO_HOLD_MS,
                FEEDER_SERVO_CLOSED_ANGLE);
    esp_err_t result = servo_dispense(
        &feeder_servo,
        FEEDER_SERVO_OPEN_ANGLE,
        FEEDER_SERVO_CLOSED_ANGLE,
        FEEDER_SERVO_HOLD_MS
    );
    if (result == ESP_OK) {
        FEEDER_LOGI(FEED, "单次出粮动作完成 | 已发送关闭指令");
    } else {
        FEEDER_LOGE(FEED, "单次出粮动作失败 | %s", esp_err_to_name(result));
    }
    return result;
#else
    FEEDER_LOGW(FEED, "无法执行出粮动作 | 舵机模块已停用");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
