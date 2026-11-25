#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "feeder_hardware.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs_flash.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT BIT1
#define WIFI_MAXIMUM_RETRY 10
#define HTTP_RESPONSE_BUFFER_SIZE 8192
#define AI_REPLY_BUFFER_SIZE 513
#define DEEPSEEK_MAX_RETRIES 3
#define DEEPSEEK_RETRY_DELAY_MS 3000

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} http_response_buffer_t;

static const char *TAG = "AI_FEEDER";
static EventGroupHandle_t wifi_event_group;
static SemaphoreHandle_t ai_task_gate;
static esp_mqtt_client_handle_t mqtt_client;
static bool mqtt_connected;
static int wifi_retry_count;
static unsigned long message_id = 1;
static char latest_ai_reply[AI_REPLY_BUFFER_SIZE] =
    "ESP32 已上线，等待 AI 指令";

static void copy_utf8_truncated(
    char *destination,
    size_t destination_size,
    const char *source
)
{
    if (destination_size == 0) {
        return;
    }

    size_t copy_length = strlen(source);
    if (copy_length >= destination_size) {
        copy_length = destination_size - 1;
        while (copy_length > 0 &&
               ((unsigned char)source[copy_length] & 0xC0) == 0x80) {
            copy_length--;
        }
    }

    memcpy(destination, source, copy_length);
    destination[copy_length] = '\0';
}

static bool topic_equals(
    const esp_mqtt_event_handle_t event,
    const char *expected_topic
)
{
    size_t expected_length = strlen(expected_topic);
    return event->topic_len == (int)expected_length &&
           strncmp(event->topic, expected_topic, expected_length) == 0;
}

static void publish_property_set_reply(
    const char *request_id,
    int code,
    const char *message
)
{
    if (!mqtt_connected || mqtt_client == NULL) {
        return;
    }

    char topic[208];
    char payload[256];

    snprintf(
        topic,
        sizeof(topic),
        "$sys/%s/%s/thing/property/set_reply",
        ONENET_PRODUCT_ID,
        ONENET_DEVICE_NAME
    );
    snprintf(
        payload,
        sizeof(payload),
        "{\"id\":\"%s\",\"code\":%d,\"msg\":\"%s\"}",
        request_id,
        code,
        message
    );

    esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 0);
}

static void publish_ai_state(const char *reply, bool command_active)
{
    copy_utf8_truncated(
        latest_ai_reply,
        sizeof(latest_ai_reply),
        reply
    );

    if (!mqtt_connected || mqtt_client == NULL) {
        ESP_LOGW(TAG, "MQTT offline, AI state saved locally");
        return;
    }

    char topic[192];
    snprintf(
        topic,
        sizeof(topic),
        "$sys/%s/%s/thing/property/post",
        ONENET_PRODUCT_ID,
        ONENET_DEVICE_NAME
    );

    cJSON *root = cJSON_CreateObject();
    cJSON *params = cJSON_CreateObject();
    cJSON *reply_property = cJSON_CreateObject();
    cJSON *command_property = cJSON_CreateObject();

    if (root == NULL || params == NULL || reply_property == NULL ||
        command_property == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(params);
        cJSON_Delete(reply_property);
        cJSON_Delete(command_property);
        ESP_LOGE(TAG, "Unable to allocate OneNET payload");
        return;
    }

    char id_buffer[24];
    snprintf(id_buffer, sizeof(id_buffer), "%lu", message_id++);
    cJSON_AddStringToObject(root, "id", id_buffer);
    cJSON_AddStringToObject(root, "version", "1.0");
    cJSON_AddStringToObject(reply_property, "value", latest_ai_reply);
    cJSON_AddBoolToObject(command_property, "value", command_active);
    cJSON_AddItemToObject(params, "ai_reply", reply_property);
    cJSON_AddItemToObject(params, "ai_cmd", command_property);
    cJSON_AddItemToObject(root, "params", params);

    char *payload = cJSON_PrintUnformatted(root);
    if (payload != NULL) {
        int result = esp_mqtt_client_publish(
            mqtt_client,
            topic,
            payload,
            0,
            1,
            0
        );
        if (result >= 0) {
            ESP_LOGI(TAG, "AI state published to OneNET");
        } else {
            ESP_LOGE(TAG, "Failed to publish AI state");
        }
        free(payload);
    }

    cJSON_Delete(root);
}

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }

    http_response_buffer_t *response = event->user_data;
    size_t available = response->capacity - response->length - 1;
    size_t copy_length = (size_t)event->data_len;
    if (copy_length > available) {
        copy_length = available;
    }

    if (copy_length > 0) {
        memcpy(
            response->data + response->length,
            event->data,
            copy_length
        );
        response->length += copy_length;
        response->data[response->length] = '\0';
    }

    return ESP_OK;
}

