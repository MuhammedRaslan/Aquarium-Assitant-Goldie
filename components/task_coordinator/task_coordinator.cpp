#include "task_coordinator.h"
#include "messages.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>

// STABILIZATION FIX: Include proper headers instead of manual extern declarations
#include "gemini_api.h"
#include "blynk_integration.h"
#include "wifi_config.h"  // For WIFI_SSID in diagnostic logs

static const char *TAG = "task_coordinator";

// STABILIZATION: Global state flags for fail-safe operation
static volatile bool wifi_initialized = false;
static volatile bool blynk_initialized = false;

// External function from dashboard.cpp (mood calculation)
extern "C" {
    mood_result_t calculate_mood_scores(aquarium_params_t params, uint32_t current_time);
}

// External dashboard calendar update
extern "C" {
    void dashboard_update_calendar(void);
}

// STEP 3: External access to frame buffers and state from dashboard.cpp
extern uint8_t *frame_buffer_a;
extern uint8_t *frame_buffer_b;
extern volatile bool buffer_a_ready;
extern volatile uint8_t buffer_a_frame_index;
extern volatile bool buffer_b_ready;
extern volatile uint8_t buffer_b_frame_index;

// External SPIFFS loading function from dashboard.cpp
extern "C" bool load_frame_from_spiffs(uint8_t frame_num, uint8_t *buffer);

// Time utility (duplicated from dashboard.cpp - no LVGL dependency)
static uint32_t get_current_time_seconds(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

/**
 * Background WiFi Init Task - STABILIZATION FIX
 * 
 * Initializes WiFi asynchronously on Core 1 to avoid blocking app_main.
 * Once WiFi connects, initializes Blynk and calendar, then deletes itself.
 * 
 * FAIL-SAFE: If WiFi fails, system continues in OFFLINE mode indefinitely.
 */
static void background_wifi_init_task(void *pvParameters)
{
    ESP_LOGI(TAG, "★═══════════════════════════════════════════════════════════★");
    ESP_LOGI(TAG, "★  Background WiFi Initialization Started (Core %d)        ★", xPortGetCoreID());
    ESP_LOGI(TAG, "★═══════════════════════════════════════════════════════════★");
    
    // Give UI time to start (1 second delay)
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    ESP_LOGI(TAG, "► Attempting WiFi connection to '%s'...", WIFI_SSID);
    bool wifi_ok = gemini_init_wifi();
    
    if (wifi_ok) {
        ESP_LOGI(TAG, "★═══════════════════════════════════════════════════════════★");
        ESP_LOGI(TAG, "★  ✓ WiFi CONNECTED Successfully!                         ★");
        ESP_LOGI(TAG, "★  Network: %s                                            ★", WIFI_SSID);
        ESP_LOGI(TAG, "★  Groq AI: READY (llama-3.3-70b-versatile)              ★");
        ESP_LOGI(TAG, "★═══════════════════════════════════════════════════════════★");
        wifi_initialized = true;
        
        // Update calendar with current time
        dashboard_update_calendar();
        
        // Initialize Blynk (graceful failure)
        if (blynk_init()) {
            ESP_LOGI(TAG, "✓ Blynk initialized - mobile dashboard active");
            blynk_initialized = true;
        } else {
            ESP_LOGW(TAG, "✗ Blynk init failed - mobile dashboard unavailable");
        }
        
        ESP_LOGI(TAG, "System now ONLINE - AI Assistant ready");
    } else {
        ESP_LOGE(TAG, "★═══════════════════════════════════════════════════════════★");
        ESP_LOGE(TAG, "★  ✗ WiFi CONNECTION FAILED!                              ★");
        ESP_LOGE(TAG, "★  Network: %s                                            ★", WIFI_SSID);
        ESP_LOGE(TAG, "★  System will remain in OFFLINE mode                     ★");
        ESP_LOGE(TAG, "★  Check: SSID, password, router settings                 ★");
        ESP_LOGE(TAG, "★═══════════════════════════════════════════════════════════★");
        wifi_initialized = false;
        blynk_initialized = false;
    }
    
    // Task complete - delete self
    ESP_LOGI(TAG, "Background WiFi init task completed (status: %s), deleting self", 
             wifi_initialized ? "SUCCESS" : "FAILED");
    vTaskDelete(NULL);
}

// Task handles
static TaskHandle_t logic_task_handle = NULL;
static TaskHandle_t storage_task_handle = NULL;
static TaskHandle_t wifi_task_handle = NULL;
static TaskHandle_t bg_wifi_init_handle = NULL;

// Queue handles (minimal placeholders)
QueueHandle_t queue_param_update = NULL;
QueueHandle_t queue_mood_result = NULL;
QueueHandle_t queue_anim_frame_request = NULL;
QueueHandle_t queue_anim_frame_ready = NULL;
QueueHandle_t queue_ai_request = NULL;
QueueHandle_t queue_ai_result = NULL;
QueueHandle_t queue_blynk_sync = NULL;

/**
 * Logic Task - STEP 2 (Mood Calculation)
 * 
 * Receives parameter updates, calculates mood, sends results.
 * Pure computation - no UI, no blocking I/O.
 */
static void logic_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Logic task started (mood calculation active)");
    
    aquarium_params_t params;
    mood_result_t result;
    
    while (1) {
        // Wait for parameter updates from LVGL task
        if (xQueueReceive(queue_param_update, &params, portMAX_DELAY) == pdTRUE) {
            
            uint32_t now = get_current_time_seconds();
            
            // Call pure function (DO NOT MODIFY - same logic as Step 1)
            result = calculate_mood_scores(params, now);
            
            // Send result back to LVGL task
            xQueueSend(queue_mood_result, &result, 0);
        }
    }
}

