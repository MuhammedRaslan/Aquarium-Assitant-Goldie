#include "gemini_api.h"
#include "wifi_config.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <string.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "gemini_api";
static bool wifi_connected = false;
static esp_netif_t *sta_netif = NULL;

// WiFi event handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi STA started, connecting...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "WiFi connected to AP, waiting for IP...");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected = false;
        ESP_LOGI(TAG, "WiFi disconnected, retrying...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_connected = true;
    }
}

bool gemini_init_wifi(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize network interface
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();
    
    // Set DHCP hostname before connecting
    ESP_ERROR_CHECK(esp_netif_set_hostname(sta_netif, "ESP32_Aquarium"));

    // Initialize WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    // Configure WiFi
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi initialization finished. Connecting to %s...", WIFI_SSID);

    // Wait for connection (timeout after 60 seconds)
    int retry = 0;
    while (!wifi_connected && retry < 600) {  // Wait up to 60 seconds for IP
        if (retry > 0 && retry % 50 == 0) {  // Log every 5 seconds
            ESP_LOGI(TAG, "Waiting for IP... (%d seconds)", retry / 10);
        }
        
        // Try restarting DHCP client at 15 seconds if still no IP
        if (retry == 150) {
            ESP_LOGW(TAG, "No IP after 15s, restarting DHCP client...");
            esp_netif_dhcpc_stop(sta_netif);
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_netif_dhcpc_start(sta_netif);
        }
        
        vTaskDelay(pdMS_TO_TICKS(100));
        retry++;
    }

    if (wifi_connected) {
        ESP_LOGI(TAG, "WiFi connection successful!");
        
        // Initialize SNTP for time synchronization
        ESP_LOGI(TAG, "Initializing SNTP for time sync...");
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_init();
        
        // Set timezone (adjust for your location - this is UTC+0)
        // For other timezones: "EST5EDT" (US East), "PST8PDT" (US West), "CET-1CEST" (Europe), etc.
        setenv("TZ", "UTC-0", 1);
        tzset();
        
        ESP_LOGI(TAG, "Waiting for time sync from NTP server...");
        // Wait up to 10 seconds for time to be set
        time_t now = 0;
        struct tm timeinfo = { 0 };
        int retry_time = 0;
        while (timeinfo.tm_year < (2024 - 1900) && ++retry_time < 100) {
            vTaskDelay(pdMS_TO_TICKS(100));
            time(&now);
            localtime_r(&now, &timeinfo);
        }
        
        if (timeinfo.tm_year >= (2024 - 1900)) {
            char strftime_buf[64];
            strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
            ESP_LOGI(TAG, "Time synchronized: %s", strftime_buf);
        } else {
            ESP_LOGW(TAG, "Time sync timeout - calendar will show incorrect date");
        }
    } else {
        ESP_LOGE(TAG, "WiFi connection timeout - no IP received after %d seconds", retry / 10);
    }

    return wifi_connected;
}

// HTTP response buffer
static char http_response[4096];
static int response_len = 0;
static bool quota_exhausted = false;  // Track if API quota is exhausted
static uint32_t quota_reset_time = 0; // Timestamp when quota might reset (seconds)