static char *create_deepseek_request(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *messages = cJSON_CreateArray();
    cJSON *system_message = cJSON_CreateObject();
    cJSON *user_message = cJSON_CreateObject();

    if (root == NULL || messages == NULL || system_message == NULL ||
        user_message == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(messages);
        cJSON_Delete(system_message);
        cJSON_Delete(user_message);
        return NULL;
    }

    cJSON_AddStringToObject(root, "model", DEEPSEEK_MODEL);
    cJSON_AddStringToObject(
        system_message,
        "role",
        "system"
    );
    cJSON_AddStringToObject(
        system_message,
        "content",
        "你是宠物智能喂食器助手，只返回简短中文结果。"
    );
    cJSON_AddStringToObject(user_message, "role", "user");
    cJSON_AddStringToObject(user_message, "content", AI_TEST_PROMPT);
    cJSON_AddItemToArray(messages, system_message);
    cJSON_AddItemToArray(messages, user_message);
    cJSON_AddItemToObject(root, "messages", messages);
    cJSON_AddNumberToObject(root, "temperature", 0.3);
    cJSON_AddNumberToObject(root, "max_tokens", 160);
    cJSON_AddBoolToObject(root, "stream", false);

    char *request_body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return request_body;
}

static esp_err_t call_deepseek(char *reply, size_t reply_size)
{
    if (strcmp(DEEPSEEK_API_KEY, "YOUR_DEEPSEEK_API_KEY") == 0 ||
        strlen(DEEPSEEK_API_KEY) < 10) {
        copy_utf8_truncated(
            reply,
            reply_size,
            "DeepSeek API Key 尚未配置"
        );
        return ESP_ERR_INVALID_STATE;
    }

    char *request_body = create_deepseek_request();
    char *response_data = calloc(1, HTTP_RESPONSE_BUFFER_SIZE);
    if (request_body == NULL || response_data == NULL) {
        free(request_body);
        free(response_data);
        copy_utf8_truncated(reply, reply_size, "内存不足，AI 请求失败");
        return ESP_ERR_NO_MEM;
    }

    http_response_buffer_t response = {
        .data = response_data,
        .length = 0,
        .capacity = HTTP_RESPONSE_BUFFER_SIZE,
    };

    esp_http_client_config_t config = {
        .url = DEEPSEEK_API_URL,
        .event_handler = http_event_handler,
        .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(request_body);
        free(response_data);
        copy_utf8_truncated(reply, reply_size, "无法创建 HTTPS 客户端");
        return ESP_FAIL;
    }

    char authorization[192];
    snprintf(
        authorization,
        sizeof(authorization),
        "Bearer %s",
        DEEPSEEK_API_KEY
    );

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(
        client,
        "Content-Type",
        "application/json"
    );
    esp_http_client_set_header(client, "Authorization", authorization);
    esp_http_client_set_post_field(
        client,
        request_body,
        strlen(request_body)
    );

    ESP_LOGI(TAG, "Calling DeepSeek...");
    esp_err_t result = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);

    if (result != ESP_OK) {
        snprintf(
            reply,
            reply_size,
            "DeepSeek 网络请求失败：%s",
            esp_err_to_name(result)
        );
    } else if (status_code != 200) {
        snprintf(
            reply,
            reply_size,
            "DeepSeek 返回 HTTP %d",
            status_code
        );
        result = ESP_FAIL;
    } else {
        cJSON *root = cJSON_Parse(response.data);
        cJSON *choices = cJSON_GetObjectItem(root, "choices");
        cJSON *first_choice = cJSON_GetArrayItem(choices, 0);
        cJSON *message = cJSON_GetObjectItem(first_choice, "message");
        cJSON *content = cJSON_GetObjectItem(message, "content");

        if (!cJSON_IsString(content) || content->valuestring == NULL) {
            copy_utf8_truncated(
                reply,
                reply_size,
                "DeepSeek 返回内容解析失败"
            );
            result = ESP_FAIL;
        } else {
            copy_utf8_truncated(reply, reply_size, content->valuestring);
            ESP_LOGI(TAG, "DeepSeek analysis received");
        }
        cJSON_Delete(root);
    }

    esp_http_client_cleanup(client);
    free(request_body);
    free(response_data);
    return result;
}