/**
 * Storage Task - REFACTORED FOR LOW-FREQUENCY STATIC FRAMES
 * 
 * ═══════════════════════════════════════════════════════════════════════════
 * CRITICAL DESIGN PRINCIPLE: COMPLETE I/O ISOLATION FROM LVGL
 * ═══════════════════════════════════════════════════════════════════════════
 * 
 * WHY THIS PATTERN PREVENTS LAG:
 * - All SPIFFS access happens ONLY in this task (Core 1)
 * - LVGL task (Core 0) NEVER waits for I/O - just swaps pointers
 * - No mutex locks needed - volatile flags provide lock-free sync
 * - Frame loading is infrequent (every 10s) so no CPU congestion
 * 
 * OPERATION:
 * 1. Wait for frame request from LVGL (blocking queue receive is OK here)
 * 2. Load frame from /spiffs/<mood>/frameX.bin into PSRAM buffer
 * 3. Set frame_ready flag to signal LVGL task
 * 4. NEVER call LVGL APIs (lv_* functions) from this task
 * 
 * BLOCKING I/O IS OK HERE - runs on Core 1, isolated from UI.
 */
static void storage_task(void *pvParameters)
{
    ESP_LOGI(TAG, "[STORAGE] ★ Storage task started on Core %d (SPIFFS handler)", xPortGetCoreID());
    
    anim_frame_request_msg_t request;
    uint32_t frame_count = 0;
    
    while (1) {
        // ═══════════════════════════════════════════════════════════════════
        // STEP 1: Wait for frame request (blocking is OK - this is Core 1)
        // ═══════════════════════════════════════════════════════════════════
        if (xQueueReceive(queue_anim_frame_request, &request, portMAX_DELAY) == pdTRUE) {
            
            uint8_t frame_index = request.frame_index;
            uint8_t category = frame_index / 8;
            uint8_t frame_in_cat = frame_index % 8;
            
            ESP_LOGI(TAG, "[STORAGE] Frame request #%lu: abs_frame=%d (cat=%d frame=%d)",
                     ++frame_count, frame_index, category, frame_in_cat);
            
            // Validate frame index (0-7 per emotion, 3 emotions = 0-23)
            if (frame_index >= 24) {
                ESP_LOGE(TAG, "[STORAGE] ✗ INVALID frame index %d (max 23)", frame_index);
                continue;
            }
            
            // ═══════════════════════════════════════════════════════════════
            // STEP 2: Choose buffer (double-buffering for safe pointer swap)
            // ═══════════════════════════════════════════════════════════════
            // Prefer buffer_a if both free, otherwise use whichever is available
            if (!buffer_a_ready) {
                ESP_LOGI(TAG, "[STORAGE] Loading frame %d into buffer_a...", frame_index);
                
                // BLOCKING SPIFFS READ - This is WHY we isolate from LVGL
                if (load_frame_from_spiffs(frame_index, frame_buffer_a)) {
                    buffer_a_frame_index = frame_index;
                    buffer_a_ready = true;  // Signal to LVGL: frame ready
                    ESP_LOGI(TAG, "[STORAGE] ✓ Frame %d → buffer_a READY", frame_index);
                } else {
                    ESP_LOGE(TAG, "[STORAGE] ✗ Failed to load frame %d (SPIFFS error)", frame_index);
                }
                
                // Yield after file I/O to prevent watchdog triggers
                taskYIELD();
                
            } else if (!buffer_b_ready) {
                ESP_LOGI(TAG, "[STORAGE] Loading frame %d into buffer_b...", frame_index);
                
                // BLOCKING SPIFFS READ
                if (load_frame_from_spiffs(frame_index, frame_buffer_b)) {
                    buffer_b_frame_index = frame_index;
                    buffer_b_ready = true;  // Signal to LVGL: frame ready
                    ESP_LOGI(TAG, "[STORAGE] ✓ Frame %d → buffer_b READY", frame_index);
                } else {
                    ESP_LOGE(TAG, "[STORAGE] ✗ Failed to load frame %d (SPIFFS error)", frame_index);
                }
                
                // Yield after file I/O
                taskYIELD();
                
            } else {
                // Both buffers occupied - LVGL hasn't consumed previous frame yet
                // This should be RARE with 10-second frame intervals
                ESP_LOGW(TAG, "[STORAGE] Both buffers busy, dropping frame %d request", frame_index);
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            
            // ═══════════════════════════════════════════════════════════════
            // NO QUEUE NOTIFICATION - LVGL polls buffer_ready flags directly
            // This is faster than queue messaging for simple flag checks
            // ═══════════════════════════════════════════════════════════════
        }
    }
}

/**
 * WiFi Task - STEP 4 + STEP 5 (AI Cloud Query + Blynk Sync)
 * 
 * Handles network I/O: AI API queries, Blynk sync.
 * Blocking network operations are OK here - runs on Core 1.
 * 
 * STABILIZATION FIX: Check WiFi/Blynk status before network calls.
 */
static void wifi_task(void *pvParameters)
{
    ESP_LOGI(TAG, "WiFi task started (AI + Blynk sync - waiting for network)");
    
    ai_request_msg_t ai_request;
    ai_result_msg_t ai_result;
    blynk_sync_msg_t blynk_sync;
    
    // Diagnostic: Log WiFi status periodically
    uint32_t status_counter = 0;
    
    while (1) {
        // Diagnostic: Every 2 seconds, log WiFi status (increased frequency to combat animation log flood)
        // Use actual wifi_connected status from gemini_api, not wifi_initialized
        extern bool gemini_is_wifi_connected(void);
        bool actually_connected = gemini_is_wifi_connected();
        
        if (++status_counter >= 2) {
            status_counter = 0;
            ESP_LOGW(TAG, "═══ WiFi Status: %s | Blynk: %s | Groq AI: %s ═══",
                     actually_connected ? "CONNECTED" : "OFFLINE",
                     blynk_initialized ? "ACTIVE" : "INACTIVE",
                     actually_connected ? "READY" : "UNAVAILABLE");
        }
        
        // Priority 1: Wait for AI request (blocking with timeout)
        if (xQueueReceive(queue_ai_request, &ai_request, pdMS_TO_TICKS(1000)) == pdTRUE) {
            
            // STABILIZATION FIX: Check if WiFi is ready (use same check as dashboard)
            if (!gemini_is_wifi_connected()) {
                ESP_LOGW(TAG, "AI request received but WiFi not ready - sending offline response");
                ai_result.success = false;
                snprintf(ai_result.advice, sizeof(ai_result.advice), 
                        "AI Assistant offline\\n\\nWiFi not connected.\\nCheck network settings.");
                xQueueOverwrite(queue_ai_result, &ai_result);
                continue;
            }
            
            ESP_LOGI(TAG, "AI request received - querying cloud API");
            
            // Call AI API (blocking network call - OK on Core 1)
            ai_result.success = gemini_query_aquarium(
                ai_request.ammonia_ppm,
                ai_request.nitrite_ppm,
                ai_request.nitrate_ppm,
                ai_request.hours_since_feed,
                ai_request.days_since_clean,
                ai_request.feeds_per_day,
                ai_request.water_change_interval,
                ai_result.advice,
                sizeof(ai_result.advice)
            );
            
            if (ai_result.success) {
                ESP_LOGI(TAG, "AI query successful - sending result");
            } else {
                ESP_LOGW(TAG, "AI query failed - sending error result");
            }
            
            // Send result back to LVGL task (non-blocking with overwrite)
            xQueueOverwrite(queue_ai_result, &ai_result);
        }
        
        // Priority 2: Check for Blynk sync request (non-blocking)
        if (xQueueReceive(queue_blynk_sync, &blynk_sync, 0) == pdTRUE) {
            
            // STABILIZATION FIX: Check if Blynk is ready
            if (!blynk_initialized) {
                ESP_LOGW(TAG, "Blynk sync requested but Blynk not initialized - skipping");
                continue;
            }
            
            ESP_LOGI(TAG, "Blynk sync received - sending to cloud (Mood=%s)", blynk_sync.mood);
            
            // Call Blynk API (blocking network call - OK on Core 1)
            // ~700ms total (7 HTTP calls × 100ms delay)
            blynk_send_all_data(
                blynk_sync.ammonia_ppm,
                blynk_sync.nitrite_ppm,
                blynk_sync.nitrate_ppm,
                blynk_sync.feed_hours,
                blynk_sync.clean_days,
                blynk_sync.mood,
                blynk_sync.ai_advice
            );
            
            ESP_LOGI(TAG, "Blynk sync complete");
        }
        
        // Yield to prevent starvation
        taskYIELD();
    }
}

void task_coordinator_init(void)
{
    ESP_LOGI(TAG, "Initializing task coordinator (Step 4 - wifi_task AI active)");
    
    // Create queues with correct sizes (updated for Step 4)
    queue_param_update = xQueueCreate(2, sizeof(aquarium_params_t));
    queue_mood_result = xQueueCreate(2, sizeof(mood_result_t));
    queue_anim_frame_request = xQueueCreate(1, sizeof(anim_frame_request_msg_t));
    queue_anim_frame_ready = xQueueCreate(1, sizeof(anim_frame_ready_msg_t));
    queue_ai_request = xQueueCreate(1, sizeof(ai_request_msg_t));
    queue_ai_result = xQueueCreate(1, sizeof(ai_result_msg_t));
    queue_blynk_sync = xQueueCreate(1, sizeof(blynk_sync_msg_t));
    
    if (!queue_param_update || !queue_mood_result || !queue_anim_frame_request ||
        !queue_anim_frame_ready || !queue_ai_request || !queue_ai_result || 
        !queue_blynk_sync) {
        ESP_LOGE(TAG, "Failed to create queues");
        return;
    }
    
    ESP_LOGI(TAG, "Queues created (7 total)");
    
    // Create tasks (pinned to Core 1)
    BaseType_t ret;
    
    ret = xTaskCreatePinnedToCore(
        logic_task,
        "logic_task",
        4096,           // Stack size
        NULL,           // Parameters
        5,              // Priority (medium)
        &logic_task_handle,
        1               // Core 1 (APP_CPU)
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create logic task");
        return;
    }
    
    ret = xTaskCreatePinnedToCore(
        storage_task,
        "storage_task",
        8192,           // Larger stack (will handle file I/O)
        NULL,
        4,              // Priority (lower than logic)
        &storage_task_handle,
        1               // Core 1
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create storage task");
        return;
    }
    
    ret = xTaskCreatePinnedToCore(
        wifi_task,
        "wifi_task",
        8192,           // Larger stack (will handle network I/O)
        NULL,
        3,              // Priority (lowest - network can wait)
        &wifi_task_handle,
        1               // Core 1
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create wifi task");
        return;
    }
    
    // STABILIZATION FIX: Create background WiFi init task
    // This task initializes WiFi asynchronously without blocking app_main
    ret = xTaskCreatePinnedToCore(
        background_wifi_init_task,
        "bg_wifi_init",
        8192,           // Larger stack (WiFi + Blynk init)
        NULL,
        2,              // Priority (low - runs in background)
        &bg_wifi_init_handle,
        1               // Core 1 (keep network off Core 0)
    );
    if (ret != pdPASS) {
        ESP_LOGW(TAG, "Failed to create background WiFi init task - system will stay offline");
        // Don't return - system can run without WiFi
    } else {
        ESP_LOGI(TAG, "Background WiFi init task created - network will start asynchronously");
    }
    
    ESP_LOGI(TAG, "Tasks created: logic (mood calc), storage (frame load), wifi (AI cloud)");
    ESP_LOGI(TAG, "Task coordinator init complete - System starting in OFFLINE mode");
}
