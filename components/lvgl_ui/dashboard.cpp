#include "dashboard.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

extern "C" {
    #include "gemini_api.h"
}

static const char *TAG = "dashboard";

// Image buffer for loading from SD card
#define FRAME_WIDTH 480
#define FRAME_HEIGHT 320
#define FRAME_SIZE (FRAME_WIDTH * FRAME_HEIGHT * 2)  // RGB565 = 2 bytes per pixel

static lv_img_dsc_t img_dsc = {
    .header = {
        .cf = LV_IMG_CF_TRUE_COLOR,
        .always_zero = 0,
        .reserved = 0,
        .w = FRAME_WIDTH,
        .h = FRAME_HEIGHT
    },
    .data_size = FRAME_SIZE,
    .data = NULL
};

// Note: If colors appear wrong, the BIN files might need byte swapping
// LVGL expects RGB565 in little-endian format

// Double buffer for smoother animation
static uint8_t *frame_buffer_a = NULL;
static uint8_t *frame_buffer_b = NULL;
static uint8_t current_buffer = 0;  // 0 = buffer_a, 1 = buffer_b

// UI Objects - Main Screen
static lv_obj_t *animation_img = NULL;
static lv_obj_t *gauge1 = NULL;
static lv_obj_t *gauge2 = NULL;
static lv_obj_t *gauge1_label = NULL;
static lv_obj_t *gauge2_label = NULL;

// UI Objects - Side Panel
static lv_obj_t *panel_content = NULL;
static lv_obj_t *panel_calendar = NULL;
static lv_obj_t *panel_day_label = NULL;
static lv_obj_t *panel_date_label = NULL;
static lv_obj_t *panel_month_label = NULL;
static lv_obj_t *panel_dropdown = NULL;
static lv_obj_t *panel_keyboard = NULL;
static lv_obj_t *panel_textarea = NULL;
static lv_obj_t *panel_modal = NULL;

// Button objects
static lv_obj_t *btn_feed = NULL;
static lv_obj_t *btn_water = NULL;
static lv_obj_t *btn_home = NULL;

// AI Assistant
static lv_obj_t *ai_text_label = NULL;

// Animation state
static lv_timer_t *anim_timer = NULL;
static uint8_t current_frame = 0;

// Animation categories
typedef enum {
    ANIM_CATEGORY_HAPPY = 0,  // Frames 1-8
    ANIM_CATEGORY_SAD = 1,    // Frames 9-16
    ANIM_CATEGORY_ANGRY = 2   // Frames 17-24
} anim_category_t;

static anim_category_t current_category = ANIM_CATEGORY_HAPPY;

// Panel state
static int current_dropdown_idx = 0;
static int current_param_idx = 0;

// 7-day logging state (circular buffer)
#define LOG_DAYS 7
static uint32_t feed_log[LOG_DAYS] = {0};  // Feed counts per day
static uint32_t water_log[LOG_DAYS] = {0}; // Water cleaning counts per day
static uint8_t current_day = 0;             // Current day index (0-6)

// Animation frame definitions
#define FRAMES_PER_CATEGORY 8
#define TOTAL_CATEGORIES 3
#define TOTAL_FRAMES 24  // 3 categories × 8 frames

// Set to 1 if colors appear wrong (swaps byte order)
#define SWAP_RGB565_BYTES 1  // Toggle if colors are wrong

// Function to load a frame from SPIFFS into specified buffer
static bool load_frame_from_spiffs(uint8_t frame_num, uint8_t *buffer) {
    char filepath[64];
    snprintf(filepath, sizeof(filepath), "/spiffs/frame%d.bin", frame_num + 1);
    
    FILE *f = fopen(filepath, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open %s", filepath);
        return false;
    }
    
    size_t bytes_read = fread(buffer, 1, FRAME_SIZE, f);
    fclose(f);
    
    if (bytes_read != FRAME_SIZE) {
        ESP_LOGE(TAG, "Frame %d: read %zu bytes, expected %d", frame_num + 1, bytes_read, FRAME_SIZE);
        return false;
    }
    
#if SWAP_RGB565_BYTES
    // Swap bytes if colors are wrong (RGB565 endianness)
    for (int i = 0; i < FRAME_SIZE; i += 2) {
        uint8_t temp = buffer[i];
        buffer[i] = buffer[i + 1];
        buffer[i + 1] = temp;
    }
#endif
    
    return true;
}

// Aquarium Parameter Values
static float temperature = 25.0f;        // Celsius (Gauge 1)
static float oxygen_level = 8.0f;        // mg/L (Gauge 2)
static float ph_level = 7.2f;            // pH 0-14
static uint32_t last_feed_time = 1;      // Timestamp of last feed (will be set to current time on init)
static uint32_t last_clean_time = 1;     // Timestamp of last tank cleaning (will be set to current time on init)

// Ideal ranges for mood calculation
#define TEMP_MIN_IDEAL 24.0f
#define TEMP_MAX_IDEAL 26.0f
#define TEMP_MIN_ACCEPTABLE 22.0f
#define TEMP_MAX_ACCEPTABLE 28.0f

#define OXYGEN_MIN_IDEAL 7.0f
#define OXYGEN_MAX_IDEAL 9.0f
#define OXYGEN_MIN_ACCEPTABLE 6.0f
#define OXYGEN_MAX_ACCEPTABLE 10.0f

