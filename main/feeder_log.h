#ifndef FEEDER_LOG_H
#define FEEDER_LOG_H

#include "esp_check.h"
#include "esp_log.h"

#define FEEDER_LOG_LABEL_SYSTEM "系统"
#define FEEDER_LOG_LABEL_HW "硬件"
#define FEEDER_LOG_LABEL_SERVO "舵机"
#define FEEDER_LOG_LABEL_FEED "出粮"
#define FEEDER_LOG_LABEL_DS1302 "DS1302"
#define FEEDER_LOG_LABEL_IR "红外"
#define FEEDER_LOG_LABEL_HX711 "HX711 称重"
#define FEEDER_LOG_LABEL_OLED "OLED"
#define FEEDER_LOG_LABEL_WIFI "Wi-Fi"
#define FEEDER_LOG_LABEL_ONENET "OneNET"
#define FEEDER_LOG_LABEL_DEEPSEEK "DeepSeek"

#define FEEDER_LOGI(module, ...) \
    ESP_LOGI(#module, FEEDER_LOG_LABEL_##module " | " __VA_ARGS__)
#define FEEDER_LOGW(module, ...) \
    ESP_LOGW(#module, FEEDER_LOG_LABEL_##module " | " __VA_ARGS__)
#define FEEDER_LOGE(module, ...) \
    ESP_LOGE(#module, FEEDER_LOG_LABEL_##module " | " __VA_ARGS__)

#define FEEDER_RETURN_ON_ERROR(module, expression, ...) \
    ESP_RETURN_ON_ERROR((expression), #module, \
                        FEEDER_LOG_LABEL_##module " | " __VA_ARGS__)

void feeder_log_boot(void);

#endif