static void deepseek_task(void *argument)
{
    char reply[AI_REPLY_BUFFER_SIZE];
    esp_err_t result = ESP_FAIL;

    for (int attempt = 1; attempt <= DEEPSEEK_MAX_RETRIES; attempt++) {
        result = call_deepseek(reply, sizeof(reply));
        if (result == ESP_OK) {
            break;
        }

        ESP_LOGW(
            TAG,
            "DeepSeek request failed, attempt %d/%d",
            attempt,
            DEEPSEEK_MAX_RETRIES
        );
        if (attempt < DEEPSEEK_MAX_RETRIES) {
            vTaskDelay(pdMS_TO_TICKS(DEEPSEEK_RETRY_DELAY_MS));
        }
    }

    publish_ai_state(reply, false);
    xSemaphoreGive(ai_task_gate);
    vTaskDelete(NULL);
}

static void handle_property_set(
    const char *data,
    size_t data_length
)
{
    cJSON *root = cJSON_ParseWithLength(data, data_length);
    if (root == NULL) {
        ESP_LOGE(TAG, "Invalid OneNET property-set JSON");
        return;
    }

    char request_id[32] = "0";
    cJSON *id = cJSON_GetObjectItem(root, "id");
    if (cJSON_IsString(id) && id->valuestring != NULL) {
        strlcpy(request_id, id->valuestring, sizeof(request_id));
    } else if (cJSON_IsNumber(id)) {
        snprintf(request_id, sizeof(request_id), "%.0f", id->valuedouble);
    }

    cJSON *params = cJSON_GetObjectItem(root, "params");
    cJSON *command = cJSON_GetObjectItem(params, "ai_cmd");
    if (cJSON_IsObject(command)) {
        command = cJSON_GetObjectItem(command, "value");
    }

    bool should_analyze = cJSON_IsTrue(command) ||
                          (cJSON_IsNumber(command) &&
                           command->valueint == 1) ||
                          (cJSON_IsString(command) &&
                           command->valuestring != NULL &&
                           (strcmp(command->valuestring, "1") == 0 ||
                            strcmp(command->valuestring, "true") == 0));

    if (!should_analyze) {
        publish_property_set_reply(request_id, 0, "success");
        cJSON_Delete(root);
        return;
    }

    if (xSemaphoreTake(ai_task_gate, 0) != pdTRUE) {
        ESP_LOGW(TAG, "AI analysis is already running");
        publish_property_set_reply(request_id, 0, "already running");
        cJSON_Delete(root);
        return;
    }

    publish_ai_state("分析中...", true);
    BaseType_t task_result = xTaskCreate(
        deepseek_task,
        "deepseek_task",
        12288,
        NULL,
        5,
        NULL
    );

    if (task_result != pdPASS) {
        xSemaphoreGive(ai_task_gate);
        publish_ai_state("无法创建 DeepSeek 分析任务", false);
        publish_property_set_reply(request_id, 500, "task create failed");
    } else {
        publish_property_set_reply(request_id, 0, "success");
    }

    cJSON_Delete(root);
}

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (wifi_retry_count < WIFI_MAXIMUM_RETRY) {
            wifi_retry_count++;
            ESP_LOGW(
                TAG,
                "Wi-Fi disconnected, retry %d/%d",
                wifi_retry_count,
                WIFI_MAXIMUM_RETRY
            );
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAILED_BIT);
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        wifi_retry_count = 0;
        ESP_LOGI(
            TAG,
            "Wi-Fi connected, IP: " IPSTR,
            IP2STR(&event->ip_info.ip)
        );
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_connect(void)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_config));

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &wifi_event_handler,
            NULL
        )
    );
    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &wifi_event_handler,
            NULL
        )
    );

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY
    );

    if ((bits & WIFI_CONNECTED_BIT) != 0) {
        return true;
    }

    ESP_LOGE(TAG, "Unable to connect to Wi-Fi");
    return false;
}