#define PH_MIN_IDEAL 6.8f
#define PH_MAX_IDEAL 7.5f
#define PH_MIN_ACCEPTABLE 6.5f
#define PH_MAX_ACCEPTABLE 8.0f

#define FEED_INTERVAL_IDEAL 28800      // 8 hours in seconds
#define FEED_INTERVAL_MAX 43200        // 12 hours (getting hungry)
#define FEED_INTERVAL_CRITICAL 86400   // 24 hours (very hungry)

#define CLEAN_INTERVAL_IDEAL 604800    // 7 days in seconds
#define CLEAN_INTERVAL_MAX 1209600     // 14 days (needs cleaning)
#define CLEAN_INTERVAL_CRITICAL 1814400 // 21 days (dirty tank)

// Panel dial parameters
struct DialParam {
    const char* name;
    float min_val;
    float max_val;
    float current_val;
};

static DialParam dial_params[] = {
    {"Feed Amount", 0.0f, 100.0f, 50.0f},
    {"pH Calibration", 0.0f, 14.0f, 7.0f},
    {"Flow Rate", 0.0f, 1000.0f, 500.0f}
};

// Mood scoring structure
typedef struct {
    int temperature_score;  // -2 to +2
    int oxygen_score;       // -2 to +2
    int ph_score;           // -2 to +2
    int feed_score;         // -2 to +2
    int clean_score;        // -2 to +2
    int total_score;        // Sum of all scores
} mood_scores_t;

static mood_scores_t current_mood_scores = {0};

// Forward declarations
static void panel_button_event_cb(lv_event_t *e);
static void panel_dropdown_event_cb(lv_event_t *e);
static void keyboard_event_cb(lv_event_t *e);
static void open_numeric_input(int param_idx);
static void close_numeric_input(void);
static void update_panel_dial(float value, bool animate);
static void evaluate_and_update_mood(void);
static uint32_t get_current_time_seconds(void);

/**
 * @brief Animation timer callback - cycles through animation frames
 */
static void animation_timer_cb(lv_timer_t *timer)
{
    if (!animation_img) return;
    
    // Move to next frame
    current_frame = (current_frame + 1) % FRAMES_PER_CATEGORY;
    uint8_t abs_frame = (current_category * FRAMES_PER_CATEGORY) + current_frame;
    
    // Swap to the other buffer
    current_buffer = 1 - current_buffer;
    
    // Display the pre-loaded frame from the new buffer
    img_dsc.data = (current_buffer == 0) ? frame_buffer_a : frame_buffer_b;
    lv_img_set_src(animation_img, &img_dsc);
    
    // Pre-load the NEXT frame into the buffer we just finished displaying
    uint8_t next_frame = (current_frame + 1) % FRAMES_PER_CATEGORY;
    uint8_t next_abs_frame = (current_category * FRAMES_PER_CATEGORY) + next_frame;
    uint8_t *load_buffer = (current_buffer == 0) ? frame_buffer_b : frame_buffer_a;
    
    load_frame_from_spiffs(next_abs_frame, load_buffer);
}

/**
 * @brief Get current system time in seconds (from boot)
 */
static uint32_t get_current_time_seconds(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);  // Convert microseconds to seconds
}

/**
 * @brief Evaluate all parameters and determine mood
 * 
 * Mood Scoring System:
 * Each parameter contributes -2 to +2 points
 * Total Score Range: -10 to +10
 * 
 * Mood Assignment:
 * - HAPPY (0):  Total score >= 5  (Most parameters in ideal range)
 * - SAD (1):    Total score 0-4   (Some parameters acceptable, some concerning)
 * - ANGRY (2):  Total score < 0   (Multiple parameters out of range)
 */
