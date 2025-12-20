#include "blynk_integration.h"
#include "blynk_config.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_http_client.h"

static const char *TAG = "blynk";
static bool blynk_initialized = false;

// HTTP event handler for Blynk responses
static esp_err_t blynk_http_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "Blynk response: %.*s", evt->data_len, (char*)evt->data);
            break;
        default:
            break;
    }
    return ESP_OK;
}

// Send data to a Blynk virtual pin
static bool blynk_write_pin(int pin, const char *value)
{
    if (!blynk_initialized) {
        ESP_LOGW(TAG, "Blynk not initialized");
        return false;
    }

    // Build URL: http://blynk.cloud/external/api/update?token=XXX&V0=value
    char url[512];
    snprintf(url, sizeof(url), 
             "http://%s/external/api/update?token=%s&V%d=%s",
             BLYNK_SERVER, BLYNK_AUTH_TOKEN, pin, value);

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.event_handler = blynk_http_event_handler;
    config.timeout_ms = 5000;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return false;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK && status_code == 200) {
        ESP_LOGD(TAG, "Pin V%d updated to: %s", pin, value);
        return true;
    } else {
        ESP_LOGW(TAG, "Failed to update pin V%d (status: %d)", pin, status_code);
        return false;
    }
}

bool blynk_init(void)
{
    ESP_LOGI(TAG, "Initializing Blynk integration");
    ESP_LOGI(TAG, "Template: %s", BLYNK_TEMPLATE_ID);
    ESP_LOGI(TAG, "Server: %s", BLYNK_SERVER);
    
    blynk_initialized = true;
    
    // Send initial "connected" message
    blynk_update_mood("HAPPY");
    
    ESP_LOGI(TAG, "Blynk initialized successfully");
    return true;
}

void blynk_update_temperature(float value)
{
    char str[32];
    snprintf(str, sizeof(str), "%.1f", value);
    blynk_write_pin(BLYNK_PIN_TEMPERATURE, str);
}

void blynk_update_oxygen(float value)
{
    char str[32];
    snprintf(str, sizeof(str), "%.1f", value);
    blynk_write_pin(BLYNK_PIN_OXYGEN, str);
}

void blynk_update_ph(float value)
{
    char str[32];
    snprintf(str, sizeof(str), "%.2f", value);
    blynk_write_pin(BLYNK_PIN_PH, str);
}

void blynk_update_feeding(float hours)
{
    char str[32];
    snprintf(str, sizeof(str), "%.1f", hours);
    blynk_write_pin(BLYNK_PIN_FEEDING, str);
}

void blynk_update_cleaning(float days)
{
    char str[32];
    snprintf(str, sizeof(str), "%.1f", days);
    blynk_write_pin(BLYNK_PIN_CLEANING, str);
}

void blynk_update_mood(const char *mood)
{
    blynk_write_pin(BLYNK_PIN_MOOD, mood);
}

void blynk_update_ai_advice(const char *advice)
{
    // URL encode the advice text (simple version - replace spaces with %20)
    char encoded[512];
    int j = 0;
    for (int i = 0; advice[i] != '\0' && j < sizeof(encoded) - 4; i++) {
        if (advice[i] == ' ') {
            encoded[j++] = '%';
            encoded[j++] = '2';
            encoded[j++] = '0';
        } else if (advice[i] == '\n') {
            encoded[j++] = '%';
            encoded[j++] = '0';
            encoded[j++] = 'A';
        } else {
            encoded[j++] = advice[i];
        }
    }
    encoded[j] = '\0';
    
    blynk_write_pin(BLYNK_PIN_AI_ADVICE, encoded);
}

void blynk_send_all_data(float temp, float oxygen, float ph, 
                         float feed_hours, float clean_days,
                         const char *mood, const char *ai_advice)
{
    if (!blynk_initialized) {
        ESP_LOGW(TAG, "Blynk not initialized");
        return;
    }

    ESP_LOGI(TAG, "Sending all data to Blynk");
    
    blynk_update_temperature(temp);
    vTaskDelay(pdMS_TO_TICKS(100));  // Small delay between requests
    
    blynk_update_oxygen(oxygen);
    vTaskDelay(pdMS_TO_TICKS(100));
    
    blynk_update_ph(ph);
    vTaskDelay(pdMS_TO_TICKS(100));
    
    blynk_update_feeding(feed_hours);
    vTaskDelay(pdMS_TO_TICKS(100));
    
    blynk_update_cleaning(clean_days);
    vTaskDelay(pdMS_TO_TICKS(100));
    
    blynk_update_mood(mood);
    vTaskDelay(pdMS_TO_TICKS(100));
    
    if (ai_advice != NULL && ai_advice[0] != '\0') {
        blynk_update_ai_advice(ai_advice);
    }
    
    ESP_LOGI(TAG, "All data sent to Blynk");
}
