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
        wifi_event_sta_disconnected_t* disconnected = (wifi_event_sta_disconnected_t*) event_data;
        ESP_LOGW(TAG, "WiFi disconnected (reason: %d), retrying immediately...", disconnected->reason);
        
        // Immediate retry for hotspot compatibility (hotspots can be unstable during initial connection)
        vTaskDelay(pdMS_TO_TICKS(500));  // Short delay to avoid storm
        
        // Attempt reconnection
        esp_err_t ret = esp_wifi_connect();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "WiFi reconnect failed: %s", esp_err_to_name(ret));
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_connected = true;
    }
}

bool gemini_init_wifi(void)
{
    // STABILIZATION FIX: All errors are handled gracefully
    // NO ESP_ERROR_CHECK - system must never reboot due to WiFi failure
    
    // Initialize NVS (graceful failure)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase, attempting recovery...");
        ret = nvs_flash_erase();
        if (ret == ESP_OK) {
            ret = nvs_flash_init();
        }
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed (%s) - continuing without persistent WiFi config", esp_err_to_name(ret));
        // Continue - WiFi can work without NVS
    }

    // Initialize network interface (graceful failure)
    ret = esp_netif_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "netif init failed (%s) - WiFi unavailable", esp_err_to_name(ret));
        return false;
    }
    
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {  // ESP_ERR_INVALID_STATE means already created
        ESP_LOGE(TAG, "event loop create failed (%s) - WiFi unavailable", esp_err_to_name(ret));
        return false;
    }
    
    sta_netif = esp_netif_create_default_wifi_sta();
    if (sta_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create WiFi STA interface - WiFi unavailable");
        return false;
    }
    
    // Set DHCP hostname (optional - don't fail if this doesn't work)
    ret = esp_netif_set_hostname(sta_netif, "ESP32_Aquarium");
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set hostname (%s)", esp_err_to_name(ret));
        // Continue anyway
    }

    // Initialize WiFi (graceful failure)
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed (%s) - WiFi unavailable", esp_err_to_name(ret));
        return false;
    }

    // Register event handlers (graceful failure)
    ret = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi event handler register failed (%s)", esp_err_to_name(ret));
        return false;
    }
    
    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "IP event handler register failed (%s)", esp_err_to_name(ret));
        return false;
    }

    // Configure WiFi for mobile hotspot compatibility
    wifi_config_t wifi_config = {};
    strcpy((char*)wifi_config.sta.ssid, WIFI_SSID);
    strcpy((char*)wifi_config.sta.password, WIFI_PASS);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;  // Accept WPA or WPA2 (phone hotspot compatible)
    wifi_config.sta.pmf_cfg.capable = true;                        // PMF capable but not required
    wifi_config.sta.pmf_cfg.required = false;                      // Don't require PMF (some phones don't support)
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;           // Scan all channels (more reliable for hotspots)
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;       // Connect to strongest signal
    wifi_config.sta.listen_interval = 3;                           // Listen interval for beacon frames

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi set mode failed (%s)", esp_err_to_name(ret));
        return false;
    }
    
    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi set config failed (%s)", esp_err_to_name(ret));
        return false;
    }
    
    // Disable power save for better connection stability
    ret = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to disable power save (%s)", esp_err_to_name(ret));
    }
    
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi start failed (%s)", esp_err_to_name(ret));
        return false;
    }
    
    // Configure WiFi for stable hotspot connection
    esp_wifi_set_inactive_time(WIFI_IF_STA, 10);  // Keep-alive every 10 seconds
    esp_wifi_set_max_tx_power(78);                 // Slightly reduce power (78 = 19.5dBm, helps with some phones)
    esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);  // All protocols
    ESP_LOGI(TAG, "WiFi configured for mobile hotspot (keep-alive 10s, all protocols enabled)");

    ESP_LOGI(TAG, "WiFi initialization finished. Connecting to %s...", WIFI_SSID);

    // STABILIZATION FIX: Add yielding to prevent watchdog triggers
    // Wait for connection (timeout after 30 seconds to avoid long boot delays)
    int retry = 0;
    while (!wifi_connected && retry < 300) {  // Wait up to 30 seconds for IP
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
        
        // CRITICAL: Yield to prevent watchdog trigger
        vTaskDelay(pdMS_TO_TICKS(100));
        taskYIELD();  // Explicitly yield to IDLE task
        retry++;
    }

    if (wifi_connected) {
        ESP_LOGI(TAG, "WiFi connection successful!");
        
        // Initialize SNTP for time synchronization
        ESP_LOGI(TAG, "Initializing SNTP for time sync...");
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        // Try multiple NTP servers for better reliability with mobile hotspots
        esp_sntp_setservername(0, "time.google.com");  // Google's NTP (often less blocked)
        esp_sntp_setservername(1, "pool.ntp.org");     // Public NTP pool
        esp_sntp_setservername(2, "time.nist.gov");    // NIST (US government)
        esp_sntp_init();
        
        // Set timezone (adjust for your location - this is UTC+0)
        // For other timezones: "EST5EDT" (US East), "PST8PDT" (US West), "CET-1CEST" (Europe), etc.
        setenv("TZ", "UTC-0", 1);
        tzset();
        
        ESP_LOGI(TAG, "Waiting for time sync from NTP server...");
        // Wait up to 10 seconds for time sync (hotspots can be slow)
        time_t now = 0;
        struct tm timeinfo = {};
        int retry_time = 0;
        while (timeinfo.tm_year < (2024 - 1900) && ++retry_time < 100) {  // 100 * 100ms = 10 seconds
            vTaskDelay(pdMS_TO_TICKS(100));
            taskYIELD();  // Explicitly yield to IDLE task
            time(&now);
            localtime_r(&now, &timeinfo);
        }
        
        if (timeinfo.tm_year >= (2024 - 1900)) {
            char strftime_buf[64];
            strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
            ESP_LOGI(TAG, "Time synchronized: %s", strftime_buf);
        } else {
            ESP_LOGW(TAG, "NTP sync timeout (hotspot may block UDP/123) - date will be incorrect");
            ESP_LOGW(TAG, "Time will sync eventually if NTP becomes available");
        }
    } else {
        ESP_LOGE(TAG, "WiFi connection timeout - no IP received after %d seconds", retry / 10);
    }

    return wifi_connected;
}