static void evaluate_and_update_mood(void)
{
    uint32_t current_time = get_current_time_seconds();
    uint32_t time_since_feed = current_time - last_feed_time;
    uint32_t time_since_clean = current_time - last_clean_time;
    
    // Reset scores
    current_mood_scores.temperature_score = 0;
    current_mood_scores.oxygen_score = 0;
    current_mood_scores.ph_score = 0;
    current_mood_scores.feed_score = 0;
    current_mood_scores.clean_score = 0;
    
    // 1. Temperature Scoring
    if (temperature >= TEMP_MIN_IDEAL && temperature <= TEMP_MAX_IDEAL) {
        current_mood_scores.temperature_score = 2;  // Perfect
    } else if (temperature >= TEMP_MIN_ACCEPTABLE && temperature <= TEMP_MAX_ACCEPTABLE) {
        current_mood_scores.temperature_score = 1;  // Acceptable
    } else if (temperature < TEMP_MIN_ACCEPTABLE - 2.0f || temperature > TEMP_MAX_ACCEPTABLE + 2.0f) {
        current_mood_scores.temperature_score = -2; // Critical
    } else {
        current_mood_scores.temperature_score = -1; // Concerning
    }
    
    // 2. Oxygen Level Scoring
    if (oxygen_level >= OXYGEN_MIN_IDEAL && oxygen_level <= OXYGEN_MAX_IDEAL) {
        current_mood_scores.oxygen_score = 2;  // Perfect
    } else if (oxygen_level >= OXYGEN_MIN_ACCEPTABLE && oxygen_level <= OXYGEN_MAX_ACCEPTABLE) {
        current_mood_scores.oxygen_score = 1;  // Acceptable
    } else if (oxygen_level < OXYGEN_MIN_ACCEPTABLE - 1.0f || oxygen_level > OXYGEN_MAX_ACCEPTABLE + 1.0f) {
        current_mood_scores.oxygen_score = -2; // Critical
    } else {
        current_mood_scores.oxygen_score = -1; // Concerning
    }
    
    // 3. pH Level Scoring
    if (ph_level >= PH_MIN_IDEAL && ph_level <= PH_MAX_IDEAL) {
        current_mood_scores.ph_score = 2;  // Perfect
    } else if (ph_level >= PH_MIN_ACCEPTABLE && ph_level <= PH_MAX_ACCEPTABLE) {
        current_mood_scores.ph_score = 1;  // Acceptable
    } else if (ph_level < PH_MIN_ACCEPTABLE - 0.5f || ph_level > PH_MAX_ACCEPTABLE + 0.5f) {
        current_mood_scores.ph_score = -2; // Critical
    } else {
        current_mood_scores.ph_score = -1; // Concerning
    }
    
    // 4. Feed Timing Scoring
    if (time_since_feed <= FEED_INTERVAL_IDEAL) {
        current_mood_scores.feed_score = 2;  // Well fed
    } else if (time_since_feed <= FEED_INTERVAL_MAX) {
        current_mood_scores.feed_score = 1;  // Getting hungry
    } else if (time_since_feed >= FEED_INTERVAL_CRITICAL) {
        current_mood_scores.feed_score = -2; // Starving
    } else {
        current_mood_scores.feed_score = -1; // Very hungry
    }
    
    // 5. Tank Cleanliness Scoring
    if (time_since_clean <= CLEAN_INTERVAL_IDEAL) {
        current_mood_scores.clean_score = 2;  // Clean tank
    } else if (time_since_clean <= CLEAN_INTERVAL_MAX) {
        current_mood_scores.clean_score = 1;  // Needs cleaning soon
    } else if (time_since_clean >= CLEAN_INTERVAL_CRITICAL) {
        current_mood_scores.clean_score = -2; // Very dirty
    } else {
        current_mood_scores.clean_score = -1; // Dirty
    }
    
    // Calculate total score
    current_mood_scores.total_score = current_mood_scores.temperature_score +
                                      current_mood_scores.oxygen_score +
                                      current_mood_scores.ph_score +
                                      current_mood_scores.feed_score +
                                      current_mood_scores.clean_score;
    
    // Determine mood category based on total score
    anim_category_t new_category;
    if (current_mood_scores.total_score >= 5) {
        new_category = ANIM_CATEGORY_HAPPY;   // 5 to 10: Happy
    } else if (current_mood_scores.total_score >= 0) {
        new_category = ANIM_CATEGORY_SAD;     // 0 to 4: Sad
    } else {
        new_category = ANIM_CATEGORY_ANGRY;   // -10 to -1: Angry
    }
    
    // Update mood if changed
    if (new_category != current_category) {
        ESP_LOGI(TAG, "Mood changed: %s -> %s (Score: %d)",
                 current_category == ANIM_CATEGORY_HAPPY ? "HAPPY" : (current_category == ANIM_CATEGORY_SAD ? "SAD" : "ANGRY"),
                 new_category == ANIM_CATEGORY_HAPPY ? "HAPPY" : (new_category == ANIM_CATEGORY_SAD ? "SAD" : "ANGRY"),
                 current_mood_scores.total_score);
        dashboard_set_animation_category(new_category);
    }
    
    // Log detailed mood analysis
    ESP_LOGI(TAG, "Mood Scores: Temp=%d, O2=%d, pH=%d, Feed=%d, Clean=%d | Total=%d",
             current_mood_scores.temperature_score,
             current_mood_scores.oxygen_score,
             current_mood_scores.ph_score,
             current_mood_scores.feed_score,
             current_mood_scores.clean_score,
             current_mood_scores.total_score);
}

/**
 * @brief AI Assistant - Query Gemini API for advice
 */
static void update_ai_assistant(void)
{
    if (!ai_text_label) return;
    
    static char advice[512];
    uint32_t current_time = get_current_time_seconds();
    uint32_t time_since_feed = current_time - last_feed_time;
    uint32_t time_since_clean = current_time - last_clean_time;
    
    // Convert to hours/days for API
    float hours_since_feed = time_since_feed / 3600.0f;
    float days_since_clean = time_since_clean / 86400.0f;
    
    // Show loading message
    lv_label_set_text(ai_text_label, LV_SYMBOL_REFRESH " Consulting AI...");
    lv_task_handler();  // Force UI update
    
    // Query Gemini API
    bool success = gemini_query_aquarium(
        temperature, 
        oxygen_level, 
        ph_level,
        hours_since_feed,
        days_since_clean,
        advice,
        sizeof(advice)
    );
    
    if (success) {
        lv_label_set_text(ai_text_label, advice);
    } else {
        // Fallback to local analysis if API fails
        lv_label_set_text(ai_text_label, 
            LV_SYMBOL_WARNING " AI offline.\n"
            "Using local analysis:\n"
            "Check WiFi connection.");
    }
}

/**
 * @brief Gesture event handler for swipe detection
 */
/**
 * @brief Panel button event handler
 */