static void mqtt_event_handler(
    void *handler_args,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED: {
            mqtt_connected = true;
            ESP_LOGI(TAG, "Connected to OneNET");

            char post_reply_topic[208];
            char property_set_topic[208];
            snprintf(
                post_reply_topic,
                sizeof(post_reply_topic),
                "$sys/%s/%s/thing/property/post/reply",
                ONENET_PRODUCT_ID,
                ONENET_DEVICE_NAME
            );
            snprintf(
                property_set_topic,
                sizeof(property_set_topic),
                "$sys/%s/%s/thing/property/set",
                ONENET_PRODUCT_ID,
                ONENET_DEVICE_NAME
            );

            esp_mqtt_client_subscribe(
                mqtt_client,
                post_reply_topic,
                0
            );
            esp_mqtt_client_subscribe(
                mqtt_client,
                property_set_topic,
                0
            );
            publish_ai_state(latest_ai_reply, false);
            break;
        }

        case MQTT_EVENT_DISCONNECTED:
            mqtt_connected = false;
            ESP_LOGW(TAG, "Disconnected from OneNET");
            break;

        case MQTT_EVENT_DATA: {
            char property_set_topic[208];
            snprintf(
                property_set_topic,
                sizeof(property_set_topic),
                "$sys/%s/%s/thing/property/set",
                ONENET_PRODUCT_ID,
                ONENET_DEVICE_NAME
            );

            if (topic_equals(event, property_set_topic)) {
                ESP_LOGI(TAG, "OneNET property command received");
                handle_property_set(event->data, event->data_len);
            } else {
                ESP_LOGI(
                    TAG,
                    "OneNET message topic=%.*s data=%.*s",
                    event->topic_len,
                    event->topic,
                    event->data_len,
                    event->data
                );
            }
            break;
        }

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT event error");
            break;

        default:
            break;
    }
}

static void mqtt_start(void)
{
    const esp_mqtt_client_config_t mqtt_config = {
        .broker.address.uri = ONENET_MQTT_URI,
        .credentials.client_id = ONENET_DEVICE_NAME,
        .credentials.username = ONENET_PRODUCT_ID,
        .credentials.authentication.password = ONENET_DEVICE_TOKEN,
        .session.keepalive = 60,
        .session.disable_clean_session = false,
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_config);
    ESP_ERROR_CHECK(
        esp_mqtt_client_register_event(
            mqtt_client,
            ESP_EVENT_ANY_ID,
            mqtt_event_handler,
            NULL
        )
    );
    ESP_ERROR_CHECK(esp_mqtt_client_start(mqtt_client));
}

void app_main(void)
{
    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs_result);
    }

    esp_err_t hardware_result = feeder_hardware_init();
    if (hardware_result != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Hardware framework initialized with errors: %s",
            esp_err_to_name(hardware_result)
        );
    }
    hardware_result = feeder_hardware_start_test_task();
    if (hardware_result != ESP_OK &&
        hardware_result != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(
            TAG,
            "Hardware test task not started: %s",
            esp_err_to_name(hardware_result)
        );
    }

    ai_task_gate = xSemaphoreCreateBinary();
    if (ai_task_gate == NULL) {
        ESP_LOGE(TAG, "Unable to create AI task gate");
        return;
    }
    xSemaphoreGive(ai_task_gate);

    if (!wifi_connect()) {
        return;
    }

    mqtt_start();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