bool gemini_is_wifi_connected(void)
{
    return wifi_connected;
}

uint32_t gemini_get_current_time(void)
{
    time_t now;
    time(&now);
    return (uint32_t)now;
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

bool gemini_query_aquarium(float ammonia_ppm, float nitrite_ppm, float nitrate_ppm, 
                          float hours_since_feed, float days_since_clean,
                          int feeds_per_day, int water_change_interval,
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

    // Build the prompt with Goldie personality - focusing on nitrogen cycle
    char prompt[768];
    snprintf(prompt, sizeof(prompt),
        "You are Goldie, a friendly and caring goldfish who lives in this aquarium! 🐠\n"
        "Respond in first-person as Goldie with a cheerful, bubbly personality (max 80 words).\n\n"
        "Current water quality (Nitrogen Cycle):\n"
        "⚠️ Ammonia (NH3): %.2f ppm (MUST be 0!)\n"
        "⚠️ Nitrite (NO2): %.2f ppm (MUST be 0!)\n"
        "📊 Nitrate (NO3): %.0f ppm (safe <20, warning 20-40)\n\n"
        "Feeding schedule:\n"
        "🍽️ Scheduled feeds: %d times per day\n"
        "⏰ Last fed: %.1f hours ago\n\n"
        "Water maintenance:\n"
        "💧 Water change interval: every %d days\n"
        "🧽 Last cleaned: %.1f days ago\n\n"
        "As Goldie, comment on how you're feeling in these conditions and give friendly advice!",
        ammonia_ppm, nitrite_ppm, nitrate_ppm, feeds_per_day, hours_since_feed, 
        water_change_interval, days_since_clean);

    // Build JSON request body for Groq (OpenAI-compatible format)
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", "llama-3.3-70b-versatile");
    
    cJSON *messages = cJSON_CreateArray();
    cJSON *message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "role", "user");
    cJSON_AddStringToObject(message, "content", prompt);
    cJSON_AddItemToArray(messages, message);
    cJSON_AddItemToObject(root, "messages", messages);
    
    cJSON_AddNumberToObject(root, "max_tokens", 150);
    cJSON_AddNumberToObject(root, "temperature", 0.7);
    
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    // Configure HTTP client
    response_len = 0;
    memset(http_response, 0, sizeof(http_response));

    esp_http_client_config_t config = {};
    config.url = GROQ_API_URL;
    config.method = HTTP_METHOD_POST;
    config.event_handler = http_event_handler;
    config.timeout_ms = 10000;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    
    // Groq uses Bearer token authentication
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", GROQ_API_KEY);
    esp_http_client_set_header(client, "Authorization", auth_header);
    
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
            
            // Parse JSON response (OpenAI format)
            cJSON *resp_json = cJSON_Parse(http_response);
            if (resp_json) {
                cJSON *choices = cJSON_GetObjectItem(resp_json, "choices");
                if (choices && cJSON_GetArraySize(choices) > 0) {
                    cJSON *choice = cJSON_GetArrayItem(choices, 0);
                    cJSON *message = cJSON_GetObjectItem(choice, "message");
                    cJSON *content = cJSON_GetObjectItem(message, "content");
                    if (content && content->valuestring) {
                        strncpy(response_buffer, content->valuestring, buffer_size - 1);
                        response_buffer[buffer_size - 1] = '\0';
                        success = true;
                        ESP_LOGI(TAG, "AI Response: %s", response_buffer);
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
