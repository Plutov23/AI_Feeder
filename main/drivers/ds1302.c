#include "ds1302.h"

#include "feeder_log.h"
#include "esp_rom_sys.h"

#define DS1302_REG_SECONDS 0x80
#define DS1302_REG_MINUTES 0x82
#define DS1302_REG_HOURS 0x84
#define DS1302_REG_DATE 0x86
#define DS1302_REG_MONTH 0x88
#define DS1302_REG_DAY 0x8A
#define DS1302_REG_YEAR 0x8C
#define DS1302_REG_WRITE_PROTECT 0x8E
#define DS1302_REG_CLOCK_BURST 0xBE
#define DS1302_EDGE_DELAY_US 4

static uint8_t decimal_to_bcd(uint8_t value)
{
    return (uint8_t)(((value / 10U) << 4U) | (value % 10U));
}

static uint8_t bcd_to_decimal(uint8_t value)
{
    return (uint8_t)(((value >> 4U) * 10U) + (value & 0x0FU));
}

static void ds1302_set_io_output(ds1302_t *device)
{
    gpio_set_direction(device->config.io_gpio, GPIO_MODE_OUTPUT);
}

static void ds1302_set_io_input(ds1302_t *device)
{
    gpio_set_direction(device->config.io_gpio, GPIO_MODE_INPUT);
}

static void ds1302_write_byte(ds1302_t *device, uint8_t value, bool release_io)
{
    ds1302_set_io_output(device);

    for (uint8_t bit = 0; bit < 8; bit++) {
        gpio_set_level(device->config.sclk_gpio, 0);
        gpio_set_level(device->config.io_gpio, (value >> bit) & 0x01U);
        esp_rom_delay_us(DS1302_EDGE_DELAY_US);
        gpio_set_level(device->config.sclk_gpio, 1);
        esp_rom_delay_us(DS1302_EDGE_DELAY_US);
    }
    /* The RTC drives the first read bit on this falling edge. */
    if (release_io) {
        ds1302_set_io_input(device);
    }
    gpio_set_level(device->config.sclk_gpio, 0);
    esp_rom_delay_us(DS1302_EDGE_DELAY_US);
}

static uint8_t ds1302_read_byte(ds1302_t *device)
{
    uint8_t value = 0;
    for (uint8_t bit = 0; bit < 8; bit++) {
        if (gpio_get_level(device->config.io_gpio) != 0) {
            value |= (uint8_t)(1U << bit);
        }
        gpio_set_level(device->config.sclk_gpio, 1);
        esp_rom_delay_us(DS1302_EDGE_DELAY_US);
        gpio_set_level(device->config.sclk_gpio, 0);
        esp_rom_delay_us(DS1302_EDGE_DELAY_US);
    }

    return value;
}

static void ds1302_write_register(
    ds1302_t *device,
    uint8_t register_address,
    uint8_t value
)
{
    gpio_set_level(device->config.ce_gpio, 1);
    esp_rom_delay_us(DS1302_EDGE_DELAY_US);
    ds1302_write_byte(device, register_address & 0xFEU, false);
    ds1302_write_byte(device, value, false);
    gpio_set_level(device->config.ce_gpio, 0);
    esp_rom_delay_us(DS1302_EDGE_DELAY_US);
}

static void ds1302_read_clock(ds1302_t *device, uint8_t registers[7])
{
    gpio_set_level(device->config.ce_gpio, 1);
    esp_rom_delay_us(DS1302_EDGE_DELAY_US);
    ds1302_write_byte(device, DS1302_REG_CLOCK_BURST | 0x01U, true);
    for (uint8_t index = 0; index < 7; index++) {
        registers[index] = ds1302_read_byte(device);
    }
    gpio_set_level(device->config.ce_gpio, 0);
    esp_rom_delay_us(DS1302_EDGE_DELAY_US);
}

static uint8_t ds1302_decode_hour(uint8_t value)
{
    if ((value & 0x80U) == 0U) {
        return bcd_to_decimal(value & 0x3FU);
    }

    uint8_t hour = bcd_to_decimal(value & 0x1FU);
    bool is_pm = (value & 0x20U) != 0U;
    if (hour == 12U) {
        hour = 0;
    }
    return (uint8_t)(hour + (is_pm ? 12U : 0U));
}

esp_err_t ds1302_init(ds1302_t *device, const ds1302_config_t *config)
{
    if (device == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    gpio_config_t output_config = {
        .pin_bit_mask = (1ULL << config->ce_gpio) |
                        (1ULL << config->sclk_gpio) |
                        (1ULL << config->io_gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    FEEDER_RETURN_ON_ERROR(DS1302, gpio_config(&output_config), "配置 GPIO 失败");

    device->config = *config;
    device->initialized = true;
    gpio_set_level(config->ce_gpio, 0);
    gpio_set_level(config->sclk_gpio, 0);
    gpio_set_level(config->io_gpio, 0);
    return ESP_OK;
}

bool ds1302_time_is_valid(const ds1302_datetime_t *time)
{
    return time != NULL &&
           time->month >= 1U && time->month <= 12U &&
           time->day >= 1U && time->day <= 31U &&
           time->day_of_week >= 1U && time->day_of_week <= 7U &&
           time->hour <= 23U && time->minute <= 59U &&
           time->second <= 59U;
}

esp_err_t ds1302_get_time(ds1302_t *device, ds1302_datetime_t *time)
{
    if (device == NULL || time == NULL || !device->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t registers[7];
    ds1302_read_clock(device, registers);
    if ((registers[0] & 0x80U) != 0U) {
        return ESP_ERR_INVALID_STATE;
    }

    time->second = bcd_to_decimal(registers[0] & 0x7FU);
    time->minute = bcd_to_decimal(registers[1] & 0x7FU);
    time->hour = ds1302_decode_hour(registers[2]);
    time->day = bcd_to_decimal(registers[3] & 0x3FU);
    time->month = bcd_to_decimal(registers[4] & 0x1FU);
    time->day_of_week = registers[5] & 0x07U;
    time->year = bcd_to_decimal(registers[6]);

    return ds1302_time_is_valid(time) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t ds1302_set_time(
    ds1302_t *device,
    const ds1302_datetime_t *time
)
{
    if (device == NULL || !device->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!ds1302_time_is_valid(time)) {
        return ESP_ERR_INVALID_ARG;
    }

    ds1302_write_register(device, DS1302_REG_WRITE_PROTECT, 0x00);
    ds1302_write_register(device, DS1302_REG_YEAR, decimal_to_bcd(time->year));
    ds1302_write_register(device, DS1302_REG_MONTH, decimal_to_bcd(time->month));
    ds1302_write_register(device, DS1302_REG_DATE, decimal_to_bcd(time->day));
    ds1302_write_register(device, DS1302_REG_DAY, time->day_of_week);
    ds1302_write_register(device, DS1302_REG_HOURS, decimal_to_bcd(time->hour));
    ds1302_write_register(device, DS1302_REG_MINUTES, decimal_to_bcd(time->minute));
    ds1302_write_register(
        device,
        DS1302_REG_SECONDS,
        decimal_to_bcd(time->second) & 0x7FU
    );
    ds1302_write_register(device, DS1302_REG_WRITE_PROTECT, 0x80);
    return ESP_OK;
}