static void panel_button_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *btn = lv_event_get_target(e);
    
    if (code == LV_EVENT_CLICKED) {
        if (btn == btn_feed) {
            // Log feed event with current feed amount parameter
            feed_log[current_day]++;
            last_feed_time = get_current_time_seconds();  // Update last feed time
            ESP_LOGI(TAG, "Feed logged - Day %d: %lu feeds (Amount: %.1f)", current_day, feed_log[current_day], dial_params[0].current_val);
            
            // Re-evaluate mood after feeding
            evaluate_and_update_mood();
            update_ai_assistant();
            
        } else if (btn == btn_water) {
            // Log water cleaning event
            water_log[current_day]++;
            last_clean_time = get_current_time_seconds();  // Update last clean time
            ESP_LOGI(TAG, "Water cleaned - Day %d: %lu cleanings", current_day, water_log[current_day]);
            
            // Re-evaluate mood after cleaning
            evaluate_and_update_mood();
            update_ai_assistant();
            
        } else if (btn == btn_home) {
            // Scroll back to top (animation section)
            lv_obj_t *scroll_cont = lv_obj_get_parent(btn_home);
            lv_obj_scroll_to_y(scroll_cont, 0, LV_ANIM_ON);
            ESP_LOGI(TAG, "Scrolling back to home (top)");
        }
    }
}

/**
 * @brief Dropdown event handler
 */
static void panel_dropdown_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *dd = lv_event_get_target(e);
        current_dropdown_idx = lv_dropdown_get_selected(dd);
        
        ESP_LOGI(TAG, "Dropdown selected: %s (Current value: %.2f)", 
                 dial_params[current_dropdown_idx].name, 
                 dial_params[current_dropdown_idx].current_val);
        
        // Update meter to show selected parameter's current value
        update_panel_dial(dial_params[current_dropdown_idx].current_val, true);
        
        // Open numeric input for this parameter
        open_numeric_input(current_dropdown_idx);
    }
}

/**
 * @brief Keyboard event handler
 */
static void keyboard_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *kb = lv_event_get_target(e);
    
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        if (code == LV_EVENT_READY && panel_textarea) {
            // Get entered value
            const char *txt = lv_textarea_get_text(panel_textarea);
            float value = atof(txt);
            
            // Validate range
            DialParam &param = dial_params[current_dropdown_idx];
            if (value < param.min_val) value = param.min_val;
            if (value > param.max_val) value = param.max_val;
            
            // Update parameter and animate dial
            param.current_val = value;
            update_panel_dial(value, true);
            
            ESP_LOGI(TAG, "Parameter '%s' set to %.2f", param.name, value);
        }
        
        close_numeric_input();
    }
}

/**
 * @brief Update panel dial value with optional animation
 */
static void update_panel_dial(float value, bool animate)
{
    // Since we removed the meter, just log the value update
    DialParam &param = dial_params[current_dropdown_idx];
    ESP_LOGI(TAG, "Parameter '%s' updated to %.2f", param.name, value);
}

/**
 * @brief Open numeric input modal
 */
