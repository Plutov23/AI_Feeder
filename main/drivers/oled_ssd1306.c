#include "oled_ssd1306.h"

#include <ctype.h>
#include <string.h>

#include "feeder_log.h"
#include "freertos/FreeRTOS.h"

#define OLED_CONTROL_COMMAND 0x00
#define OLED_CONTROL_DATA 0x40

typedef struct {
    char character;
    uint8_t columns[5];
} oled_glyph_t;

static const oled_glyph_t oled_glyphs[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
    {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
    {'.', {0x00, 0x60, 0x60, 0x00, 0x00}},
    {':', {0x00, 0x36, 0x36, 0x00, 0x00}},
    {'%', {0x62, 0x64, 0x08, 0x13, 0x23}},
    {'?', {0x02, 0x01, 0x51, 0x09, 0x06}},
    {'0', {0x3E, 0x51, 0x49, 0x45, 0x3E}},
    {'1', {0x00, 0x42, 0x7F, 0x40, 0x00}},
    {'2', {0x42, 0x61, 0x51, 0x49, 0x46}},
    {'3', {0x21, 0x41, 0x45, 0x4B, 0x31}},
    {'4', {0x18, 0x14, 0x12, 0x7F, 0x10}},
    {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
    {'6', {0x3C, 0x4A, 0x49, 0x49, 0x30}},
    {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
    {'8', {0x36, 0x49, 0x49, 0x49, 0x36}},
    {'9', {0x06, 0x49, 0x49, 0x29, 0x1E}},
    {'A', {0x7E, 0x11, 0x11, 0x11, 0x7E}},
    {'B', {0x7F, 0x49, 0x49, 0x49, 0x36}},
    {'C', {0x3E, 0x41, 0x41, 0x41, 0x22}},
    {'D', {0x7F, 0x41, 0x41, 0x22, 0x1C}},
    {'E', {0x7F, 0x49, 0x49, 0x49, 0x41}},
    {'F', {0x7F, 0x09, 0x09, 0x09, 0x01}},
    {'G', {0x3E, 0x41, 0x49, 0x49, 0x7A}},
    {'H', {0x7F, 0x08, 0x08, 0x08, 0x7F}},
    {'I', {0x00, 0x41, 0x7F, 0x41, 0x00}},
    {'J', {0x20, 0x40, 0x41, 0x3F, 0x01}},
    {'K', {0x7F, 0x08, 0x14, 0x22, 0x41}},
    {'L', {0x7F, 0x40, 0x40, 0x40, 0x40}},
    {'M', {0x7F, 0x02, 0x0C, 0x02, 0x7F}},
    {'N', {0x7F, 0x04, 0x08, 0x10, 0x7F}},
    {'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}},
    {'P', {0x7F, 0x09, 0x09, 0x09, 0x06}},
    {'Q', {0x3E, 0x41, 0x51, 0x21, 0x5E}},
    {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}},
    {'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
    {'T', {0x01, 0x01, 0x7F, 0x01, 0x01}},
    {'U', {0x3F, 0x40, 0x40, 0x40, 0x3F}},
    {'V', {0x1F, 0x20, 0x40, 0x20, 0x1F}},
    {'W', {0x3F, 0x40, 0x38, 0x40, 0x3F}},
    {'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
    {'Y', {0x07, 0x08, 0x70, 0x08, 0x07}},
    {'Z', {0x61, 0x51, 0x49, 0x45, 0x43}},
};

static esp_err_t oled_write(
    oled_ssd1306_t *display,
    uint8_t control,
    const uint8_t *data,
    size_t data_length
)
{
    if (data_length > OLED_SSD1306_WIDTH) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t buffer[OLED_SSD1306_WIDTH + 1];
    buffer[0] = control;
    memcpy(&buffer[1], data, data_length);
    return i2c_master_write_to_device(
        display->config.i2c_port,
        display->config.address,
        buffer,
        data_length + 1,
        pdMS_TO_TICKS(100)
    );
}

static esp_err_t oled_write_commands(
    oled_ssd1306_t *display,
    const uint8_t *commands,
    size_t command_count
)
{
    return oled_write(
        display,
        OLED_CONTROL_COMMAND,
        commands,
        command_count
    );
}

static const uint8_t *oled_find_glyph(char character)
{
    char normalized = (char)toupper((unsigned char)character);
    for (size_t index = 0;
         index < sizeof(oled_glyphs) / sizeof(oled_glyphs[0]);
         index++) {
        if (oled_glyphs[index].character == normalized) {
            return oled_glyphs[index].columns;
        }
    }
    return oled_glyphs[5].columns;
}

static esp_err_t oled_set_cursor(
    oled_ssd1306_t *display,
    uint8_t page,
    uint8_t column
)
{
    uint8_t commands[] = {
        (uint8_t)(0xB0U | page),
        (uint8_t)(0x00U | (column & 0x0FU)),
        (uint8_t)(0x10U | ((column >> 4U) & 0x0FU)),
    };
    return oled_write_commands(display, commands, sizeof(commands));
}

esp_err_t oled_ssd1306_init(
    oled_ssd1306_t *display,
    const oled_ssd1306_config_t *config
)
{
    if (display == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_config_t i2c_config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = config->sda_gpio,
        .scl_io_num = config->scl_gpio,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = config->frequency_hz,
        .clk_flags = 0,
    };
    FEEDER_RETURN_ON_ERROR(
        OLED, i2c_param_config(config->i2c_port, &i2c_config), "配置 I2C 失败"
    );

    esp_err_t install_result = i2c_driver_install(
        config->i2c_port,
        I2C_MODE_MASTER,
        0,
        0,
        0
    );
    if (install_result != ESP_OK && install_result != ESP_ERR_INVALID_STATE) {
        return install_result;
    }

    display->config = *config;
    display->initialized = true;

    const uint8_t init_commands[] = {
        0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
        0xA1, 0xC8, 0xDA, 0x12, 0x81, 0xCF, 0xD9, 0xF1,
        0xDB, 0x30, 0xA4, 0xA6, 0x8D, 0x14, 0xAF,
    };
    FEEDER_RETURN_ON_ERROR(
        OLED,
        oled_write_commands(
            display,
            init_commands,
            sizeof(init_commands)
        ),
        "发送初始化命令失败"
    );
    return oled_ssd1306_clear(display);
}

esp_err_t oled_ssd1306_clear(oled_ssd1306_t *display)
{
    if (display == NULL || !display->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t blank_line[OLED_SSD1306_WIDTH] = {0};
    for (uint8_t page = 0; page < OLED_SSD1306_PAGE_COUNT; page++) {
        FEEDER_RETURN_ON_ERROR(OLED, oled_set_cursor(display, page, 0),
                               "设置光标失败");
        FEEDER_RETURN_ON_ERROR(
            OLED,
            oled_write(
                display,
                OLED_CONTROL_DATA,
                blank_line,
                sizeof(blank_line)
            ),
            "清屏失败"
        );
    }
    return ESP_OK;
}

esp_err_t oled_ssd1306_write_text(
    oled_ssd1306_t *display,
    uint8_t page,
    uint8_t column,
    const char *text
)
{
    if (display == NULL || text == NULL || !display->initialized ||
        page >= OLED_SSD1306_PAGE_COUNT || column >= OLED_SSD1306_WIDTH) {
        return ESP_ERR_INVALID_ARG;
    }

    FEEDER_RETURN_ON_ERROR(OLED, oled_set_cursor(display, page, column),
                           "设置光标失败");

    uint8_t output[OLED_SSD1306_WIDTH] = {0};
    size_t output_length = 0;
    while (*text != '\0' && output_length + 6 <= sizeof(output)) {
        const uint8_t *glyph = oled_find_glyph(*text++);
        memcpy(&output[output_length], glyph, 5);
        output_length += 5;
        output[output_length++] = 0x00;
    }

    return oled_write(
        display,
        OLED_CONTROL_DATA,
        output,
        output_length
    );
}