// HTTP event handler
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (response_len + evt->data_len < sizeof(http_response)) {
                memcpy(http_response + response_len, evt->data, evt->data_len);
                response_len += evt->data_len;
                http_response[response_len] = '\0';
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

bool gemini_query_aquarium(float temperature, float oxygen, float ph, 
                          float hours_since_feed, float days_since_clean,
                          char *response_buffer, size_t buffer_size)
{
    if (!wifi_connected) {
        ESP_LOGE(TAG, "WiFi not connected");
        return false;
    }

    // Check if quota is exhausted - don't waste network bandwidth
    if (quota_exhausted) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000);
        if (now < quota_reset_time) {
            // Still in cooldown period
            if (response_buffer && buffer_size > 0) {
                snprintf(response_buffer, buffer_size, 
                        "API quota exhausted. Resets in %lu seconds.", 
                        quota_reset_time - now);
            }
            return false;
        } else {
            // Cooldown expired, try again
            ESP_LOGI(TAG, "Quota cooldown expired, retrying API...");
            quota_exhausted = false;
        }
    }

    // Build the prompt
    char prompt[512];
    snprintf(prompt, sizeof(prompt),
        "You are an aquarium expert assistant. Analyze these parameters and provide brief advice (max 100 words):\n"
        "Temperature: %.1f°C (ideal: 24-26°C)\n"
        "Oxygen: %.1f mg/L (ideal: 7-9 mg/L)\n"
        "pH: %.1f (ideal: 6.8-7.5)\n"
        "Hours since feeding: %.1f (feed every 8 hours)\n"
        "Days since cleaning: %.1f (clean weekly)\n"
        "Provide actionable advice if anything is wrong, or confirm if all is well.",
        temperature, oxygen, ph, hours_since_feed, days_since_clean);

    // Build JSON request body
    cJSON *root = cJSON_CreateObject();
    cJSON *contents = cJSON_CreateArray();
    cJSON *content = cJSON_CreateObject();
    cJSON *parts = cJSON_CreateArray();
    cJSON *part = cJSON_CreateObject();
    
    cJSON_AddStringToObject(part, "text", prompt);
    cJSON_AddItemToArray(parts, part);
    cJSON_AddItemToObject(content, "parts", parts);
    cJSON_AddItemToArray(contents, content);
    cJSON_AddItemToObject(root, "contents", contents);
    
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    // Configure HTTP client
    response_len = 0;
    memset(http_response, 0, sizeof(http_response));

    esp_http_client_config_t config = {};
    config.url = GEMINI_API_URL;
    config.method = HTTP_METHOD_POST;
    config.event_handler = http_event_handler;
    config.timeout_ms = 10000;
    config.crt_bundle_attach = esp_crt_bundle_attach;  // Use ESP-IDF certificate bundle for HTTPS

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_str, strlen(json_str));

    // Perform request
    esp_err_t err = esp_http_client_perform(client);
    free(json_str);

    bool success = false;
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP Status = %d, Response length = %d", status, response_len);
        
        // Log error responses for debugging
        if (status != 200 && response_len > 0) {
            ESP_LOGE(TAG, "API Error Response: %s", http_response);
            
            // Handle 429 (quota exhausted) - stop retrying for 1 hour
            if (status == 429) {
                quota_exhausted = true;
                quota_reset_time = (uint32_t)(esp_timer_get_time() / 1000000) + 3600; // 1 hour cooldown
                ESP_LOGW(TAG, "API quota exhausted! Disabling API calls for 1 hour to save bandwidth.");
                if (response_buffer && buffer_size > 0) {
                    snprintf(response_buffer, buffer_size, 
                            "API quota exhausted. Will retry in 1 hour.");
                }
            }
        }
        
        if (status == 200 && response_len > 0) {
            // Success! Reset quota flag if it was set
            if (quota_exhausted) {
                ESP_LOGI(TAG, "API quota restored!");
                quota_exhausted = false;
            }
            
            // Parse JSON response
            cJSON *resp_json = cJSON_Parse(http_response);
            if (resp_json) {
                cJSON *candidates = cJSON_GetObjectItem(resp_json, "candidates");
                if (candidates && cJSON_GetArraySize(candidates) > 0) {
                    cJSON *candidate = cJSON_GetArrayItem(candidates, 0);
                    cJSON *content = cJSON_GetObjectItem(candidate, "content");
                    cJSON *parts = cJSON_GetObjectItem(content, "parts");
                    if (parts && cJSON_GetArraySize(parts) > 0) {
                        cJSON *part = cJSON_GetArrayItem(parts, 0);
                        cJSON *text = cJSON_GetObjectItem(part, "text");
                        if (text && text->valuestring) {
                            strncpy(response_buffer, text->valuestring, buffer_size - 1);
                            response_buffer[buffer_size - 1] = '\0';
                            success = true;
                            ESP_LOGI(TAG, "AI Response: %s", response_buffer);
                        }
                    }
                }
                cJSON_Delete(resp_json);
            }
        }
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return success;
}