static void open_numeric_input(int param_idx)
{
    if (panel_modal) return;  // Already open
    
    DialParam &param = dial_params[param_idx];
    
    // Create modal background - positioned in panel section
    panel_modal = lv_obj_create(panel_content);
    lv_obj_set_size(panel_modal, 480, 320);  // Full landscape panel size
    lv_obj_set_pos(panel_modal, 0, 0);
    lv_obj_set_style_bg_color(panel_modal, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel_modal, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel_modal, 0, LV_PART_MAIN);
    lv_obj_clear_flag(panel_modal, LV_OBJ_FLAG_SCROLLABLE);
    
    // Create input container (no rotation needed - panel_content is already rotated)
    lv_obj_t *input_cont = lv_obj_create(panel_modal);
    lv_obj_set_size(input_cont, 280, 200);
    lv_obj_center(input_cont);
    
    // Title label
    lv_obj_t *title = lv_label_create(input_cont);
    lv_label_set_text_fmt(title, "Enter %s", param.name);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    
    // Text area for input
    panel_textarea = lv_textarea_create(input_cont);
    lv_obj_set_size(panel_textarea, 200, 40);
    lv_obj_align(panel_textarea, LV_ALIGN_TOP_MID, 0, 40);
    lv_textarea_set_one_line(panel_textarea, true);
    lv_textarea_set_text(panel_textarea, "");  // Start empty for fresh input
    lv_textarea_set_placeholder_text(panel_textarea, "Enter value");
    lv_textarea_set_cursor_pos(panel_textarea, LV_TEXTAREA_CURSOR_LAST);
    
    // Numeric keyboard
    panel_keyboard = lv_keyboard_create(input_cont);
    lv_obj_set_size(panel_keyboard, 260, 120);
    lv_obj_align(panel_keyboard, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_keyboard_set_mode(panel_keyboard, LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_textarea(panel_keyboard, panel_textarea);
    
    lv_obj_add_event_cb(panel_keyboard, keyboard_event_cb, LV_EVENT_ALL, NULL);
    
    lv_obj_move_foreground(panel_modal);
}

/**
 * @brief Close numeric input modal
 */
static void close_numeric_input(void)
{
    if (panel_modal) {
        lv_obj_del(panel_modal);
        panel_modal = NULL;
        panel_keyboard = NULL;
        panel_textarea = NULL;
    }
}

/**
 * @brief Create a gauge widget at specified position
 */
static lv_obj_t* create_gauge(lv_obj_t *parent, lv_align_t align, lv_coord_t x_ofs, lv_coord_t y_ofs)
{
    // Create a container to hold the rotated gauge (larger to accommodate rotation)
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_set_size(container, 170, 170);  // Larger container for rotated gauge
    lv_obj_align(container, align, x_ofs, y_ofs);
    lv_obj_set_style_bg_opa(container, LV_OPA_0, LV_PART_MAIN);
    lv_obj_set_style_border_width(container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(container, 0, LV_PART_MAIN);
    
    // Create meter (gauge) widget inside the container
    lv_obj_t *meter = lv_meter_create(container);
    lv_obj_set_size(meter, 120, 120);
    lv_obj_center(meter);  // Center the gauge in the container
    
    // Remove the default background
    lv_obj_set_style_bg_opa(meter, LV_OPA_0, LV_PART_MAIN);
    lv_obj_set_style_border_width(meter, 0, LV_PART_MAIN);
    
    // Add a scale without labels or major tick marks
    lv_meter_scale_t *scale = lv_meter_add_scale(meter);
    lv_meter_set_scale_ticks(meter, scale, 41, 1, 5, lv_palette_main(LV_PALETTE_GREY));
    lv_meter_set_scale_major_ticks(meter, scale, 0, 0, 0, lv_color_black(), 0);  // All 0 = no major ticks or labels
    // Rotate scale 90° clockwise: start at 225° instead of 135°
    lv_meter_set_scale_range(meter, scale, 0, 100, 270, 135);
    
    // Add a three arc indicator with different colors
    lv_meter_indicator_t *indic1 = lv_meter_add_arc(meter, scale, 8, lv_palette_main(LV_PALETTE_RED), 0);
    lv_meter_set_indicator_start_value(meter, indic1, 0);
    lv_meter_set_indicator_end_value(meter, indic1, 33);
    
    lv_meter_indicator_t *indic2 = lv_meter_add_arc(meter, scale, 8, lv_palette_main(LV_PALETTE_YELLOW), 0);
    lv_meter_set_indicator_start_value(meter, indic2, 33);
    lv_meter_set_indicator_end_value(meter, indic2, 66);
    
    lv_meter_indicator_t *indic3 = lv_meter_add_arc(meter, scale, 8, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_meter_set_indicator_start_value(meter, indic3, 66);
    lv_meter_set_indicator_end_value(meter, indic3, 100);
    
    // Add needle indicator
    lv_meter_indicator_t *indic_needle = lv_meter_add_needle_line(meter, scale, 4, lv_palette_main(LV_PALETTE_BLUE), -10);
    lv_meter_set_indicator_value(meter, indic_needle, 0);
    
    // Store the needle indicator in the meter's user data for later updates
    lv_obj_set_user_data(meter, indic_needle);
    
    return container;  // Return the container, not the meter
}

/**
 * @brief Create side panel with rotated landscape content
 */
/**
 * @brief Create a label for gauge value display
 */
static lv_obj_t* create_gauge_label(lv_obj_t *parent, lv_obj_t *gauge_container)
{
    // Get the actual meter widget (first child of the container)
    lv_obj_t *meter = lv_obj_get_child(gauge_container, 0);
    lv_obj_t *label = lv_label_create(meter);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(label, "0");
    lv_obj_center(label);
    
    return label;
}

/**
 * @brief Initialize the dashboard UI
 */
void dashboard_init(void)
{
    ESP_LOGI(TAG, "Initializing IoT Dashboard");
    
    // Initialize timestamps to current time
    uint32_t current_time = get_current_time_seconds();
    last_feed_time = current_time;
    last_clean_time = current_time;
    
    // Allocate double buffers in PSRAM for smooth animation
    frame_buffer_a = (uint8_t *)heap_caps_malloc(FRAME_SIZE, MALLOC_CAP_SPIRAM);
    frame_buffer_b = (uint8_t *)heap_caps_malloc(FRAME_SIZE, MALLOC_CAP_SPIRAM);
    
    if (frame_buffer_a == NULL || frame_buffer_b == NULL) {
        ESP_LOGE(TAG, "Failed to allocate frame buffers in PSRAM!");
        return;
    }
    ESP_LOGI(TAG, "Allocated %d bytes × 2 in PSRAM for double buffering", FRAME_SIZE);
    
    // Test SPIFFS access
    ESP_LOGI(TAG, "Testing SPIFFS access...");
    FILE *test_file = fopen("/spiffs/frame1.bin", "rb");
    if (test_file == NULL) {
        ESP_LOGE(TAG, "SPIFFS ERROR: Cannot access /spiffs/frame1.bin");
        ESP_LOGE(TAG, "Please ensure frame BIN files are in spiffs_image/ and rebuild");
    } else {
        fclose(test_file);
        ESP_LOGI(TAG, "SPIFFS accessible - frame files found!");
    }
    
    // Perform initial mood evaluation
    evaluate_and_update_mood();
    
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);
    
    // Create a scrollable container that's taller than the screen (landscape: 480×800 total)
    // Layout: Animation+Gauges (0-320px) + AI Assistant (320-470px) + Panel (470-800px)
    lv_obj_t *scroll_container = lv_obj_create(scr);
    lv_obj_set_size(scroll_container, 480, 800);  // 480 wide, 800 tall (2.5× screen height)
    lv_obj_set_pos(scroll_container, 0, 0);
    lv_obj_set_style_bg_color(scroll_container, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_border_width(scroll_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scroll_container, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(scroll_container, LV_DIR_VER);  // Vertical scrolling only
    
    // ===== ANIMATION + GAUGES SECTION (0-320px) =====
    
    // Create animation image widget
    animation_img = lv_img_create(scroll_container);
    lv_obj_set_pos(animation_img, 0, 0);
    
    // Pre-load first two frames
    if (load_frame_from_spiffs(0, frame_buffer_a) && load_frame_from_spiffs(1, frame_buffer_b)) {
        img_dsc.data = frame_buffer_a;
        current_buffer = 0;
        lv_img_set_src(animation_img, &img_dsc);
        ESP_LOGI(TAG, "Initial frames loaded and ready");
    } else {
        ESP_LOGE(TAG, "Failed to pre-load initial frames!");
    }
    
    // Create Gauge 1 (Bottom Left corner of animation area)
    gauge1 = create_gauge(scroll_container, LV_ALIGN_TOP_LEFT, -30, 180);
    gauge1_label = create_gauge_label(scroll_container, gauge1);
    
    // Create Gauge 2 (Bottom Right corner of animation area, moved up)
    gauge2 = create_gauge(scroll_container, LV_ALIGN_TOP_RIGHT, 30, 180);
    gauge2_label = create_gauge_label(scroll_container, gauge2);
    
    // Move gauges to front (on top of animation)
    lv_obj_move_foreground(gauge1);
    lv_obj_move_foreground(gauge2);
    
    // ===== AI ASSISTANT SECTION (320-470px) =====
    
    // Create AI assistant background
    lv_obj_t *ai_bg = lv_obj_create(scroll_container);
    lv_obj_set_size(ai_bg, 480, 150);
    lv_obj_set_pos(ai_bg, 0, 320);
    lv_obj_set_style_bg_color(ai_bg, lv_color_hex(0x1a1a3a), LV_PART_MAIN);
    lv_obj_set_style_border_width(ai_bg, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(ai_bg, lv_palette_main(LV_PALETTE_CYAN), LV_PART_MAIN);
    lv_obj_clear_flag(ai_bg, LV_OBJ_FLAG_SCROLLABLE);
    
    // AI Assistant title
    lv_obj_t *ai_title = lv_label_create(ai_bg);
    lv_label_set_text(ai_title, LV_SYMBOL_WIFI " AI Assistant");
    lv_obj_set_style_text_font(ai_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(ai_title, lv_palette_main(LV_PALETTE_CYAN), 0);
    lv_obj_set_pos(ai_title, 10, 10);
    
    // AI advice/status text area
    ai_text_label = lv_label_create(ai_bg);
    lv_label_set_text(ai_text_label, "System initializing...\nAnalyzing aquarium parameters...");
    lv_obj_set_size(ai_text_label, 460, 90);
    lv_obj_set_pos(ai_text_label, 10, 40);
    lv_obj_set_style_text_color(ai_text_label, lv_color_white(), 0);
    lv_label_set_long_mode(ai_text_label, LV_LABEL_LONG_WRAP);
    
    // ===== PANEL SECTION (470-800px) =====
    
    // Create panel background
    lv_obj_t *panel_bg = lv_obj_create(scroll_container);
    lv_obj_set_size(panel_bg, 480, 320);
    lv_obj_set_pos(panel_bg, 0, 470);  // 320 + 150 = 470
    lv_obj_set_style_bg_color(panel_bg, lv_color_hex(0x1a1a1a), LV_PART_MAIN);
    lv_obj_set_style_border_width(panel_bg, 0, LV_PART_MAIN);
    lv_obj_clear_flag(panel_bg, LV_OBJ_FLAG_SCROLLABLE);
    
    // Create panel container for landscape content
    panel_content = lv_obj_create(panel_bg);
    lv_obj_set_size(panel_content, 440, 280);
    lv_obj_set_pos(panel_content, 0, 0);  // Top-left corner
    lv_obj_set_style_bg_color(panel_content, lv_color_hex(0x2a2a2a), LV_PART_MAIN);
    lv_obj_set_style_border_width(panel_content, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel_content, lv_palette_main(LV_PALETTE_BLUE), LV_PART_MAIN);
    lv_obj_clear_flag(panel_content, LV_OBJ_FLAG_SCROLLABLE);
    
    // Layout buttons horizontally in landscape - centered in 420px width
    int btn_width = 120, btn_height = 45, btn_spacing = 10;
    int total_btn_width = 3 * btn_width + 2 * btn_spacing;  // 390px
    int btn_x_start = (420 - total_btn_width) / 2;  // Center horizontally
    int btn_y = 15;
    
    btn_feed = lv_btn_create(panel_content);
    lv_obj_set_size(btn_feed, btn_width, btn_height);
    lv_obj_set_pos(btn_feed, btn_x_start, btn_y);
    lv_obj_t *label_feed = lv_label_create(btn_feed);
    lv_label_set_text(label_feed, "Feed Logger");
    lv_obj_center(label_feed);
    lv_obj_add_event_cb(btn_feed, panel_button_event_cb, LV_EVENT_CLICKED, NULL);
    
    btn_water = lv_btn_create(panel_content);
    lv_obj_set_size(btn_water, btn_width, btn_height);
    lv_obj_set_pos(btn_water, btn_x_start + (btn_width + btn_spacing) * 1, btn_y);
    lv_obj_t *label_water = lv_label_create(btn_water);
    lv_label_set_text(label_water, "Water Cleaned");
    lv_obj_center(label_water);
    lv_obj_add_event_cb(btn_water, panel_button_event_cb, LV_EVENT_CLICKED, NULL);
    
    btn_home = lv_btn_create(panel_content);
    lv_obj_set_size(btn_home, btn_width, btn_height);
    lv_obj_set_pos(btn_home, btn_x_start + (btn_width + btn_spacing) * 2, btn_y);
    lv_obj_t *label_home = lv_label_create(btn_home);
    lv_label_set_text(label_home, "Home");
    lv_obj_center(label_home);
    lv_obj_add_event_cb(btn_home, panel_button_event_cb, LV_EVENT_CLICKED, NULL);
    
    // Create calendar widget below buttons on the left
    int calendar_y = btn_y + btn_height + 20;
    
    // Create calendar container
    panel_calendar = lv_obj_create(panel_content);
    lv_obj_set_size(panel_calendar, 150, 130);
    lv_obj_set_pos(panel_calendar, 20, calendar_y);
    lv_obj_set_style_bg_color(panel_calendar, lv_color_hex(0x1a1a1a), LV_PART_MAIN);
    lv_obj_set_style_border_width(panel_calendar, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel_calendar, lv_palette_main(LV_PALETTE_BLUE), LV_PART_MAIN);
    lv_obj_set_style_radius(panel_calendar, 10, LV_PART_MAIN);
    lv_obj_clear_flag(panel_calendar, LV_OBJ_FLAG_SCROLLABLE);
    
    // Day name label (e.g., "Monday")
    panel_day_label = lv_label_create(panel_calendar);
    lv_obj_set_style_text_font(panel_day_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(panel_day_label, lv_palette_main(LV_PALETTE_BLUE), LV_PART_MAIN);
    lv_label_set_text(panel_day_label, "---");
    lv_obj_align(panel_day_label, LV_ALIGN_TOP_MID, 0, 10);
    
    // Date number label (e.g., "23")
    panel_date_label = lv_label_create(panel_calendar);
    lv_obj_set_style_text_font(panel_date_label, &lv_font_montserrat_32, LV_PART_MAIN);
    lv_obj_set_style_text_color(panel_date_label, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(panel_date_label, "--");
    lv_obj_align(panel_date_label, LV_ALIGN_CENTER, 0, 5);
    
    // Month/Year label (e.g., "Dec 2025")
    panel_month_label = lv_label_create(panel_calendar);
    lv_obj_set_style_text_font(panel_month_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(panel_month_label, lv_palette_main(LV_PALETTE_GREY), LV_PART_MAIN);
    lv_label_set_text(panel_month_label, "--- ----");
    lv_obj_align(panel_month_label, LV_ALIGN_BOTTOM_MID, 0, -10);
    
    // Create dropdown to the right of calendar, vertically centered
    panel_dropdown = lv_dropdown_create(panel_content);
    lv_obj_set_size(panel_dropdown, 220, 35);
    lv_obj_set_pos(panel_dropdown, 190, calendar_y + 50);
    lv_dropdown_set_options(panel_dropdown, "Feed Amount\npH Calibration\nFlow Rate");
    lv_obj_add_event_cb(panel_dropdown, panel_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    
    ESP_LOGI(TAG, "Scrollable dashboard with animation and panel created successfully");
    
    // Initialize feed and clean timestamps to "just now" so mood is HAPPY
    uint32_t now = get_current_time_seconds();
    last_feed_time = now;
    last_clean_time = now;
    
    // Initialize sensor values to ideal ranges (Happy mood)
    dashboard_update_sensor1(25.0f);  // Temperature: 25°C (ideal)
    dashboard_update_sensor2(8.0f);   // Oxygen: 8.0 mg/L (ideal)
    
    // Start animation timer (12 FPS for smooth animation)
    anim_timer = lv_timer_create(animation_timer_cb, 83, NULL);
    
    // Initialize AI assistant
    update_ai_assistant();
    
    ESP_LOGI(TAG, "Dashboard initialized successfully");
    ESP_LOGI(TAG, "Animation enabled - cycling through 8 C array frames at 12 FPS");
    ESP_LOGI(TAG, "Swipe right from left edge to open side panel");
}

/**
 * @brief Open the side panel with slide animation
 */
/**
 * @brief Update sensor 1 gauge value (Temperature in Celsius)
 */
void dashboard_update_sensor1(float value)
{
    // Temperature range: 0-100°C for display (actual range monitored separately)
    if (value < 0.0f) value = 0.0f;
    if (value > 100.0f) value = 100.0f;
    
    temperature = value;  // Store actual temperature
    
    if (gauge1 && gauge1_label) {
        // Get the actual meter widget (first child of the container)
        lv_obj_t *meter = lv_obj_get_child(gauge1, 0);
        // Update needle
        lv_meter_indicator_t *indic = (lv_meter_indicator_t*)lv_obj_get_user_data(meter);
        if (indic) {
            lv_meter_set_indicator_value(meter, indic, (int32_t)value);
        }
        
        // Update center label
        lv_label_set_text_fmt(gauge1_label, "%d", (int)value);
    }
    
    // Re-evaluate mood when temperature changes
    evaluate_and_update_mood();
    update_ai_assistant();
}

/**
 * @brief Update sensor 2 gauge value (Oxygen level in mg/L)
 */
void dashboard_update_sensor2(float value)
{
    // Oxygen range: 0-100 mg/L for display (actual range monitored separately)
    if (value < 0.0f) value = 0.0f;
    if (value > 100.0f) value = 100.0f;
    
    oxygen_level = value;  // Store actual oxygen level
    
    if (gauge2 && gauge2_label) {
        // Get the actual meter widget (first child of the container)
        lv_obj_t *meter = lv_obj_get_child(gauge2, 0);
        // Update needle
        lv_meter_indicator_t *indic = (lv_meter_indicator_t*)lv_obj_get_user_data(meter);
        if (indic) {
            lv_meter_set_indicator_value(meter, indic, (int32_t)value);
        }
        
        // Update center label
        lv_label_set_text_fmt(gauge2_label, "%d", (int)value);
    }
    
    // Re-evaluate mood when oxygen changes
    evaluate_and_update_mood();
    update_ai_assistant();
}

/**
 * @brief Update pH level
 * @param value pH value (0-14)
 */
void dashboard_update_ph(float value)
{
    if (value < 0.0f) value = 0.0f;
    if (value > 14.0f) value = 14.0f;
    
    ph_level = value;
    dial_params[1].current_val = value;  // Update pH calibration dial
    
    ESP_LOGI(TAG, "pH updated: %.2f", ph_level);
    
    // Re-evaluate mood when pH changes
    evaluate_and_update_mood();
    update_ai_assistant();
}

/**
 * @brief Get feed log for a specific day
 */
uint32_t dashboard_get_feed_log(uint8_t day)
{
    if (day >= LOG_DAYS) return 0;
    return feed_log[day];
}

/**
 * @brief Get water cleaning log for a specific day
 */
uint32_t dashboard_get_water_log(uint8_t day)
{
    if (day >= LOG_DAYS) return 0;
    return water_log[day];
}

/**
 * @brief Print all logs to console
 */
void dashboard_print_logs(void)
{
    ESP_LOGI(TAG, "===== DASHBOARD LOGS =====");
    ESP_LOGI(TAG, "Current Day Index: %d", current_day);
    ESP_LOGI(TAG, "");
    
    ESP_LOGI(TAG, "Feed Logs (last 7 days):");
    for (int i = 0; i < LOG_DAYS; i++) {
        ESP_LOGI(TAG, "  Day %d: %lu feeds", i, feed_log[i]);
    }
    ESP_LOGI(TAG, "");
    
    ESP_LOGI(TAG, "Water Cleaning Logs (last 7 days):");
    for (int i = 0; i < LOG_DAYS; i++) {
        ESP_LOGI(TAG, "  Day %d: %lu cleanings", i, water_log[i]);
    }
    ESP_LOGI(TAG, "");
    
    ESP_LOGI(TAG, "Current Parameter Values:");
    for (int i = 0; i < 3; i++) {
        ESP_LOGI(TAG, "  %s: %.2f (range: %.1f - %.1f)", 
                 dial_params[i].name, 
                 dial_params[i].current_val,
                 dial_params[i].min_val,
                 dial_params[i].max_val);
    }
    ESP_LOGI(TAG, "==========================");
}

/**
 * @brief Set animation category (Happy, Sad, or Angry)
 * @param category Category to switch to (0=Happy, 1=Sad, 2=Angry)
 */
void dashboard_set_animation_category(uint8_t category)
{
    if (category >= TOTAL_CATEGORIES) {
        ESP_LOGW(TAG, "Invalid category %d, must be 0-2", category);
        return;
    }
    
    current_category = (anim_category_t)category;
    current_frame = 0;  // Reset to first frame of new category
    
    // Immediately load and display first two frames of new category
    uint8_t abs_frame = (current_category * FRAMES_PER_CATEGORY);
    if (load_frame_from_spiffs(abs_frame, frame_buffer_a) && 
        load_frame_from_spiffs(abs_frame + 1, frame_buffer_b)) {
        img_dsc.data = frame_buffer_a;
        current_buffer = 0;
        lv_img_set_src(animation_img, &img_dsc);
        ESP_LOGI(TAG, "Switched to %s animation (frames %d-%d)", 
                 category == 0 ? "HAPPY" : (category == 1 ? "SAD" : "ANGRY"),
                 abs_frame + 1, abs_frame + FRAMES_PER_CATEGORY);
    }
}

/**
 * @brief Update calendar with current date/time from system
 */
void dashboard_update_calendar(void)
{
    if (!panel_day_label || !panel_date_label || !panel_month_label) {
        return;  // Calendar not initialized yet
    }
    
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    
    // Day names
    const char *days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    
    // Update day name
    lv_label_set_text(panel_day_label, days[timeinfo.tm_wday]);
    
    // Update date number
    char date_str[8];
    snprintf(date_str, sizeof(date_str), "%d", timeinfo.tm_mday);
    lv_label_set_text(panel_date_label, date_str);
    
    // Update month and year
    char month_str[16];
    snprintf(month_str, sizeof(month_str), "%s %d", months[timeinfo.tm_mon], 1900 + timeinfo.tm_year);
    lv_label_set_text(panel_month_label, month_str);
}

/**
 * @brief Get current animation category
 * @return Current category (0=Happy, 1=Sad, 2=Angry)
 */
uint8_t dashboard_get_animation_category(void)
{
    return (uint8_t)current_category;
}

