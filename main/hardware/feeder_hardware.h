#ifndef FEEDER_HARDWARE_H
#define FEEDER_HARDWARE_H

#include <stdbool.h>
#include <stdint.h>

#include "ds1302.h"
#include "esp_err.h"

typedef struct {
    bool weight_valid;
    float weight_g;
    bool rtc_valid;
    ds1302_datetime_t time;
    bool infrared_active;
    uint32_t feeding_event_count;
} feeder_hardware_snapshot_t;

esp_err_t feeder_hardware_init(void);
esp_err_t feeder_hardware_start_test_task(void);
esp_err_t feeder_hardware_get_snapshot(feeder_hardware_snapshot_t *snapshot);
esp_err_t feeder_hardware_tare_scale(void);
esp_err_t feeder_hardware_set_time(const ds1302_datetime_t *time);
esp_err_t feeder_hardware_dispense_once(void);

#endif
