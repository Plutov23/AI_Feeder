#include "feeder_log.h"

#include "esp_system.h"

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_POWERON:
            return "上电";
        case ESP_RST_EXT:
            return "外部复位";
        case ESP_RST_SW:
            return "软件重启";
        case ESP_RST_PANIC:
            return "程序异常";
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
            return "看门狗";
        case ESP_RST_BROWNOUT:
        case ESP_RST_PWR_GLITCH:
            return "供电异常";
        default:
            return "其他原因";
    }
}

void feeder_log_boot(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    if (reason == ESP_RST_BROWNOUT || reason == ESP_RST_PWR_GLITCH) {
        FEEDER_LOGW(SYSTEM, "启动 | 原因=%s, reset=%d",
                    reset_reason_name(reason), (int)reason);
        return;
    }
    FEEDER_LOGI(SYSTEM, "启动 | 原因=%s, reset=%d",
                reset_reason_name(reason), (int)reason);
}
