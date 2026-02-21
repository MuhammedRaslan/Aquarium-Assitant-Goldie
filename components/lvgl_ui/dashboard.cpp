#include "dashboard.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "messages.h"
#include "task_coordinator.h"
#include "gemini_api.h"
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>

static const char *TAG = "dashboard";

// Image buffer for loading from SD card
#define FRAME_WIDTH 480
#define FRAME_HEIGHT 320
#define FRAME_SIZE (FRAME_WIDTH * FRAME_HEIGHT * 2)  // RGB565 = 2 bytes per pixel

// CRITICAL FIX: Dual image descriptors for LVGL redraw triggering
// LVGL only redraws when the source pointer changes, not just data contents
// We alternate between these two descriptors to force redraws
static lv_img_dsc_t anim_dsc_a = {
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

static lv_img_dsc_t anim_dsc_b = {
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

// Track which descriptor is currently active (alternates on each frame)
static lv_img_dsc_t *active_dsc = &anim_dsc_a;

// Note: If colors appear wrong, the BIN files might need byte swapping
// LVGL expects RGB565 in little-endian format

// ═══════════════════════════════════════════════════════════════════════════
// FRAME BUFFER STATE - SHARED BETWEEN STORAGE TASK (WRITER) AND LVGL (READER)
// ═══════════════════════════════════════════════════════════════════════════
// 
// WHY VOLATILE: Storage task (Core 1) writes, LVGL task (Core 0) reads.
// Volatile ensures compiler doesn't cache these values across cores.
// 
// WHY NO MUTEX: Lock-free design using atomic flag pattern.
// - storage_task sets frame_ready = true after loading
// - LVGL task reads frame_ready, consumes buffer, sets frame_ready = false
// - Only one writer, one reader = no race condition
// 
// Double buffer for PSRAM frame storage
uint8_t *frame_buffer_a = NULL;  // PSRAM buffer A (307,200 bytes)
uint8_t *frame_buffer_b = NULL;  // PSRAM buffer B (307,200 bytes)

// Buffer state (volatile for cross-core visibility)
volatile bool buffer_a_ready = false;          // true = frame loaded, ready to display
volatile uint8_t buffer_a_frame_index = 0;     // Which frame is in buffer A (0-23)
volatile bool buffer_b_ready = false;          // true = frame loaded, ready to display
volatile uint8_t buffer_b_frame_index = 0;     // Which frame is in buffer B (0-23)

// Current display state (ONLY modified by LVGL task)
static uint8_t current_frame = 0;              // Current frame being displayed (0-7)
static uint8_t current_category = 0;           // Current mood category (0=HAPPY, 1=SAD, 2=ANGRY)
static uint32_t last_frame_update_time = 0;    // Timestamp of last frame change (seconds)

// UI Objects - Main Screen
static lv_obj_t *animation_img = NULL;
static lv_obj_t *btn_feed_main = NULL;   // Feed button on animation screen
static lv_obj_t *btn_water_main = NULL;  // Water button on animation screen
static lv_obj_t *date_label = NULL;      // Date display on animation screen
static lv_obj_t *date_shadow = NULL;     // Black shadow behind date for outline effect

// UI Objects - Side Panel
static lv_obj_t *panel_content = NULL;
static lv_obj_t *panel_calendar = NULL;
static lv_obj_t *panel_day_label = NULL;
static lv_obj_t *panel_date_label = NULL;
static lv_obj_t *panel_month_label = NULL;
static lv_obj_t *panel_keyboard = NULL;
static lv_obj_t *panel_textarea = NULL;
static lv_obj_t *panel_modal = NULL;

// Weekly calendar day boxes (for updating dots)
static lv_obj_t *week_day_boxes[7] = {NULL};

// New calendar page buttons
static lv_obj_t *btn_param_log = NULL;
static lv_obj_t *btn_water_log = NULL;
static lv_obj_t *btn_feed_log = NULL;

// Pop-up modals
static lv_obj_t *popup_param = NULL;
static lv_obj_t *popup_water = NULL;
static lv_obj_t *popup_feed = NULL;
static lv_obj_t *popup_history = NULL;
static lv_obj_t *popup_keypad = NULL;
static lv_obj_t *popup_monthly_cal = NULL;

// Monthly calendar state
static int monthly_cal_display_month = 0;  // 0 = current month
static int monthly_cal_display_year = 0;

// Active input tracking
static lv_obj_t *active_input_field = NULL;

// Scroll container
static lv_obj_t *scroll_container = NULL;

// Button objects
static lv_obj_t *btn_home = NULL;

// AI Assistant
static lv_obj_t *ai_text_label = NULL;

// Mood indicator
static lv_obj_t *mood_face = NULL;

// ═════════════════════════════════════════════════════════════════════════════
// ANIMATION STATE - STATIC FRAME SYSTEM (ONE FRAME PER 10 SECONDS)
// ═════════════════════════════════════════════════════════════════════════════
// No continuous animation timer - frames update at low frequency
// This eliminates CPU congestion that causes LVGL scrolling lag
static lv_timer_t *static_frame_timer = NULL;  // Checks every 10s for frame update

// Panel state
static int current_dropdown_idx = 0;

// AI assistant state
static bool ai_initial_request_sent = false;  // Track if we've triggered AI after WiFi connects
static uint32_t last_ai_update = 0;          // Timestamp of last successful AI response (for rate limiting)

// 7-day logging state (circular buffer)
#define LOG_DAYS 7

// Parameter log structure
typedef struct {
    time_t timestamp;
    float ammonia;
    float nitrate;
    float nitrite;
    float high_ph;
    float low_ph;
} param_log_t;

// Water change log structure
typedef struct {
    time_t timestamp;
    uint8_t interval_days;  // How often water should be changed
} water_change_log_t;

// Feed log structure
typedef struct {
    time_t timestamp;
    uint8_t feeds_per_day;
} feed_log_t;

static param_log_t param_log[LOG_DAYS] = {};
static water_change_log_t water_change_log[LOG_DAYS] = {};
static feed_log_t feed_log_data[LOG_DAYS] = {};
static uint32_t feed_log[LOG_DAYS] = {0};  // Feed button click counts per day (legacy)
static uint32_t water_log[LOG_DAYS] = {0}; // Water button click counts per day (legacy)
static uint8_t current_day = 0;             // Current day index (0-6)

// Current feed schedule settings
static uint8_t current_feeds_per_day = 2;  // Default: 2 feeds (morning + evening)
static uint8_t current_water_interval_days = 7;  // Default: weekly water changes

// Planned schedule storage
#define MAX_FEED_TIMES 6
typedef struct {
    uint8_t hour;
    uint8_t minute;
    bool enabled;
} feed_time_t;

static feed_time_t planned_feed_times[MAX_FEED_TIMES] = {
    {8, 0, true},   // 8:00 AM
    {14, 0, true},  // 2:00 PM
    {20, 0, true},  // 8:00 PM
    {0, 0, false},  // Disabled
    {0, 0, false},  // Disabled
    {0, 0, false}   // Disabled
};
// static uint8_t planned_water_change_interval = 7;  // Days between water changes (OLD - replaced with seconds at line 209)

// ═════════════════════════════════════════════════════════════════════════════
// MEDICATION CALCULATOR
// ═════════════════════════════════════════════════════════════════════════════

// Medication types
typedef enum {
    MED_ICH_TREATMENT = 0,
    MED_FUNGAL_TREATMENT,
    MED_ANTIBIOTICS,
    MED_ANTI_PARASITIC,
    MED_WATER_CONDITIONER,
    MED_TYPE_COUNT
} medication_type_t;

// Medication dosage data structure
typedef struct {
    const char *name;
    float dosage_per_gallon_ml;  // ml per gallon
    float dosage_per_liter_ml;   // ml per liter
    const char *instructions;
} medication_data_t;

// Pre-configured medication dosages (standard industry values)
static const medication_data_t medication_database[MED_TYPE_COUNT] = {
    {"Ich Treatment", 5.0, 1.32, "Treat daily for 3 days, then 25% water change"},
    {"Fungal Treatment", 2.5, 0.66, "Treat every other day for 1 week"},
    {"Antibiotics", 250.0, 66.0, "Dose every 24h for 5 days, remove carbon filter"},
    {"Anti-parasitic", 1.0, 0.26, "Single dose, repeat after 48 hours if needed"},
    {"Water Conditioner", 2.0, 0.53, "Use during water changes"}
};

// Calculator state - Universal dosage calculator
typedef struct {
    float product_amount;     // Amount of product (e.g., 5 ml)
    float per_volume;         // Per X gallons/litres (e.g., 10)
    float tank_size;          // Tank size
    bool is_gallons;          // true = gallons, false = liters (for "Per" field)
    bool tank_is_gallons;     // true = gallons, false = liters (for "Tank Size" field)
    int unit_type;            // 0=ml, 1=tsp, 2=tbsp, 3=drops, 4=fl oz, 5=cups, 6=g
    float calculated_dosage;
    char result_text[256];
} med_calculator_state_t;

static med_calculator_state_t med_calc_state = {
    .product_amount = 5.0,
    .per_volume = 10.0,
    .tank_size = 0.0,
    .is_gallons = false,
    .tank_is_gallons = false,
    .unit_type = 0,  // Default to ml
    .calculated_dosage = 0.0,
    .result_text = {0}
};

// Calculator UI objects
static lv_obj_t *btn_med_calc = NULL;          // Medication calculator button in calendar panel
static lv_obj_t *popup_med_calc = NULL;        // Calculator popup
static lv_obj_t *med_product_amount_input = NULL;  // Product amount input
static lv_obj_t *med_unit_dropdown = NULL;     // Unit type selector (ml/tsp/tbsp/drops/fl oz/cups/g)
static lv_obj_t *med_per_volume_input = NULL;  // Per X gallons/litres input
static lv_obj_t *med_tank_size_input = NULL;   // Tank size input
static lv_obj_t *med_unit_switch = NULL;       // Gallon/Liter toggle for "Per" field
static lv_obj_t *med_tank_unit_switch = NULL;  // Gallon/Liter toggle for "Tank Size" field
static lv_obj_t *med_result_label = NULL;      // Result display in popup
static lv_obj_t *ai_med_result_label = NULL;   // Result display in AI screen

// Latest calculation result (for AI integration) - exported for gemini_api
char latest_med_calculation[512] = {0};

// Latest mood reason (explains why mood is bad) - exported for gemini_api
char latest_mood_reason[512] = {0};

// Animation frame definitions
#define FRAMES_PER_CATEGORY 8
#define TOTAL_CATEGORIES 3
#define TOTAL_FRAMES 24  // 3 categories × 8 frames

// Set to 1 if colors appear wrong (swaps byte order)
#define SWAP_RGB565_BYTES 1  // Toggle if colors are wrong

// Function to load a frame from SPIFFS into specified buffer
// STEP 3: Exported for storage_task (runs on Core 1, not in LVGL context)
extern "C" bool load_frame_from_spiffs(uint8_t frame_num, uint8_t *buffer) {
    char filepath[64];
    snprintf(filepath, sizeof(filepath), "/spiffs/frame%d.bin", frame_num + 1);
    
    ESP_LOGI(TAG, "[STORAGE] Opening file: %s", filepath);
    
    FILE *f = fopen(filepath, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "[STORAGE] ✗ fopen() FAILED for %s (errno=%d)", filepath, errno);
        return false;
    }
    
    ESP_LOGI(TAG, "[STORAGE] Reading %d bytes from %s...", FRAME_SIZE, filepath);
    size_t bytes_read = fread(buffer, 1, FRAME_SIZE, f);
    fclose(f);
    
    if (bytes_read != FRAME_SIZE) {
        ESP_LOGE(TAG, "[STORAGE] ✗ fread() INCOMPLETE: got %zu bytes, expected %d", bytes_read, FRAME_SIZE);
        return false;
    }
    
    ESP_LOGI(TAG, "[STORAGE] Read %zu bytes successfully", bytes_read);
    
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

// Aquarium Parameter Values - Nitrogen Cycle & Water Quality
static float ammonia_ppm = 0.0f;         // Ammonia in ppm (MUST be 0)
static float nitrite_ppm = 0.0f;         // Nitrite in ppm (MUST be 0)
static float nitrate_ppm = 5.0f;         // Nitrate in ppm (<20 safe, <40 warning)
static float ph_level = 7.0f;            // pH 0-14 (6.5-7.5 ideal for most freshwater)
static uint32_t last_feed_time = 1;      // Timestamp of last feed
static uint32_t last_clean_time = 1;     // Timestamp of last water change
static uint32_t planned_water_change_interval = 7;       // User-set interval in DAYS (default 7 days)
static uint32_t planned_feed_interval = 28800;           // User-set interval in SECONDS (default 8 hours)

// STEP 5: AI advice cache for Blynk sync
static char latest_ai_advice[512] = "System initializing...";

// Thresholds based on aquarium research:
// Ammonia: Most toxic, any amount is dangerous
#define AMMONIA_SAFE 0.0f          // Only 0 ppm is safe
#define AMMONIA_WARNING 0.25f      // Any detection is concerning
#define AMMONIA_CRITICAL 0.5f      // Can kill fish quickly

// Nitrite: Toxic, interferes with oxygen uptake
#define NITRITE_SAFE 0.0f          // Only 0 ppm is safe
#define NITRITE_WARNING 0.25f      // Problematic
#define NITRITE_CRITICAL 0.5f      // Dangerous

// Nitrate: Less toxic but builds up over time
#define NITRATE_SAFE 20.0f         // <20 ppm is ideal
#define NITRATE_WARNING 40.0f      // 20-40 ppm needs water change soon
#define NITRATE_CRITICAL 80.0f     // >40 ppm is stressful, >80 dangerous

// pH: Most freshwater fish thrive in 6.5-7.5
#define PH_MIN_IDEAL 6.5f
#define PH_MAX_IDEAL 7.5f
#define PH_MIN_ACCEPTABLE 6.0f     // Acidic stress
#define PH_MAX_ACCEPTABLE 8.0f     // Alkaline stress
#define PH_CRITICAL_LOW 5.5f       // Dangerous
#define PH_CRITICAL_HIGH 8.5f      // Dangerous

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
    int ammonia_score;      // -2 to +2 (most critical)
    int nitrite_score;      // -2 to +2 (very critical)
    int nitrate_score;      // -2 to +2
    int ph_score;           // -2 to +2
    int feed_score;         // -2 to +2
    int clean_score;        // -2 to +2
    int total_score;        // Sum of all scores
} mood_scores_t;

static mood_scores_t current_mood_scores = {};

// Forward declarations
static void show_monthly_calendar(void);
static void panel_button_event_cb(lv_event_t *e);
static void keyboard_event_cb(lv_event_t *e);
static void close_numeric_input(void);
static void update_panel_dial(float value, bool animate);
static void refresh_weekly_calendar_dots(void);
static void evaluate_and_update_mood(void);
static void update_ai_assistant(void);
static void date_update_timer_cb(lv_timer_t *timer);
static uint32_t get_current_time_seconds(void);
static void main_button_event_cb(lv_event_t *e);
static lv_color_t score_to_rgb_color(int score);
static void update_button_colors(void);
static void animation_init_timer_cb(lv_timer_t *timer);
static void animation_timer_cb(lv_timer_t *timer);
static void calculate_medication_dosage(void);
static void show_med_calculator_popup(void);
static void med_calc_close_event_cb(lv_event_t *e);
static void med_calc_calculate_event_cb(lv_event_t *e);
static void input_field_event_cb(lv_event_t *e);

// ═══════════════════════════════════════════════════════════════════════════
// SD Card Logging Functions
// ═══════════════════════════════════════════════════════════════════════════

#define SD_LOG_DIR "/sdcard/logs"

/**
 * @brief Ensure log directory exists on SD card
 */
static bool ensure_log_directory(void) {
    struct stat st;
    if (stat(SD_LOG_DIR, &st) == -1) {
        if (mkdir(SD_LOG_DIR, 0700) == -1) {
            ESP_LOGE(TAG, "Failed to create log directory: %s (errno=%d)", SD_LOG_DIR, errno);
            return false;
        }
        ESP_LOGI(TAG, "Created log directory: %s", SD_LOG_DIR);
    }
    return true;
}

/**
 * @brief Save medication calculation to SD card
 */
static void save_medication_to_sd(void) {
    if (!ensure_log_directory()) return;
    
    time_t now = time(NULL);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    
    char filepath[128];
    snprintf(filepath, sizeof(filepath), "%s/medication_%04d%02d%02d.csv",
             SD_LOG_DIR, timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
    
    // Check if file exists to add header
    bool file_exists = (access(filepath, F_OK) == 0);
    
    FILE *f = fopen(filepath, "a");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open medication log: %s (errno=%d)", filepath, errno);
        return;
    }
    
    // Write header if new file
    if (!file_exists) {
        fprintf(f, "DateTime,ProductAmount,Unit,PerVolume,PerUnit,TankSize,TankUnit,DosageML,DosageTsp,DosageTbsp\n");
    }
    
    // Write log entry
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d,%.2f,%s,%.2f,%s,%.2f,%s,%.2f,%.2f,%.2f\n",
            timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
            timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
            med_calc_state.product_amount,
            (med_calc_state.unit_type == 0) ? "ml" : 
            (med_calc_state.unit_type == 1) ? "tsp" :
            (med_calc_state.unit_type == 2) ? "tbsp" :
            (med_calc_state.unit_type == 3) ? "drops" :
            (med_calc_state.unit_type == 4) ? "fl oz" :
            (med_calc_state.unit_type == 5) ? "cups" : "g",
            med_calc_state.per_volume,
            med_calc_state.is_gallons ? "gal" : "L",
            med_calc_state.tank_size,
            med_calc_state.tank_is_gallons ? "gal" : "L",
            med_calc_state.calculated_dosage,
            med_calc_state.calculated_dosage / 5.0f,
            med_calc_state.calculated_dosage / 15.0f);
    
    fclose(f);
    ESP_LOGI(TAG, "Medication log saved to SD: %s", filepath);
}

/**
 * @brief Save parameter log to SD card
 */
static void save_parameters_to_sd(float ammonia, float nitrate, float nitrite, float ph) {
    if (!ensure_log_directory()) return;
    
   time_t now = time(NULL);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    
    char filepath[128];
    snprintf(filepath, sizeof(filepath), "%s/parameters_%04d%02d%02d.csv",
             SD_LOG_DIR, timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
    
    bool file_exists = (access(filepath, F_OK) == 0);
    
    FILE *f = fopen(filepath, "a");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open parameter log: %s (errno=%d)", filepath, errno);
        return;
    }
    
    if (!file_exists) {
        fprintf(f, "DateTime,Ammonia_ppm,Nitrate_ppm,Nitrite_ppm,pH\n");
    }
    
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d,%.3f,%.2f,%.3f,%.2f\n",
            timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
            timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
            ammonia, nitrate, nitrite, ph);
    
    fclose(f);
    ESP_LOGI(TAG, "Parameter log saved to SD: %s", filepath);
}

/**
 * @brief Save water change log to SD card
 */
static void save_water_change_to_sd(uint8_t interval_days) {
    if (!ensure_log_directory()) return;
    
    time_t now = time(NULL);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    
    char filepath[128];
    snprintf(filepath, sizeof(filepath), "%s/water_change_%04d%02d%02d.csv",
             SD_LOG_DIR, timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
    
    bool file_exists = (access(filepath, F_OK) == 0);
    
    FILE *f = fopen(filepath, "a");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open water change log: %s (errno=%d)", filepath, errno);
        return;
    }
    
    if (!file_exists) {
        fprintf(f, "DateTime,PlannedIntervalDays\n");
    }
    
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d,%d\n",
            timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
            timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
            interval_days);
    
    fclose(f);
    ESP_LOGI(TAG, "Water change log saved to SD: %s", filepath);
}

/**
 * @brief Save feed log to SD card
 */
static void save_feed_to_sd(uint8_t feeds_per_day) {
    if (!ensure_log_directory()) return;
    
    time_t now = time(NULL);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    
    char filepath[128];
    snprintf(filepath, sizeof(filepath), "%s/feed_%04d%02d%02d.csv",
             SD_LOG_DIR, timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
    
    bool file_exists = (access(filepath, F_OK) == 0);
    
    FILE *f = fopen(filepath, "a");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open feed log: %s (errno=%d)", filepath, errno);
        return;
    }
    
    if (!file_exists) {
        fprintf(f, "DateTime,FeedsPerDay\n");
    }
    
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d,%d\n",
            timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
            timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
            feeds_per_day);
    
    fclose(f);
    ESP_LOGI(TAG, "Feed log saved to SD: %s", filepath);
}

/**
 * @brief Pure function: Calculate mood scores from parameters
 * 
 * CRITICAL: Contains EXACT SAME logic as original evaluate_and_update_mood()
 * NO side effects, NO global access, NO logging, NO UI calls
 * 
 * Exported for use by logic_task in task_coordinator
 * 
 * @param params Input parameters (by value)
 * @param current_time Current time in seconds
 * @return mood_result_t with scores and category
 */
extern "C" mood_result_t calculate_mood_scores(aquarium_params_t params, uint32_t current_time)
{
    mood_result_t result = {};
    
    uint32_t time_since_feed = current_time - params.last_feed_time;
    uint32_t time_since_clean = current_time - params.last_clean_time;
    
    // Reset scores
    result.ammonia_score = 0;
    result.nitrite_score = 0;
    result.nitrate_score = 0;
    result.ph_score = 0;
    result.feed_score = 0;
    result.clean_score = 0;
    
    // 1. AMMONIA SCORING (Most Critical - ANY amount is dangerous)
    if (params.ammonia_ppm <= AMMONIA_SAFE) {
        result.ammonia_score = 2;  // Perfect - 0 ppm
    } else if (params.ammonia_ppm < AMMONIA_WARNING) {
        result.ammonia_score = 0;  // Detectable but low
    } else if (params.ammonia_ppm < AMMONIA_CRITICAL) {
        result.ammonia_score = -1; // Warning level - stress
    } else {
        result.ammonia_score = -2; // Critical - fish death imminent
    }
    
    // 2. NITRITE SCORING (Very Critical - toxic to fish)
    if (params.nitrite_ppm <= NITRITE_SAFE) {
        result.nitrite_score = 2;  // Perfect - 0 ppm
    } else if (params.nitrite_ppm < NITRITE_WARNING) {
        result.nitrite_score = 0;  // Detectable but low
    } else if (params.nitrite_ppm < NITRITE_CRITICAL) {
        result.nitrite_score = -1; // Warning - gill damage
    } else {
        result.nitrite_score = -2; // Critical - severe stress
    }
    
    // 3. NITRATE SCORING (Less toxic but indicates waste buildup)
    if (params.nitrate_ppm < NITRATE_SAFE) {
        result.nitrate_score = 2;  // Perfect - under 20 ppm
    } else if (params.nitrate_ppm < NITRATE_WARNING) {
        result.nitrate_score = 1;  // Acceptable - 20-40 ppm
    } else if (params.nitrate_ppm < NITRATE_CRITICAL) {
        result.nitrate_score = -1; // Warning - needs water change
    } else {
        result.nitrate_score = -2; // Critical - over 80 ppm
    }
    
    // 4. pH SCORING (Critical for fish health and stress)
    if (params.ph_level >= PH_MIN_IDEAL && params.ph_level <= PH_MAX_IDEAL) {
        result.ph_score = 2;  // Perfect - 6.5-7.5 range
    } else if (params.ph_level >= PH_MIN_ACCEPTABLE && params.ph_level <= PH_MAX_ACCEPTABLE) {
        result.ph_score = 1;  // Acceptable - 6.0-8.0 range
    } else if (params.ph_level < PH_CRITICAL_LOW || params.ph_level > PH_CRITICAL_HIGH) {
        result.ph_score = -2; // Critical - extreme pH kills fish
    } else {
        result.ph_score = -1; // Concerning - approaching danger zone
    }
    
    // 5. FEEDING SCORING (based on time since last feed)
    float feed_warning_time = params.planned_feed_interval * 1.5f;   // 1.5x interval
    float feed_critical_time = params.planned_feed_interval * 2.0f;  // 2x interval
    
    if (time_since_feed <= params.planned_feed_interval) {
        result.feed_score = 2;  // Fed on schedule
    } else if (time_since_feed <= feed_warning_time) {
        result.feed_score = 1;  // Slightly overdue
    } else if (time_since_feed <= feed_critical_time) {
        result.feed_score = -1; // Hungry
    } else {
        result.feed_score = -2; // Starving
    }
    
    // 6. WATER CHANGE SCORING (affects all parameters over time)
    float clean_warning_time = (params.planned_water_change_interval * 86400) * 1.2f;   // 20% over (days to seconds)
    float clean_critical_time = (params.planned_water_change_interval * 86400) * 1.5f;  // 50% over (days to seconds)
    
    if (time_since_clean <= (params.planned_water_change_interval * 86400)) {
        result.clean_score = 2;  // Clean water
    } else if (time_since_clean <= clean_warning_time) {
        result.clean_score = 1;  // Needs change soon
    } else if (time_since_clean <= clean_critical_time) {
        result.clean_score = -1; // Overdue
    } else {
        result.clean_score = -2; // Very overdue - poor water quality
    }
    
    // Calculate total score with proper weighting
    // Ammonia and Nitrite get counted first (most critical)
    result.total_score = result.ammonia_score +
                         result.nitrite_score +
                         result.nitrate_score +
                         result.ph_score +
                         result.feed_score +
                         result.clean_score;
    
    // ═══════════════════════════════════════════════════════════════════════════
    // CRITICAL OVERRIDES - Single factors that force mood change (ALL 6 FACTORS)
    // ═══════════════════════════════════════════════════════════════════════════
    
    // Build mood reason string (clear latest_mood_reason if mood is good)
    latest_mood_reason[0] = '\0';
    
    // OVERRIDE 1: Critical ammonia (-2) = ANGRY (fish death imminent)
    if (result.ammonia_score <= -2) {
        result.category = 2;  // ANGRY - immediate danger
        snprintf(latest_mood_reason, sizeof(latest_mood_reason),
                 "🚨 CRITICAL: Ammonia %.2f ppm (TOXIC! Fish dying! Emergency water change needed!)",
                 params.ammonia_ppm);
        return result;
    }
    
    // OVERRIDE 2: Critical nitrite (-2) = ANGRY (severe oxygen deprivation)
    if (result.nitrite_score <= -2) {
        result.category = 2;  // ANGRY - immediate danger
        snprintf(latest_mood_reason, sizeof(latest_mood_reason),
                 "🚨 CRITICAL: Nitrite %.2f ppm (TOXIC! Severe oxygen deprivation! Water change NOW!)",
                 params.nitrite_ppm);
        return result;
    }
    
    // OVERRIDE 3: Critical pH (-2) = ANGRY (extreme pH is lethal)
    if (result.ph_score <= -2) {
        result.category = 2;  // ANGRY - immediate danger
        snprintf(latest_mood_reason, sizeof(latest_mood_reason),
                 "🚨 CRITICAL: pH %.1f (EXTREME! Lethal to fish! Adjust pH immediately!)",
                 params.ph_level);
        return result;
    }
    
    // OVERRIDE 4: Critical nitrate (-2) = ANGRY (severe waste buildup)
    if (result.nitrate_score <= -2) {
        result.category = 2;  // ANGRY - immediate danger
        snprintf(latest_mood_reason, sizeof(latest_mood_reason),
                 "🚨 CRITICAL: Nitrate %.0f ppm (VERY HIGH! Severe waste buildup! Water change urgently needed!)",
                 params.nitrate_ppm);
        return result;
    }
    
    // OVERRIDE 5: Critical feed (-2) = ANGRY (fish starving)
    if (result.feed_score <= -2) {
        result.category = 2;  // ANGRY - immediate danger
        float hours_late = time_since_feed / 3600.0f;
        snprintf(latest_mood_reason, sizeof(latest_mood_reason),
                 "🚨 CRITICAL: Not fed for %.1f hours (STARVING! Feed immediately!)",
                 hours_late);
        return result;
    }
    
    // OVERRIDE 6: Critical clean (-2) = ANGRY (water quality severely degraded)
    if (result.clean_score <= -2) {
        result.category = 2;  // ANGRY - immediate danger
        float days_late = time_since_clean / 86400.0f;
        snprintf(latest_mood_reason, sizeof(latest_mood_reason),
                 "🚨 CRITICAL: Water not changed for %.1f days (VERY OVERDUE! Poor water quality! Clean tank now!)",
                 days_late);
        return result;
    }
    
    // ═══════════════════════════════════════════════════════════════════════════
    // WARNING OVERRIDES - Prevent HAPPY mood when problems are present
    // ═══════════════════════════════════════════════════════════════════════════
    
    // Build warning reason (multiple warnings possible)
    char warning_reasons[512] = {0};
    int warning_count = 0;
    
    // Warning ammonia/nitrite (never happy with toxins)
    if (result.ammonia_score <= -1) {
        warning_count++;
        snprintf(warning_reasons + strlen(warning_reasons), sizeof(warning_reasons) - strlen(warning_reasons),
                 "⚠️ Ammonia %.2f ppm (Detectable ammonia causing stress). ",
                 params.ammonia_ppm);
    }
    if (result.nitrite_score <= -1) {
        warning_count++;
        snprintf(warning_reasons + strlen(warning_reasons), sizeof(warning_reasons) - strlen(warning_reasons),
                 "⚠️ Nitrite %.2f ppm (Detectable nitrite causing gill damage). ",
                 params.nitrite_ppm);
    }
    
    // Warning pH
    if (result.ph_score <= -1) {
        warning_count++;
        snprintf(warning_reasons + strlen(warning_reasons), sizeof(warning_reasons) - strlen(warning_reasons),
                 "⚠️ pH %.1f (Approaching danger zone). ",
                 params.ph_level);
    }
    
    // Warning nitrate
    if (result.nitrate_score <= -1) {
        warning_count++;
        snprintf(warning_reasons + strlen(warning_reasons), sizeof(warning_reasons) - strlen(warning_reasons),
                 "⚠️ Nitrate %.0f ppm (High waste buildup, needs water change). ",
                 params.nitrate_ppm);
    }
    
    // Warning feed
    if (result.feed_score <= -1) {
        warning_count++;
        float hours_late = time_since_feed / 3600.0f;
        snprintf(warning_reasons + strlen(warning_reasons), sizeof(warning_reasons) - strlen(warning_reasons),
                 "⚠️ Not fed for %.1f hours (Hungry, feed soon). ",
                 hours_late);
    }
    
    // Warning clean
    if (result.clean_score <= -1) {
        warning_count++;
        float days_late = time_since_clean / 86400.0f;
        snprintf(warning_reasons + strlen(warning_reasons), sizeof(warning_reasons) - strlen(warning_reasons),
                 "⚠️ Water not changed for %.1f days (Overdue, clean soon). ",
                 days_late);
    }
    
    // If any warnings exist, force at least SAD mood
    if (warning_count > 0) {
        strcpy(latest_mood_reason, warning_reasons);
        if (result.total_score >= 0) {
            result.category = 1;  // SAD - warning conditions present
        } else {
            result.category = 2;  // ANGRY - warning + other problems
        }
        return result;
    }
    
    // ═══════════════════════════════════════════════════════════════════════════
    // Normal mood determination (only if no critical or warning overrides)
    // ═══════════════════════════════════════════════════════════════════════════
    if (result.total_score >= 6) {
        result.category = 0;  // ANIM_CATEGORY_HAPPY
        snprintf(latest_mood_reason, sizeof(latest_mood_reason),
                 "😊 Everything is perfect! Water quality excellent, feeding on schedule, tank clean!");
    } else if (result.total_score >= 0) {
        result.category = 1;  // ANIM_CATEGORY_SAD
        snprintf(latest_mood_reason, sizeof(latest_mood_reason),
                 "😐 Conditions are okay but could be better. Check parameters and schedules.");
    } else {
        result.category = 2;  // ANIM_CATEGORY_ANGRY
        snprintf(latest_mood_reason, sizeof(latest_mood_reason),
                 "😠 Multiple issues detected! Check water parameters, feeding, and cleaning schedules!");
    }
    
    return result;
}

/**
 * @brief Calculate next feed time based on feeds per day schedule
 */
static time_t get_next_feed_time(uint8_t feeds_per_day) {
    time_t now = time(NULL);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    
    // Feed times based on feeds_per_day
    int feed_hours[4][4] = {
        {12, 0, 0, 0},        // 1 feed: 12:00 PM
        {8, 18, 0, 0},        // 2 feeds: 8:00 AM, 6:00 PM
        {7, 12, 19, 0},       // 3 feeds: 7:00 AM, 12:00 PM, 7:00 PM
        {7, 11, 15, 19}       // 4 feeds: 7:00 AM, 11:00 AM, 3:00 PM, 7:00 PM
    };
    
    int current_hour = timeinfo.tm_hour;
    int schedule_idx = (feeds_per_day > 4) ? 3 : feeds_per_day - 1;
    
    // Find next feed time today
    for (int i = 0; i < feeds_per_day && i < 4; i++) {
        if (feed_hours[schedule_idx][i] > current_hour) {
            timeinfo.tm_hour = feed_hours[schedule_idx][i];
            timeinfo.tm_min = 0;
            timeinfo.tm_sec = 0;
            return mktime(&timeinfo);
        }
    }
    
    // No more feeds today, return first feed tomorrow
    timeinfo.tm_mday++;
    timeinfo.tm_hour = feed_hours[schedule_idx][0];
    timeinfo.tm_min = 0;
    timeinfo.tm_sec = 0;
    return mktime(&timeinfo);
}

/**
 * @brief One-shot timer to scroll to animation after UI is ready
 */
static void scroll_to_animation_cb(lv_timer_t *timer)
{
    if (scroll_container) {
        lv_obj_scroll_to_y(scroll_container, 150, LV_ANIM_OFF);
        ESP_LOGI(TAG, "Scrolled to animation view (Y=150)");
    }
    lv_timer_del(timer);  // Delete one-shot timer
}

/**
 * ═════════════════════════════════════════════════════════════════════════════
 * ONE-SHOT INITIALIZER: Create static frame timer after LVGL task is running
 * ═════════════════════════════════════════════════════════════════════════════
 * This ensures the frame update timer is created in the correct LVGL context.
 * 
 * CRITICAL CHANGE: Timer runs every 3 seconds (3000ms) NOT 83ms.
 * This is the KEY architectural change that eliminates scrolling lag.
 */
static void animation_init_timer_cb(lv_timer_t *timer)
{
    ESP_LOGI(TAG, "★ Creating static frame timer (3-second intervals)");
    static_frame_timer = lv_timer_create(animation_timer_cb, 3000, NULL);
    if (static_frame_timer) {
        ESP_LOGI(TAG, "★ Static frame timer created - handle=%p", static_frame_timer);
        ESP_LOGI(TAG, "★ animation_img=%p", animation_img);
        ESP_LOGI(TAG, "★ Frame update interval: 3 seconds (smooth scrolling enabled)");
    } else {
        ESP_LOGE(TAG, "Failed to create static frame timer!");
    }
    
    // Initialize timestamp for first frame update
    last_frame_update_time = (uint32_t)(esp_timer_get_time() / 1000000);
    
    // Delete this one-shot initializer
    lv_timer_del(timer);
}

/**
 * ═════════════════════════════════════════════════════════════════════════════
 * STATIC FRAME TIMER CALLBACK - NON-BLOCKING LVGL TASK PATTERN
 * ═════════════════════════════════════════════════════════════════════════════
 * 
 * CRITICAL DESIGN: This function NEVER blocks, waits, or accesses files.
 * It ONLY checks flags and swaps image pointers when frames are ready.
 * 
 * WHY THIS PREVENTS LAG:
 * - Runs every 10 seconds (not 83ms like old animation)
 * - Never calls SPIFFS/SD functions
 * - Never waits on queues
 * - Just checks volatile flag and swaps pointer - < 1µs execution time
 * - LVGL can process touch/scroll events without I/O interference
 * 
 * OPERATION:
 * 1. Check if 10 seconds have elapsed since last frame
 * 2. Check if next frame is ready in buffer (non-blocking flag check)
 * 3. If ready: swap lv_img_dsc_t pointer, consume buffer, request next frame
 * 4. If not ready: do nothing, try again next timer tick
 * 
 * Frame progression: 0 → 1 → 2 → 3 → 4 → 5 → 6 → 7 → 0 (loop)
 * On mood change: reset to 0
 */
static void animation_timer_cb(lv_timer_t *timer)
{
    static uint32_t call_count = 0;
    call_count++;
    
    if (!animation_img) {
        if (call_count == 1) {
            ESP_LOGE(TAG, "Static frame timer: animation_img is NULL!");
        }
        return;
    }
    
    // ═════════════════════════════════════════════════════════════════════════
    // STEP 1: Check if 3 seconds have elapsed (NON-BLOCKING TIME CHECK)
    // ═════════════════════════════════════════════════════════════════════════
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000);  // Seconds since boot
    uint32_t elapsed = now - last_frame_update_time;
    
    // Log status every 3 seconds for debugging
    if (call_count % 1 == 0) {
        ESP_LOGI(TAG, "[STATIC] Timer tick %lu | frame=%d/%d cat=%d | elapsed=%lus | Buffers: A(ready=%d idx=%d) B(ready=%d idx=%d)",
                 call_count, current_frame, 7, current_category, elapsed,
                 buffer_a_ready, buffer_a_frame_index,
                 buffer_b_ready, buffer_b_frame_index);
    }
    
    // Only update frame every 3 seconds (not continuous animation)
    if (elapsed < 3) {
        // Not time yet - do nothing (this is the KEY to preventing lag)
        return;
    }
    
    // ═════════════════════════════════════════════════════════════════════════
    // STEP 2: Calculate next frame index (0-7, loops back to 0)
    // ═════════════════════════════════════════════════════════════════════════
    uint8_t next_frame_local = (current_frame + 1) % 8;  // 0-7 loop
    uint8_t next_abs_frame = (current_category * 8) + next_frame_local;
    
    // ═════════════════════════════════════════════════════════════════════════
    // STEP 3: Check if next frame is READY (NON-BLOCKING FLAG CHECK)
    // ═════════════════════════════════════════════════════════════════════════
    // This is LOCK-FREE - just read a volatile bool (< 1µs)
    bool frame_ready = false;
    uint8_t *display_buffer = NULL;
    uint8_t buffer_used = 0;
    
    if (buffer_a_ready && buffer_a_frame_index == next_abs_frame) {
        display_buffer = frame_buffer_a;
        frame_ready = true;
        buffer_used = 0;
    } else if (buffer_b_ready && buffer_b_frame_index == next_abs_frame) {
        display_buffer = frame_buffer_b;
        frame_ready = true;
        buffer_used = 1;
    }
    
    if (!frame_ready) {
        // Frame not loaded yet - skip this update (acceptable with 3s interval)
        ESP_LOGW(TAG, "[STATIC] Frame %d not ready, will retry in 3s", next_abs_frame);
        // IMPORTANT: Do NOT reset last_frame_update_time - will retry next tick
        return;
    }
    
    // ═════════════════════════════════════════════════════════════════════════
    // STEP 4: FRAME IS READY - Update display (3 critical sub-steps)
    // ═════════════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "[STATIC] Frame %d ready in buffer_%c - updating display",
             next_abs_frame, (buffer_used == 0 ? 'A' : 'B'));
    
    // Sub-step 4A: CONSUME BUFFER (mark as free for storage_task)
    if (buffer_used == 0) {
        buffer_a_ready = false;
    } else {
        buffer_b_ready = false;
    }
    
    // Sub-step 4B: ADVANCE FRAME INDEX
    current_frame = next_frame_local;
    last_frame_update_time = now;  // Reset timer
    
    // Sub-step 4C: SWAP IMAGE DESCRIPTOR (force LVGL redraw)
    // Alternate between two descriptors so LVGL sees a "new" pointer
    active_dsc = (active_dsc == &anim_dsc_a) ? &anim_dsc_b : &anim_dsc_a;
    active_dsc->data = display_buffer;
    lv_img_set_src(animation_img, active_dsc);
    
    ESP_LOGI(TAG, "[STATIC] ✓ DISPLAYED frame=%d (dsc=%p)", current_frame, active_dsc);
    
    // ═════════════════════════════════════════════════════════════════════════
    // STEP 5: REQUEST NEXT FRAME from storage_task (NON-BLOCKING QUEUE SEND)
    // ═════════════════════════════════════════════════════════════════════════
    uint8_t preload_frame_local = (current_frame + 1) % 8;
    uint8_t preload_abs_frame = (current_category * 8) + preload_frame_local;
    
    anim_frame_request_msg_t request = { .frame_index = preload_abs_frame };
    if (xQueueOverwrite(queue_anim_frame_request, &request) != pdTRUE) {
        ESP_LOGE(TAG, "[STATIC] Failed to request frame %d", preload_abs_frame);
    } else {
        ESP_LOGI(TAG, "[STATIC] Requested frame %d for next update", preload_abs_frame);
    }
    
    // Done! Function took < 100µs - no I/O blocking whatsoever
}

/**
 * @brief Get current system time in seconds (from boot)
 */
static uint32_t get_current_time_seconds(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);  // Convert microseconds to seconds
}

/**
 * @brief Map mood score to RGB color gradient
 * @param score Score from -2 (critical/red) to +2 (perfect/green)
 * @return RGB color
 */
static lv_color_t score_to_rgb_color(int score)
{
    // Score mapping with aesthetically pleasing colors
    switch(score) {
        case 2:  return lv_color_hex(0x4CAF50);  // Soft Green (perfect)
        case 1:  return lv_color_hex(0x8BC34A);  // Light Green (good)
        case 0:  return lv_color_hex(0xFFC107);  // Amber (acceptable)
        case -1: return lv_color_hex(0xFF9800);  // Soft Orange (concerning)
        case -2: return lv_color_hex(0xF44336);  // Soft Red (critical)
        default: return lv_color_hex(0xFFC107);  // Amber (fallback)
    }
}

/**
 * @brief Update button colors based on current mood scores
 */
static void update_button_colors(void)
{
    if (btn_feed_main) {
        lv_color_t feed_color = score_to_rgb_color(current_mood_scores.feed_score);
        lv_obj_set_style_bg_color(btn_feed_main, feed_color, LV_PART_MAIN);
    }
    
    if (btn_water_main) {
        lv_color_t clean_color = score_to_rgb_color(current_mood_scores.clean_score);
        lv_obj_set_style_bg_color(btn_water_main, clean_color, LV_PART_MAIN);
    }
}

/**
 * @brief LVGL timer callback: Receive mood calculation results from logic_task
 * 
 * STEP 2: Polls queue_mood_result every 50ms to apply results from background calculation
 * Maintains EXACT SAME behavior as original evaluate_and_update_mood()
 */
static void mood_result_handler(lv_timer_t *timer)
{
    mood_result_t result;
    
    // Non-blocking receive (0 ticks timeout)
    if (xQueueReceive(queue_mood_result, &result, 0) == pdTRUE) {
        // Apply results to global state (EXACT SAME as Step 1)
        current_mood_scores.ammonia_score = result.ammonia_score;
        current_mood_scores.nitrite_score = result.nitrite_score;
        current_mood_scores.nitrate_score = result.nitrate_score;
        current_mood_scores.ph_score = result.ph_score;
        current_mood_scores.feed_score = result.feed_score;
        current_mood_scores.clean_score = result.clean_score;
        current_mood_scores.total_score = result.total_score;
        
        uint8_t new_category = result.category;  // 0=HAPPY, 1=SAD, 2=ANGRY
        
        // Update mood if changed (EXACT SAME as Step 1)
        if (new_category != current_category) {
            ESP_LOGI(TAG, "Mood changed: %s -> %s (Score: %d)",
                     current_category == 0 ? "HAPPY" : (current_category == 1 ? "SAD" : "ANGRY"),
                     new_category == 0 ? "HAPPY" : (new_category == 1 ? "SAD" : "ANGRY"),
                     result.total_score);
            dashboard_set_animation_category(new_category);
        }
        
        // Log detailed mood analysis (EXACT SAME as Step 1)
        ESP_LOGI(TAG, "Mood Scores: NH3=%d, NO2=%d, NO3=%d, pH=%d, Feed=%d, Clean=%d | Total=%d",
                 result.ammonia_score,
                 result.nitrite_score,
                 result.nitrate_score,
                 result.ph_score,
                 result.feed_score,
                 result.clean_score,
                 result.total_score);
        
        // Update button colors based on new scores (EXACT SAME as Step 1)
        update_button_colors();
    }
}

/**
 * STEP 4: AI Result Handler
 * 
 * Polls queue_ai_result for AI advice from wifi_task.
 * Updates ai_text_label when result arrives.
 * STEP 5: Also caches advice for Blynk sync.
 */
static void ai_result_handler(lv_timer_t *timer)
{
    // Check if WiFi just became ready and we haven't sent initial AI request
    if (!ai_initial_request_sent && gemini_is_wifi_connected()) {
        ESP_LOGI(TAG, "WiFi is ready - triggering initial AI assistant request");
        ai_initial_request_sent = true;
        update_ai_assistant();
    }
    
    ai_result_msg_t result;
    
    // Non-blocking receive (0 ticks timeout)
    if (xQueueReceive(queue_ai_result, &result, 0) == pdTRUE) {
        if (!ai_text_label) return;
        
        if (result.success) {
            // Display AI advice
            lv_label_set_text(ai_text_label, result.advice);
            // STEP 5: Cache advice for Blynk sync
            snprintf(latest_ai_advice, sizeof(latest_ai_advice), "%s", result.advice);
            ESP_LOGI(TAG, "AI advice received and displayed");
            // Update timestamp only on SUCCESS to enable failed request retries
            last_ai_update = get_current_time_seconds();
        } else {
            // Fallback if API fails (but tank is healthy)
            const char *fallback = LV_SYMBOL_OK " Tank is healthy!\n\n"
                "All parameters normal.\n"
                "Continue regular maintenance.\n\n"
                "(AI offline - WiFi issue)";
            lv_label_set_text(ai_text_label, fallback);
            // STEP 5: Cache fallback for Blynk sync
            snprintf(latest_ai_advice, sizeof(latest_ai_advice), "%s", fallback);
            ESP_LOGW(TAG, "AI request failed, showing fallback");
            // Reset flag to allow retry when system becomes fully ready
            ai_initial_request_sent = false;
        }
    }
}

/**
 * @brief Timer callback to update date display every 10 minutes
 * Updates both animation screen date AND calendar panel date
 */
static void date_update_timer_cb(lv_timer_t *timer)
{
    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    
    // Only update if time is synced (year should be >= 2024)
    if (timeinfo.tm_year < (2024 - 1900)) {
        ESP_LOGW(TAG, "Date update skipped - time not synced yet");
        return;
    }
    
    // Update animation screen date (top-left corner)
    if (date_label && date_shadow) {
        char date_str[16];
        strftime(date_str, sizeof(date_str), "%d %b", &timeinfo);
        
        // Convert to uppercase
        for (int i = 0; date_str[i]; i++) {
            date_str[i] = toupper((unsigned char)date_str[i]);
        }
        
        lv_label_set_text(date_shadow, date_str);
        lv_label_set_text(date_label, date_str);
    }
    
    // Update calendar panel date (synchronized update)
    if (panel_day_label && panel_date_label && panel_month_label) {
        const char *days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
        const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        
        lv_label_set_text(panel_day_label, days[timeinfo.tm_wday]);
        
        char date_str[8];
        snprintf(date_str, sizeof(date_str), "%d", timeinfo.tm_mday);
        lv_label_set_text(panel_date_label, date_str);
        
        char month_str[16];
        snprintf(month_str, sizeof(month_str), "%s %d", months[timeinfo.tm_mon], 1900 + timeinfo.tm_year);
        lv_label_set_text(panel_month_label, month_str);
    }
    
    ESP_LOGI(TAG, "Date displays updated (animation + calendar)");
}

/**
 * STEP 5: Blynk Snapshot Publisher
 * 
 * Timer callback (30s interval) that creates a snapshot of current
 * aquarium state and sends it to wifi_task for Blynk cloud sync.
 * 
 * Uses xQueueOverwrite - only latest snapshot needed, older ones can be discarded.
 */
static void blynk_snapshot_publisher(lv_timer_t *timer)
{
    blynk_sync_msg_t snapshot;
    
    // Get current time
    uint32_t current_time = get_current_time_seconds();
    
    // Copy water parameters (snapshot - no LVGL lock needed)
    snapshot.ammonia_ppm = ammonia_ppm;
    snapshot.nitrite_ppm = nitrite_ppm;
    snapshot.nitrate_ppm = nitrate_ppm;
    snapshot.ph_level = ph_level;
    
    // Calculate time since feed/clean (in requested units)
    float hours_since_feed = (current_time - last_feed_time) / 3600.0f;
    float days_since_clean = (current_time - last_clean_time) / 86400.0f;
    snapshot.feed_hours = hours_since_feed;
    snapshot.clean_days = days_since_clean;
    
    // Build mood string based on current category
    const char *mood_str = "HAPPY";
    if (current_category == 1) mood_str = "SAD";       // 1 = SAD
    else if (current_category == 2) mood_str = "ANGRY";  // 2 = ANGRY
    snprintf(snapshot.mood, sizeof(snapshot.mood), "%s", mood_str);
    
    // Copy latest AI advice
    snprintf(snapshot.ai_advice, sizeof(snapshot.ai_advice), "%s", latest_ai_advice);
    
    // Send to wifi_task (non-blocking overwrite - only latest snapshot matters)
    if (xQueueOverwrite(queue_blynk_sync, &snapshot) == pdTRUE) {
        ESP_LOGI(TAG, "Blynk snapshot sent (Mood=%s, Feed=%.1fh, Clean=%.1fd)", 
                 mood_str, hours_since_feed, days_since_clean);
    } else {
        ESP_LOGW(TAG, "Failed to send Blynk snapshot");
    }
}

/**
 * @brief Evaluate all parameters and determine mood
 * 
 * Realistic Aquarium Mood Scoring System based on Nitrogen Cycle
 * 
 * Priority Weighting (most toxic first):
 * 1. Ammonia (NH3): CRITICAL - Can kill fish in hours if present
 * 2. Nitrite (NO2): CRITICAL - Burns gills, interferes with oxygen
 * 3. pH Extremes: HIGH - Major stress factor
 * 4. Nitrate (NO3): MEDIUM - Chronic issue, builds up slowly
 * 5. Water Change: MEDIUM - Affects all water quality parameters
 * 6. Feeding: LOWER - Fish can survive days without food
 * 
 * Each parameter contributes -2 to +2 points
 * Total Score Range: -12 to +12
 * 
 * Mood Assignment:
 * - HAPPY (0):  Total score >= 6  (All critical parameters safe)
 * - SAD (1):    Total score 0-5   (Some warnings, no critical issues)
 * - ANGRY (2):  Total score < 0   (Critical water quality issues)
 */
static void evaluate_and_update_mood(void)
{
    // STEP 2: Send parameters to logic_task for calculation
    // Gather parameters into struct
    aquarium_params_t params = {
        .ammonia_ppm = ammonia_ppm,
        .nitrite_ppm = nitrite_ppm,
        .nitrate_ppm = nitrate_ppm,
        .ph_level = ph_level,
        .last_feed_time = last_feed_time,
        .last_clean_time = last_clean_time,
        .planned_feed_interval = planned_feed_interval,
        .planned_water_change_interval = planned_water_change_interval
    };
    
    // Send to logic_task (non-blocking)
    xQueueSend(queue_param_update, &params, 0);
    
    // Result will be received by mood_result_handler() timer callback
}

/**
 * @brief AI Assistant - Query Gemini API for advice
 */
static void update_ai_assistant(void)
{
    ESP_LOGI(TAG, "update_ai_assistant() called");
    
    if (!ai_text_label) {
        ESP_LOGW(TAG, "AI update skipped: ai_text_label is NULL");
        return;
    }
    
    uint32_t current_time = get_current_time_seconds();
    ESP_LOGI(TAG, "AI update: current_time=%lu, mood=%d", current_time, current_category);
    uint32_t time_since_feed = current_time - last_feed_time;
    uint32_t time_since_clean = current_time - last_clean_time;
    
    // Convert to hours/days for display
    float hours_since_feed = time_since_feed / 3600.0f;
    float days_since_clean = time_since_clean / 86400.0f;
    
    // Check if mood is SAD or ANGRY - provide immediate local feedback (NO RATE LIMIT)
    if (current_category == 1 || current_category == 2) {  // 1=SAD, 2=ANGRY
        char local_advice[512] = "";
        bool has_issues = false;
        
        if (current_category == 2) {  // 2 = ANGRY
            strcat(local_advice, LV_SYMBOL_WARNING " CRITICAL ISSUES:\n");
        } else {
            strcat(local_advice, LV_SYMBOL_WARNING " ATTENTION NEEDED:\n");
        }
        
        // Check ammonia (most critical)
        if (current_mood_scores.ammonia_score < 0) {
            if (ammonia_ppm >= AMMONIA_CRITICAL) {
                strcat(local_advice, "• AMMONIA TOXIC (");
            } else {
                strcat(local_advice, "• Ammonia detected (");
            }
            char temp[32];
            snprintf(temp, sizeof(temp), "%.2f ppm)!\n", ammonia_ppm);
            strcat(local_advice, temp);
            has_issues = true;
        }
        
        // Check nitrite
        if (current_mood_scores.nitrite_score < 0) {
            if (nitrite_ppm >= NITRITE_CRITICAL) {
                strcat(local_advice, "• NITRITE TOXIC (");
            } else {
                strcat(local_advice, "• Nitrite detected (");
            }
            char temp[32];
            snprintf(temp, sizeof(temp), "%.2f ppm)!\n", nitrite_ppm);
            strcat(local_advice, temp);
            has_issues = true;
        }
        
        // Check nitrate
        if (current_mood_scores.nitrate_score < 0) {
            if (nitrate_ppm >= NITRATE_CRITICAL) {
                strcat(local_advice, "• Nitrate very high (");
            } else {
                strcat(local_advice, "• Nitrate high (");
            }
            char temp[32];
            snprintf(temp, sizeof(temp), "%.0f ppm)\n", nitrate_ppm);
            strcat(local_advice, temp);
            has_issues = true;
        }
        
        // Check pH
        if (current_mood_scores.ph_score < 0) {
            if (ph_level < PH_CRITICAL_LOW) {
                strcat(local_advice, "• pH TOO LOW (");
            } else if (ph_level > PH_CRITICAL_HIGH) {
                strcat(local_advice, "• pH TOO HIGH (");
            } else if (ph_level < PH_MIN_IDEAL) {
                strcat(local_advice, "• pH too acidic (");
            } else {
                strcat(local_advice, "• pH too alkaline (");
            }
            char temp[32];
            snprintf(temp, sizeof(temp), "%.1f)\n", ph_level);
            strcat(local_advice, temp);
            has_issues = true;
        }
        
        // Check feeding
        if (current_mood_scores.feed_score <= -1) {
            char temp[64];
            snprintf(temp, sizeof(temp), "• Fish hungry (%.0fh since feed)\n", hours_since_feed);
            strcat(local_advice, temp);
            has_issues = true;
        }
        
        // Check water change
        if (current_mood_scores.clean_score <= -1) {
            char temp[64];
            snprintf(temp, sizeof(temp), "• Water change overdue (%.0f days)\n", days_since_clean);
            strcat(local_advice, temp);
            has_issues = true;
        }
        
        // Add recommendations
        if (has_issues) {
            strcat(local_advice, "\nRECOMMENDED ACTIONS:\n");
            if (current_mood_scores.ammonia_score < 0 || current_mood_scores.nitrite_score < 0) {
                strcat(local_advice, "→ 50% water change NOW\n");
                strcat(local_advice, "→ Stop feeding temporarily\n");
            } else if (current_mood_scores.nitrate_score < 0) {
                strcat(local_advice, "→ Perform water change\n");
            }
            if (current_mood_scores.ph_score < 0) {
                strcat(local_advice, "→ Check pH and adjust\n");
            }
            if (current_mood_scores.feed_score <= -1) {
                strcat(local_advice, "→ Feed fish now\n");
            }
        }
        
        lv_label_set_text(ai_text_label, local_advice);
        ESP_LOGI(TAG, "AI Assistant: Showing emergency local analysis");
        return;  // Don't call API when showing urgent local advice
    }
    
    // HAPPY mood - Rate limit API calls to avoid quota exhaustion
    const uint32_t AI_UPDATE_INTERVAL = 300; // 5 minutes between API calls
    
    ESP_LOGI(TAG, "HAPPY mood detected - checking rate limit (last=%lu, interval=%lu)", 
             last_ai_update, AI_UPDATE_INTERVAL);
    
    if (last_ai_update > 0 && (current_time - last_ai_update) < AI_UPDATE_INTERVAL) {
        // Too soon since last API call, keep showing previous advice
        ESP_LOGW(TAG, "AI API call skipped (rate limited - wait %lu more seconds)", 
                 AI_UPDATE_INTERVAL - (current_time - last_ai_update));
        return;
    }
    
    ESP_LOGI(TAG, "Rate limit passed - proceeding with AI request");
    
    // STEP 4: Check WiFi status before sending request (avoid rate-limiting failed boot requests)
    if (!gemini_is_wifi_connected()) {
        ESP_LOGW(TAG, "WiFi not ready yet - skipping AI request (will retry when parameters change)");
        lv_label_set_text(ai_text_label, 
            LV_SYMBOL_OK " Tank is healthy!\n\n"
            "All parameters normal.\n"
            "Continue regular maintenance.\n\n"
            "(Waiting for WiFi...)");
        return;  // Don't set last_ai_update - allow immediate retry when WiFi is ready
    }
    
    // STEP 5: Send AI request to wifi_task (non-blocking)
    // Show loading message immediately
    lv_label_set_text(ai_text_label, LV_SYMBOL_REFRESH " Consulting AI...");
    
    // Count enabled feeds
    int feeds_count = 0;
    for (int i = 0; i < MAX_FEED_TIMES; i++) {
        if (planned_feed_times[i].enabled) feeds_count++;
    }
    
    // Package parameters into request message
    ai_request_msg_t request = {
        .ammonia_ppm = ammonia_ppm,
        .nitrite_ppm = nitrite_ppm,
        .nitrate_ppm = nitrate_ppm,
        .hours_since_feed = hours_since_feed,
        .days_since_clean = days_since_clean,
        .feeds_per_day = feeds_count,
        .water_change_interval = (int)planned_water_change_interval,
        .timestamp = current_time
    };
    
    // Send to wifi_task (non-blocking with overwrite for latest request)
    if (xQueueOverwrite(queue_ai_request, &request) == pdTRUE) {
        // Note: last_ai_update is set in ai_result_handler() on success only
        ESP_LOGI(TAG, "AI request sent to wifi_task (WiFi is ready)");
    } else {
        ESP_LOGW(TAG, "AI request queue full");
        // Fallback if queue fails (but tank is healthy)
        lv_label_set_text(ai_text_label, 
            LV_SYMBOL_OK " Tank is healthy!\n\n"
            "All parameters normal.\n"
            "Continue regular maintenance.\n\n"
            "(AI busy)");
    }
    // Result will be received by ai_result_handler() timer callback
}

/**
 * @brief Refresh weekly calendar activity dots
 */
static void refresh_weekly_calendar_dots(void) {
    time_t now_time = time(NULL);
    struct tm now_tm;
    localtime_r(&now_time, &now_tm);
    
    // Calculate today's start (midnight)
    struct tm today_tm = now_tm;
    today_tm.tm_hour = 0;
    today_tm.tm_min = 0;
    today_tm.tm_sec = 0;
    time_t today_start = mktime(&today_tm);
    
    for (int i = 0; i < 7; i++) {
        if (!week_day_boxes[i]) continue;
        
        // Calculate this day's date
        time_t day_time = now_time + ((i - 3) * 86400);
        struct tm day_tm;
        localtime_r(&day_time, &day_tm);
        int log_index = day_tm.tm_yday % LOG_DAYS;
        
        // Delete all child objects (dots and day name) except the first one (day name)
        uint32_t child_count = lv_obj_get_child_cnt(week_day_boxes[i]);
        while (child_count > 1) {
            lv_obj_del(lv_obj_get_child(week_day_boxes[i], child_count - 1));
            child_count--;
        }
        
        // Recreate dots
        int day_width = 55;
        int day_height = 60;
        
        // Check if water change is planned for this day
        bool water_planned = false;
        bool water_done = false;
        
        // Check if water was actually done on this specific day
        for (int j = 0; j < LOG_DAYS; j++) {
            if (water_change_log[j].timestamp == 0) continue;
            struct tm water_tm_buf;
            struct tm *water_tm = localtime_r(&water_change_log[j].timestamp, &water_tm_buf);
            if (water_tm->tm_yday == day_tm.tm_yday && water_tm->tm_year == day_tm.tm_year) {
                water_done = true;
                break;
            }
        }
        
        // Find the most recent water change
        time_t last_water_change_time = 0;
        for (int j = 0; j < LOG_DAYS; j++) {
            if (water_change_log[j].timestamp > last_water_change_time) {
                last_water_change_time = water_change_log[j].timestamp;
            }
        }
        
        // If we have a water change schedule, check if one is due on THIS SPECIFIC day
        if (planned_water_change_interval > 0) {
            if (last_water_change_time > 0) {
                // Calculate the next due date
                struct tm last_change_tm;
                localtime_r(&last_water_change_time, &last_change_tm);
                last_change_tm.tm_hour = 0;
                last_change_tm.tm_min = 0;
                last_change_tm.tm_sec = 0;
                time_t last_change_day = mktime(&last_change_tm);
                
                // Calculate next due date
                time_t next_due_date = last_change_day + (planned_water_change_interval * 86400);
                
                // Normalize day_time to start of day for comparison
                struct tm day_start_tm = day_tm;
                day_start_tm.tm_hour = 0;
                day_start_tm.tm_min = 0;
                day_start_tm.tm_sec = 0;
                time_t day_start = mktime(&day_start_tm);
                
                // Show hollow circle only on the exact next due date, or on today if overdue
                if (day_start == next_due_date) {
                    // Exact due date - show hollow circle
                    water_planned = true;
                } else if (day_start == today_start && today_start > next_due_date) {
                    // Today and overdue - show hollow circle on today only
                    water_planned = true;
                }
            } else if (day_time >= today_start) {
                // No water change recorded yet, show on today only
                struct tm today_tm = now_tm;
                today_tm.tm_hour = 0;
                today_tm.tm_min = 0;
                today_tm.tm_sec = 0;
                time_t today = mktime(&today_tm);
                
                struct tm day_start_tm = day_tm;
                day_start_tm.tm_hour = 0;
                day_start_tm.tm_min = 0;
                day_start_tm.tm_sec = 0;
                time_t day_start = mktime(&day_start_tm);
                
                if (day_start == today) {
                    water_planned = true;
                }
            }
        }
        
        // Draw water dot/circle - centered horizontally at bottom
        if (water_done) {
            // Solid blue dot - water change was done
            ESP_LOGI(TAG, "Creating solid water dot for day %d", i);
            lv_obj_t *water_dot = lv_obj_create(week_day_boxes[i]);
            lv_obj_set_size(water_dot, 6, 6);
            lv_obj_set_pos(water_dot, (day_width - 40) / 2, 25);
            lv_obj_set_style_bg_color(water_dot, lv_palette_main(LV_PALETTE_CYAN), 0);
            lv_obj_set_style_bg_opa(water_dot, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(water_dot, 0, 0);
            lv_obj_set_style_radius(water_dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_pad_all(water_dot, 0, 0);
            lv_obj_clear_flag(water_dot, LV_OBJ_FLAG_SCROLLABLE);
        } else if (water_planned) {
            // Hollow blue circle - water change is planned but not done
            ESP_LOGI(TAG, "Creating hollow water circle for day %d", i);
            lv_obj_t *water_dot = lv_obj_create(week_day_boxes[i]);
            lv_obj_set_size(water_dot, 6, 6);
            lv_obj_set_pos(water_dot, (day_width - 40) / 2, 25);
            lv_obj_set_style_bg_opa(water_dot, LV_OPA_TRANSP, 0);  // Transparent background
            lv_obj_set_style_border_color(water_dot, lv_palette_main(LV_PALETTE_CYAN), 0);
            lv_obj_set_style_border_width(water_dot, 1, 0);  // 1px border
            lv_obj_set_style_radius(water_dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_pad_all(water_dot, 0, 0);
            lv_obj_clear_flag(water_dot, LV_OBJ_FLAG_SCROLLABLE);
        }
        
        // Check planned feeds for this day
        int planned_feed_count = 0;
        for (int j = 0; j < MAX_FEED_TIMES; j++) {
            if (planned_feed_times[j].enabled) {
                planned_feed_count++;
            }
        }
        
        // Count actual logged feeds for this day
        int logged_feed_count = 0;
        for (int j = 0; j < LOG_DAYS; j++) {
            if (feed_log_data[j].timestamp == 0) continue;
            struct tm feed_tm_buf;
            struct tm *feed_tm = localtime_r(&feed_log_data[j].timestamp, &feed_tm_buf);
            if (feed_tm->tm_yday == day_tm.tm_yday && feed_tm->tm_year == day_tm.tm_year) {
                logged_feed_count++;
            }
        }
        
        // Red feed dots/circles - arranged horizontally at top
        int total_feeds_to_show = (logged_feed_count > planned_feed_count) ? logged_feed_count : planned_feed_count;
        if (total_feeds_to_show > 4) total_feeds_to_show = 4;
        
        if (total_feeds_to_show > 0) {
            // Calculate total width and center the row
            int total_dots_width = (total_feeds_to_show * 6) + ((total_feeds_to_show - 1) * 2);
            int start_x = (day_width - total_dots_width) / 2 - 17.375;
            ESP_LOGI(TAG, "Feed dots day %d: planned=%d, logged=%d, showing=%d", i, planned_feed_count, logged_feed_count, total_feeds_to_show);
            
            for (int j = 0; j < total_feeds_to_show; j++) {
                lv_obj_t *feed_dot = lv_obj_create(week_day_boxes[i]);
                lv_obj_set_size(feed_dot, 6, 6);
                lv_obj_set_pos(feed_dot, start_x + (j * 8), -5);
                
                if (j < logged_feed_count) {
                    // Solid red dot - feed was logged
                    lv_obj_set_style_bg_color(feed_dot, lv_palette_main(LV_PALETTE_RED), 0);
                    lv_obj_set_style_bg_opa(feed_dot, LV_OPA_COVER, 0);
                    lv_obj_set_style_border_width(feed_dot, 0, 0);
                } else {
                    // Hollow red circle - feed is planned but not logged
                    lv_obj_set_style_bg_opa(feed_dot, LV_OPA_TRANSP, 0);  // Transparent background
                    lv_obj_set_style_border_color(feed_dot, lv_palette_main(LV_PALETTE_RED), 0);
                    lv_obj_set_style_border_width(feed_dot, 1, 0);  // 1px border
                }
                lv_obj_set_style_radius(feed_dot, LV_RADIUS_CIRCLE, 0);
                lv_obj_set_style_pad_all(feed_dot, 0, 0);
                lv_obj_clear_flag(feed_dot, LV_OBJ_FLAG_SCROLLABLE);
            }
        }
    }
}

/**
 * @brief Main screen button event handler (Feed/Water buttons)
 */
static void main_button_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *btn = lv_event_get_target(e);
    
    if (code == LV_EVENT_CLICKED) {
        // Get current day index based on day-of-year
        time_t now = time(NULL);
        struct tm now_tm;
        localtime_r(&now, &now_tm);
        int today_index = now_tm.tm_yday % LOG_DAYS;
        
        if (btn == btn_feed_main) {
            // Log feed event with timestamp
            feed_log[today_index]++;
            last_feed_time = get_current_time_seconds();
            
            // Save to feed_log_data with timestamp
            for (int i = LOG_DAYS - 1; i > 0; i--) {
                feed_log_data[i] = feed_log_data[i - 1];
            }
            feed_log_data[0].timestamp = time(NULL);
            feed_log_data[0].feeds_per_day = 1;  // 1 click
            
            ESP_LOGI(TAG, "Feed logged - Day index %d: %lu feeds", today_index, feed_log[today_index]);
            
            // Save to SD card
            save_feed_to_sd(feed_log_data[0].feeds_per_day);
            
            // Re-evaluate mood and update button colors
            evaluate_and_update_mood();
            update_ai_assistant();
            
            // Refresh weekly calendar dots
            refresh_weekly_calendar_dots();
            
        } else if (btn == btn_water_main) {
            // Log water cleaning event with timestamp
            water_log[today_index]++;
            last_clean_time = get_current_time_seconds();
            
            // Save to water_change_log with timestamp
            for (int i = LOG_DAYS - 1; i > 0; i--) {
                water_change_log[i] = water_change_log[i - 1];
            }
            water_change_log[0].timestamp = time(NULL);
            water_change_log[0].interval_days = 1;  // 1 click
            
            ESP_LOGI(TAG, "Water cleaned - Day index %d: %lu cleanings", today_index, water_log[today_index]);
            
            // Save to SD card
            save_water_change_to_sd(water_change_log[0].interval_days);
            
            // Re-evaluate mood and update button colors
            evaluate_and_update_mood();
            update_ai_assistant();
            
            // Refresh weekly calendar dots
            refresh_weekly_calendar_dots();
        }
    }
}

/**
 * @brief Close any active popup
 */
static void close_popup(void) {
    if (popup_param) { lv_obj_del(popup_param); popup_param = NULL; }
    if (popup_water) { lv_obj_del(popup_water); popup_water = NULL; }
    if (popup_feed) { lv_obj_del(popup_feed); popup_feed = NULL; }
    if (popup_history) { lv_obj_del(popup_history); popup_history = NULL; }
    if (popup_keypad) { lv_obj_del(popup_keypad); popup_keypad = NULL; }
    if (popup_monthly_cal) { lv_obj_del(popup_monthly_cal); popup_monthly_cal = NULL; }
    if (popup_med_calc) { lv_obj_del(popup_med_calc); popup_med_calc = NULL; }
    active_input_field = NULL;
}

/**
 * @brief Calculate medication dosage based on current state
 */
static void calculate_medication_dosage(void) {
    // Get input values
    const char *amount_text = lv_textarea_get_text(med_product_amount_input);
    const char *per_volume_text = lv_textarea_get_text(med_per_volume_input);
    const char *tank_size_text = lv_textarea_get_text(med_tank_size_input);
    
    med_calc_state.product_amount = atof(amount_text);
    med_calc_state.per_volume = atof(per_volume_text);
    med_calc_state.tank_size = atof(tank_size_text);
    med_calc_state.unit_type = lv_dropdown_get_selected(med_unit_dropdown);
    
    // Validate inputs
    if (med_calc_state.product_amount <= 0 || med_calc_state.per_volume <= 0 || med_calc_state.tank_size <= 0) {
        lv_label_set_text(med_result_label, "❌ Invalid input!\nAll values must be positive numbers.");
        return;
    }
    
    // Get unit strings
    const char *unit_names[] = {"ml", "tsp", "tbsp", "drops", "fl oz", "cups", "g"};
    const char *per_unit_str = med_calc_state.is_gallons ? "gal" : "L";
    const char *tank_unit_str = med_calc_state.tank_is_gallons ? "gal" : "L";
    const char *dose_unit = unit_names[med_calc_state.unit_type];
    
    // Convert product amount to ml for calculation
    float product_amount_ml = med_calc_state.product_amount;
    switch (med_calc_state.unit_type) {
        case 0: break;                              // ml - no conversion
        case 1: product_amount_ml *= 5.0; break;    // tsp to ml
        case 2: product_amount_ml *= 15.0; break;   // tbsp to ml
        case 3: product_amount_ml *= 0.05; break;   // drops to ml (1 drop ≈ 0.05ml)
        case 4: product_amount_ml *= 29.5735; break; // fl oz to ml
        case 5: product_amount_ml *= 236.588; break; // cups to ml (US cup)
        case 6: product_amount_ml *= 1.0; break;    // grams to ml (assuming 1:1 for liquids)
        default: break;
    }
    
    // Convert volumes to same unit (liters) for calculation
    float per_volume_l = med_calc_state.per_volume;
    if (med_calc_state.is_gallons) per_volume_l *= 3.78541;  // gallons to liters
    
    float tank_size_l = med_calc_state.tank_size;
    if (med_calc_state.tank_is_gallons) tank_size_l *= 3.78541;  // gallons to liters
    
    // Universal formula: (tank_size / per_volume) * product_amount
    float dosage_ml = (tank_size_l / per_volume_l) * product_amount_ml;
    med_calc_state.calculated_dosage = dosage_ml;
    
    // Calculate alternative measurements
    float dosage_tsp = dosage_ml / 5.0;
    float dosage_tbsp = dosage_ml / 15.0;
    float dosage_drops = dosage_ml / 0.05;
    float dosage_floz = dosage_ml / 29.5735;
    
    // Format result text
    snprintf(med_calc_state.result_text, sizeof(med_calc_state.result_text),
             "✅ Total Dosage for %.1f %s tank:\n\n"
             "Based on: %.1f %s per %.1f %s\n\n"
             "Add to tank:\n"
             "🧪 %.2f ml\n"
             "🥄 %.2f tsp\n"
             "🥄 %.2f tbsp\n"
             "💧 %.0f drops\n"
             "🧴 %.2f fl oz",
             med_calc_state.tank_size, tank_unit_str,
             med_calc_state.product_amount, dose_unit, med_calc_state.per_volume, per_unit_str,
             dosage_ml, dosage_tsp, dosage_tbsp, dosage_drops, dosage_floz);
    
    // Update result label in popup
    lv_label_set_text(med_result_label, med_calc_state.result_text);
    
    // Store for AI integration
    snprintf(latest_med_calculation, sizeof(latest_med_calculation),
             "UNIVERSAL DOSAGE CALCULATION:\n"
             "- Product: %.1f %s per %.1f %s\n"
             "- Tank Size: %.1f %s\n"
             "- Total Dosage: %.2f ml (%.2f tsp)\n",
             med_calc_state.product_amount, dose_unit, med_calc_state.per_volume, per_unit_str,
             med_calc_state.tank_size, tank_unit_str,
             dosage_ml, dosage_tsp);
    
    // Update AI screen display if it exists
    if (ai_med_result_label) {
        char ai_display[256];
        snprintf(ai_display, sizeof(ai_display),
                 "💊 Dosage Calc: %.1f%s/%.1f%s\n"
                 "Tank %.1f%s → Add %.2f ml",
                 med_calc_state.product_amount, dose_unit, med_calc_state.per_volume, per_unit_str,
                 med_calc_state.tank_size, tank_unit_str, dosage_ml);
        lv_label_set_text(ai_med_result_label, ai_display);
    }
    
    ESP_LOGI(TAG, "Universal dosage calculated: %.1f %s per %.1f %s for %.1f %s = %.2f ml",
             med_calc_state.product_amount, dose_unit, med_calc_state.per_volume, per_unit_str,
             med_calc_state.tank_size, tank_unit_str, dosage_ml);
    
    // Save to SD card
    save_medication_to_sd();
    
    // Trigger AI update with new medication context
    update_ai_assistant();
}

/**
 * @brief Close calculator popup event callback
 */
static void med_calc_close_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (popup_med_calc) {
            lv_obj_del(popup_med_calc);
            popup_med_calc = NULL;
        }
    }
}

/**
 * @brief Calculate button event callback
 */
static void med_calc_calculate_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        calculate_medication_dosage();
    }
}

/**
 * @brief Unit toggle switch event callback for "Per" field
 */
static void med_unit_switch_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *sw = lv_event_get_target(e);
        med_calc_state.is_gallons = lv_obj_has_state(sw, LV_STATE_CHECKED);
        ESP_LOGI(TAG, "Per field unit switched to: %s", med_calc_state.is_gallons ? "Gallons" : "Liters");
    }
}

/**
 * @brief Tank unit toggle switch event callback
 */
static void med_tank_unit_switch_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *sw = lv_event_get_target(e);
        med_calc_state.tank_is_gallons = lv_obj_has_state(sw, LV_STATE_CHECKED);
        ESP_LOGI(TAG, "Tank size unit switched to: %s", med_calc_state.tank_is_gallons ? "Gallons" : "Liters");
    }
}

/**
 * @brief Medication type dropdown event callback
 */
// Removed - no longer needed for Universal calculator
// static void med_type_dropdown_event_cb(lv_event_t *e) {
//     if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
//         lv_obj_t *dropdown = lv_event_get_target(e);
//         med_calc_state.med_type = (medication_type_t)lv_dropdown_get_selected(dropdown);
//         ESP_LOGI(TAG, "Medication type changed to: %s",
//                  medication_database[med_calc_state.med_type].name);
//     }
// }

/**
 * @brief Show medication calculator popup
 */
/**
 * @brief Show medication calculator popup
 */
static void show_med_calculator_popup(void) {
    // Close any existing popups
    close_popup();
    
    // Create full-screen popup overlay
    popup_med_calc = lv_obj_create(scroll_container);
    lv_obj_set_size(popup_med_calc, 440, 300);
    lv_obj_set_pos(popup_med_calc, 20, 490);  // Y=490 (calendar panel area)
    lv_obj_set_style_bg_color(popup_med_calc, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_width(popup_med_calc, 3, 0);
    lv_obj_set_style_border_color(popup_med_calc, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_radius(popup_med_calc, 10, 0);
    lv_obj_set_scrollbar_mode(popup_med_calc, LV_SCROLLBAR_MODE_AUTO);  // Enable scrollbar
    lv_obj_set_scroll_dir(popup_med_calc, LV_DIR_VER);  // Vertical scrolling
    
    // Title
    lv_obj_t *title = lv_label_create(popup_med_calc);
    lv_label_set_text(title, "💊 Universal Dosage Calculator");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_pos(title, 10, 8);
    
    // Row 1: Amount of Product
    lv_obj_t *amount_label = lv_label_create(popup_med_calc);
    lv_label_set_text(amount_label, "Amount:");
    lv_obj_set_style_text_color(amount_label, lv_color_white(), 0);
    lv_obj_set_pos(amount_label, 20, 45);
    
    med_product_amount_input = lv_textarea_create(popup_med_calc);
    lv_obj_set_size(med_product_amount_input, 80, 35);
    lv_obj_set_pos(med_product_amount_input, 100, 40);
    lv_textarea_set_one_line(med_product_amount_input, true);
    lv_textarea_set_text(med_product_amount_input, "5");
    lv_obj_add_event_cb(med_product_amount_input, input_field_event_cb, LV_EVENT_CLICKED, NULL);
    
    // Unit dropdown (ml, tsp, tbsp, drops, fl oz, cups, g)
    med_unit_dropdown = lv_dropdown_create(popup_med_calc);
    lv_obj_set_size(med_unit_dropdown, 80, 35);
    lv_obj_set_pos(med_unit_dropdown, 195, 40);
    lv_dropdown_set_options(med_unit_dropdown, "ml\ntsp\ntbsp\ndrops\nfl oz\ncups\ng");
    
    // Row 2: Per X gallons/litres
    lv_obj_t *per_label = lv_label_create(popup_med_calc);
    lv_label_set_text(per_label, "Per:");
    lv_obj_set_style_text_color(per_label, lv_color_white(), 0);
    lv_obj_set_pos(per_label, 20, 90);
    
    med_per_volume_input = lv_textarea_create(popup_med_calc);
    lv_obj_set_size(med_per_volume_input, 80, 35);
    lv_obj_set_pos(med_per_volume_input, 100, 85);
    lv_textarea_set_one_line(med_per_volume_input, true);
    lv_textarea_set_text(med_per_volume_input, "10");
    lv_obj_add_event_cb(med_per_volume_input, input_field_event_cb, LV_EVENT_CLICKED, NULL);
    
    // Unit toggle (L/Gal)
    lv_obj_t *unit_label_l = lv_label_create(popup_med_calc);
    lv_label_set_text(unit_label_l, "L");
    lv_obj_set_style_text_color(unit_label_l, lv_color_white(), 0);
    lv_obj_set_pos(unit_label_l, 195, 92);
    
    med_unit_switch = lv_switch_create(popup_med_calc);
    lv_obj_set_pos(med_unit_switch, 220, 88);
    lv_obj_add_event_cb(med_unit_switch, med_unit_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    
    lv_obj_t *unit_label_g = lv_label_create(popup_med_calc);
    lv_label_set_text(unit_label_g, "Gal");
    lv_obj_set_style_text_color(unit_label_g, lv_color_white(), 0);
    lv_obj_set_pos(unit_label_g, 275, 92);
    
    // Row 3: Tank Size with L/Gal toggle
    lv_obj_t *tank_label = lv_label_create(popup_med_calc);
    lv_label_set_text(tank_label, "Tank Size:");
    lv_obj_set_style_text_color(tank_label, lv_color_white(), 0);
    lv_obj_set_pos(tank_label, 20, 135);
    
    med_tank_size_input = lv_textarea_create(popup_med_calc);
    lv_obj_set_size(med_tank_size_input, 80, 35);
    lv_obj_set_pos(med_tank_size_input, 120, 130);
    lv_textarea_set_one_line(med_tank_size_input, true);
    lv_textarea_set_text(med_tank_size_input, "50");
    lv_obj_add_event_cb(med_tank_size_input, input_field_event_cb, LV_EVENT_CLICKED, NULL);
    
    // Tank Size Unit toggle (L/Gal)
    lv_obj_t *tank_unit_label_l = lv_label_create(popup_med_calc);
    lv_label_set_text(tank_unit_label_l, "L");
    lv_obj_set_style_text_color(tank_unit_label_l, lv_color_white(), 0);
    lv_obj_set_pos(tank_unit_label_l, 215, 137);
    
    med_tank_unit_switch = lv_switch_create(popup_med_calc);
    lv_obj_set_pos(med_tank_unit_switch, 240, 133);
    lv_obj_add_event_cb(med_tank_unit_switch, med_tank_unit_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    
    lv_obj_t *tank_unit_label_g = lv_label_create(popup_med_calc);
    lv_label_set_text(tank_unit_label_g, "Gal");
    lv_obj_set_style_text_color(tank_unit_label_g, lv_color_white(), 0);
    lv_obj_set_pos(tank_unit_label_g, 295, 137);
    
    // Calculate button
    lv_obj_t *btn_calc = lv_btn_create(popup_med_calc);
    lv_obj_set_size(btn_calc, 120, 40);
    lv_obj_set_pos(btn_calc, 20, 185);
    lv_obj_set_style_bg_color(btn_calc, lv_color_hex(0x00aa00), 0);
    lv_obj_add_event_cb(btn_calc, med_calc_calculate_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *calc_lbl = lv_label_create(btn_calc);
    lv_label_set_text(calc_lbl, "Calculate");
    lv_obj_center(calc_lbl);
    
    // Close button - moved next to Calculate button
    lv_obj_t *btn_close = lv_btn_create(popup_med_calc);
    lv_obj_set_size(btn_close, 80, 40);
    lv_obj_set_pos(btn_close, 155, 185);
    lv_obj_set_style_bg_color(btn_close, lv_color_hex(0xff0000), 0);
    lv_obj_add_event_cb(btn_close, med_calc_close_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_lbl = lv_label_create(btn_close);
    lv_label_set_text(close_lbl, "Close");
    lv_obj_center(close_lbl);
    
    // Result display - positioned below buttons, extends beyond viewport to enable scrolling
    med_result_label = lv_label_create(popup_med_calc);
    lv_obj_set_size(med_result_label, 400, 200);
    lv_obj_set_pos(med_result_label, 20, 240);
    lv_label_set_long_mode(med_result_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(med_result_label, lv_color_hex(0x00ff00), 0);
    lv_label_set_text(med_result_label, "Enter values and click Calculate.");
    
    ESP_LOGI(TAG, "Universal dosage calculator popup opened on calendar page");
}

/**
 * @brief Decimal keypad button event callback
 */
static void keypad_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) return;
    
    lv_obj_t *btn = lv_event_get_target(e);
    const char *txt = lv_btnmatrix_get_btn_text(btn, lv_btnmatrix_get_selected_btn(btn));
    
    if (!active_input_field) return;
    
    // Get the display textarea from keypad container
    lv_obj_t *keypad_cont = lv_obj_get_parent(btn);
    lv_obj_t *display = (lv_obj_t *)lv_obj_get_user_data(keypad_cont);
    
    if (strcmp(txt, "OK") == 0) {
        // Copy display value to input field and close
        if (display) {
            const char *val = lv_textarea_get_text(display);
            lv_textarea_set_text(active_input_field, val);
        }
        if (popup_keypad) { lv_obj_del(popup_keypad); popup_keypad = NULL; }
        active_input_field = NULL;
    } else if (strcmp(txt, "DEL") == 0) {
        if (display) lv_textarea_del_char(display);
    } else if (strcmp(txt, "CLR") == 0) {
        if (display) lv_textarea_set_text(display, "0");
    } else {
        // Add digit/decimal to display
        if (display) {
            const char *current = lv_textarea_get_text(display);
            if (strcmp(current, "0") == 0) {
                // Replace leading zero
                lv_textarea_set_text(display, txt);
            } else {
                lv_textarea_add_text(display, txt);
            }
        }
    }
}

/**
 * @brief Show decimal keypad for input field
 */
static void show_keypad(lv_obj_t *input_field) {
    if (popup_keypad) return;
    active_input_field = input_field;
    
    // Clear the input field when keypad opens (replace instead of append)
    lv_textarea_set_text(active_input_field, "");
    
    // Check if input field is inside medication calculator popup
    bool is_med_calc_input = false;
    lv_obj_t *parent = lv_obj_get_parent(input_field);
    while (parent != NULL) {
        if (parent == popup_med_calc) {
            is_med_calc_input = true;
            break;
        }
        parent = lv_obj_get_parent(parent);
    }
    
    // Create keypad in appropriate container
    if (is_med_calc_input && popup_med_calc) {
        // Create keypad inside medication calculator popup
        popup_keypad = lv_obj_create(popup_med_calc);
        lv_obj_set_size(popup_keypad, 440, 300);  // Match popup_med_calc size
        lv_obj_set_pos(popup_keypad, 0, 0);  // Fill entire popup
    } else {
        // Create keypad inside panel_content (for other inputs)
        popup_keypad = lv_obj_create(panel_content);
        lv_obj_set_size(popup_keypad, 440, 280);  // Match panel_content size
        lv_obj_set_pos(popup_keypad, 0, 0);  // Fill entire panel_content
    }
    
    lv_obj_set_style_bg_color(popup_keypad, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(popup_keypad, LV_OPA_80, 0);
    lv_obj_set_style_border_width(popup_keypad, 0, 0);
    lv_obj_clear_flag(popup_keypad, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(popup_keypad, 0, 0);  // Remove padding
    
    lv_obj_t *keypad_cont = lv_obj_create(popup_keypad);
    lv_obj_set_size(keypad_cont, 300, 260);
    lv_obj_set_pos(keypad_cont, 70, 20);  // Centered: (440-300)/2=70, (300-260)/2=20
    lv_obj_set_style_bg_color(keypad_cont, lv_color_hex(0x2a2a2a), 0);
    
    // Add display area at top showing current value
    lv_obj_t *display = lv_textarea_create(keypad_cont);
    lv_obj_set_size(display, 280, 40);
    lv_obj_set_pos(display, 10, 5);
    lv_textarea_set_text(display, "0");
    lv_textarea_set_one_line(display, true);
    lv_obj_set_style_text_font(display, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(display, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_clear_flag(display, LV_OBJ_FLAG_CLICKABLE);  // Read-only display
    lv_obj_set_user_data(keypad_cont, display);  // Store display reference
    
    static const char *keypad_map[] = {
        "1", "2", "3", "\n",
        "4", "5", "6", "\n",
        "7", "8", "9", "\n",
        ".", "0", "DEL", "\n",
        "CLR", "OK", ""
    };
    
    lv_obj_t *btnm = lv_btnmatrix_create(keypad_cont);
    lv_btnmatrix_set_map(btnm, keypad_map);
    lv_obj_set_size(btnm, 280, 200);
    lv_obj_set_pos(btnm, 10, 50);
    lv_obj_add_event_cb(btnm, keypad_event_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_move_foreground(popup_keypad);
}

/**
 * @brief Input field click event - shows keypad
 */
static void input_field_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        show_keypad(lv_event_get_target(e));
    }
}

/**
 * @brief Save parameter log entry
 */
static void save_param_log_cb(lv_event_t *e) {
    // Read values from input fields (5 fields: NH3, NO3, NO2, pH, pH_unused)
    lv_obj_t *inputs[5];
    int input_idx = 0;
    
    // Find all textarea children in popup_param
    uint32_t child_count = lv_obj_get_child_cnt(popup_param);
    for (uint32_t i = 0; i < child_count && input_idx < 5; i++) {
        lv_obj_t *child = lv_obj_get_child(popup_param, i);
        if (lv_obj_check_type(child, &lv_textarea_class)) {
            inputs[input_idx++] = child;
        }
    }
    
    if (input_idx == 5) {
        // Read values from textareas
        float ammonia_val = atof(lv_textarea_get_text(inputs[0]));
        float nitrate_val = atof(lv_textarea_get_text(inputs[1]));
        float nitrite_val = atof(lv_textarea_get_text(inputs[2]));
        float ph_val = atof(lv_textarea_get_text(inputs[3]));
        
        // Update dashboard with new values
        dashboard_update_ammonia(ammonia_val);
        dashboard_update_nitrate(nitrate_val);
        dashboard_update_nitrite(nitrite_val);
        dashboard_update_ph(ph_val);
        
        // Shift existing log entries down
        for (int i = LOG_DAYS - 1; i > 0; i--) {
            param_log[i] = param_log[i - 1];
        }
        
        // Save new entry at index 0 (most recent)
        param_log[0].timestamp = time(NULL);
        param_log[0].ammonia = ammonia_val;
        param_log[0].nitrate = nitrate_val;
        param_log[0].nitrite = nitrite_val;
        param_log[0].high_ph = ph_val;
        param_log[0].low_ph = ph_val;
        
        ESP_LOGI(TAG, "Parameters saved: NH3=%.2f, NO3=%.1f, NO2=%.2f, pH=%.1f",
                ammonia_val, nitrate_val, nitrite_val, ph_val);
        
        // Save to SD card
        save_parameters_to_sd(ammonia_val, nitrate_val, nitrite_val, ph_val);
    }
    
    close_popup();
}

/**
 * @brief Show day history popup - all activities for a specific day
 */
static void show_day_history(time_t target_date) {
    if (popup_history) return;
    
    popup_history = lv_obj_create(panel_content);
    lv_obj_set_size(popup_history, 450, 400);
    lv_obj_center(popup_history);
    lv_obj_set_style_bg_color(popup_history, lv_color_hex(0x1a1a1a), 0);
    
    // Get target day info
    struct tm target_tm;
    localtime_r(&target_date, &target_tm);
    int target_day = target_tm.tm_yday;
    int target_year = target_tm.tm_year;
    
    char title_text[64];
    strftime(title_text, sizeof(title_text), "Activity - %d %b %Y", &target_tm);
    lv_obj_t *title = lv_label_create(popup_history);
    lv_label_set_text(title, title_text);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    
    // Section 1: Activity Log (Left side)
    lv_obj_t *section1_title = lv_label_create(popup_history);
    lv_label_set_text(section1_title, "Activity Log");
    lv_obj_set_style_text_font(section1_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(section1_title, lv_palette_main(LV_PALETTE_CYAN), 0);
    lv_obj_set_pos(section1_title, 10, 50);
    
    lv_obj_t *list = lv_list_create(popup_history);
    lv_obj_set_size(list, 210, 125);
    lv_obj_set_pos(list, 10, 75);
    
    // Show all activities for this day
    bool has_activity = false;
    
    // Check feed log - search all entries for matching day
    for (int i = 0; i < LOG_DAYS; i++) {
        if (feed_log_data[i].timestamp == 0) continue;
        struct tm feed_tm_buf;
        struct tm *feed_tm = localtime_r(&feed_log_data[i].timestamp, &feed_tm_buf);
        if (feed_tm->tm_yday == target_day && feed_tm->tm_year == target_year) {
            char entry[128];
            snprintf(entry, sizeof(entry), "%02d:%02d - Fed",
                     feed_tm->tm_hour, feed_tm->tm_min);
            lv_list_add_text(list, entry);
            has_activity = true;
        }
    }
    
    // Check water log - search all entries for matching day
    for (int i = 0; i < LOG_DAYS; i++) {
        if (water_change_log[i].timestamp == 0) continue;
        struct tm water_tm_buf;
        struct tm *water_tm = localtime_r(&water_change_log[i].timestamp, &water_tm_buf);
        if (water_tm->tm_yday == target_day && water_tm->tm_year == target_year) {
            char entry[128];
            snprintf(entry, sizeof(entry), "%02d:%02d - Water change",
                     water_tm->tm_hour, water_tm->tm_min);
            lv_list_add_text(list, entry);
            has_activity = true;
        }
    }
    
    // Check parameter log - search all entries for matching day
    for (int i = 0; i < LOG_DAYS; i++) {
        if (param_log[i].timestamp == 0) continue;
        struct tm param_tm_buf;
        struct tm *param_tm = localtime_r(&param_log[i].timestamp, &param_tm_buf);
        if (param_tm->tm_yday == target_day && param_tm->tm_year == target_year) {
            char entry[256];
            snprintf(entry, sizeof(entry), 
                     "%02d:%02d - Parameters: NH3:%.2f NO3:%.2f NO2:%.2f pH:%.1f-%.1f",
                     param_tm->tm_hour, param_tm->tm_min,
                     param_log[i].ammonia, param_log[i].nitrate, param_log[i].nitrite,
                     param_log[i].low_ph, param_log[i].high_ph);
            lv_list_add_text(list, entry);
            has_activity = true;
        }
    }
    
    if (!has_activity) {
        lv_list_add_text(list, "No activity recorded for this day");
    }
    
    // Section 2: Planned Activity (Right side - only show for today and future days)
    time_t now = time(NULL);
    struct tm now_tm;
    localtime_r(&now, &now_tm);
    now_tm.tm_hour = 0;
    now_tm.tm_min = 0;
    now_tm.tm_sec = 0;
    time_t today_start = mktime(&now_tm);
    
    // Only show planned activity for today or future dates
    if (target_date >= today_start) {
        lv_obj_t *section2_title = lv_label_create(popup_history);
        lv_label_set_text(section2_title, "Planned Activity");
        lv_obj_set_style_text_font(section2_title, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(section2_title, lv_palette_main(LV_PALETTE_ORANGE), 0);
        lv_obj_set_pos(section2_title, 230, 50);
        
        lv_obj_t *plan_list = lv_list_create(popup_history);
        lv_obj_set_size(plan_list, 210, 125);
        lv_obj_set_pos(plan_list, 230, 75);
        
        // Show feed schedule from stored configuration
        lv_list_add_text(plan_list, "Feed Schedule:");
        bool has_feed_schedule = false;
        for (int i = 0; i < MAX_FEED_TIMES; i++) {
            if (planned_feed_times[i].enabled) {
                char feed_entry[64];
                snprintf(feed_entry, sizeof(feed_entry), "  %02d:%02d - Feed time", 
                         planned_feed_times[i].hour, planned_feed_times[i].minute);
                lv_list_add_text(plan_list, feed_entry);
                has_feed_schedule = true;
            }
        }
        if (!has_feed_schedule) {
            lv_list_add_text(plan_list, "  No feed schedule configured");
        }
        
        // Show water change schedule based on most recent change and planned interval
        // Find the most recent water change
        time_t last_water_change_time = 0;
        for (int i = 0; i < LOG_DAYS; i++) {
            if (water_change_log[i].timestamp > last_water_change_time) {
                last_water_change_time = water_change_log[i].timestamp;
            }
        }
        
        lv_list_add_text(plan_list, "");
        if (planned_water_change_interval > 0) {
            if (last_water_change_time > 0) {
                // Calculate next due date
                struct tm last_change_tm;
                localtime_r(&last_water_change_time, &last_change_tm);
                last_change_tm.tm_hour = 0;
                last_change_tm.tm_min = 0;
                last_change_tm.tm_sec = 0;
                time_t last_change_day = mktime(&last_change_tm);
                time_t next_due_date = last_change_day + (planned_water_change_interval * 86400);
                
                // Normalize target_date to start of day
                struct tm target_day_tm = target_tm;
                target_day_tm.tm_hour = 0;
                target_day_tm.tm_min = 0;
                target_day_tm.tm_sec = 0;
                time_t target_day_start = mktime(&target_day_tm);
                
                if (target_day_start == next_due_date) {
                    lv_list_add_text(plan_list, "Water change scheduled today");
                } else {
                    int days_diff = (next_due_date - target_day_start) / 86400;
                    if (days_diff > 0) {
                        char clean_info[64];
                        snprintf(clean_info, sizeof(clean_info), "Next water change in %d days", days_diff);
                        lv_list_add_text(plan_list, clean_info);
                    } else if (days_diff < 0) {
                        char clean_info[64];
                        snprintf(clean_info, sizeof(clean_info), "Water change overdue by %d days", -days_diff);
                        lv_list_add_text(plan_list, clean_info);
                    }
                }
            } else {
                // No water change recorded yet
                lv_list_add_text(plan_list, "Water change scheduled");
            }
        } else {
            lv_list_add_text(plan_list, "No water change schedule");
        }
    }
    
    lv_obj_t *btn_close = lv_btn_create(popup_history);
    lv_obj_set_size(btn_close, 100, 40);
    lv_obj_align(btn_close, LV_ALIGN_BOTTOM_MID, 0, -80);
    lv_obj_t *label = lv_label_create(btn_close);
    lv_label_set_text(label, "Close");
    lv_obj_center(label);
    lv_obj_add_event_cb(btn_close, [](lv_event_t *e) {
        if (popup_history) { lv_obj_del(popup_history); popup_history = NULL; }
    }, LV_EVENT_CLICKED, NULL);
    
    lv_obj_move_foreground(popup_history);
}

/**
 * @brief Show parameter log history
 */
static void show_param_history_cb(lv_event_t *e) {
    if (popup_history) return;
    
    popup_history = lv_obj_create(panel_content);
    lv_obj_set_size(popup_history, 450, 300);
    lv_obj_center(popup_history);
    lv_obj_set_style_bg_color(popup_history, lv_color_hex(0x1a1a1a), 0);
    
    lv_obj_t *title = lv_label_create(popup_history);
    lv_label_set_text(title, "Parameter History (7 Days)");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    
    lv_obj_t *list = lv_list_create(popup_history);
    lv_obj_set_size(list, 430, 220);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 40);
    
    for (int i = 0; i < LOG_DAYS; i++) {
        if (param_log[i].timestamp == 0) continue;
        
        char entry[256];
        struct tm *timeinfo = localtime(&param_log[i].timestamp);
        snprintf(entry, sizeof(entry), 
                 "%02d/%02d %02d:%02d - NH3:%.2f NO3:%.2f NO2:%.2f pH:%.1f-%.1f",
                 timeinfo->tm_mon + 1, timeinfo->tm_mday,
                 timeinfo->tm_hour, timeinfo->tm_min,
                 param_log[i].ammonia, param_log[i].nitrate, param_log[i].nitrite,
                 param_log[i].low_ph, param_log[i].high_ph);
        lv_list_add_text(list, entry);
    }
    
    lv_obj_t *btn_close = lv_btn_create(popup_history);
    lv_obj_set_size(btn_close, 100, 40);
    lv_obj_align(btn_close, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_t *label = lv_label_create(btn_close);
    lv_label_set_text(label, "Close");
    lv_obj_center(label);
    lv_obj_add_event_cb(btn_close, [](lv_event_t *e) {
        if (popup_history) { lv_obj_del(popup_history); popup_history = NULL; }
    }, LV_EVENT_CLICKED, NULL);
    
    lv_obj_move_foreground(popup_history);
}

/**
 * @brief Show water change log history
 */
static void show_water_history_cb(lv_event_t *e) {
    if (popup_history) return;
    
    popup_history = lv_obj_create(panel_content);
    lv_obj_set_size(popup_history, 450, 300);
    lv_obj_center(popup_history);
    lv_obj_set_style_bg_color(popup_history, lv_color_hex(0x1a1a1a), 0);
    
    lv_obj_t *title = lv_label_create(popup_history);
    lv_label_set_text(title, "Water Button History (7 Days)");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    
    lv_obj_t *list = lv_list_create(popup_history);
    lv_obj_set_size(list, 430, 220);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 40);
    
    for (int i = 0; i < LOG_DAYS; i++) {
        if (water_change_log[i].timestamp == 0) continue;
        
        char entry[128];
        struct tm *timeinfo = localtime(&water_change_log[i].timestamp);
        snprintf(entry, sizeof(entry), "%02d/%02d %02d:%02d - Water button click",
                 timeinfo->tm_mon + 1, timeinfo->tm_mday,
                 timeinfo->tm_hour, timeinfo->tm_min);
        lv_list_add_text(list, entry);
    }
    
    lv_obj_t *btn_close = lv_btn_create(popup_history);
    lv_obj_set_size(btn_close, 100, 40);
    lv_obj_align(btn_close, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_t *label = lv_label_create(btn_close);
    lv_label_set_text(label, "Close");
    lv_obj_center(label);
    lv_obj_add_event_cb(btn_close, [](lv_event_t *e) {
        if (popup_history) { lv_obj_del(popup_history); popup_history = NULL; }
    }, LV_EVENT_CLICKED, NULL);
    
    lv_obj_move_foreground(popup_history);
}

/**
 * @brief Show feed log history
 */
static void show_feed_history_cb(lv_event_t *e) {
    if (popup_history) return;
    
    popup_history = lv_obj_create(panel_content);
    lv_obj_set_size(popup_history, 450, 300);
    lv_obj_center(popup_history);
    lv_obj_set_style_bg_color(popup_history, lv_color_hex(0x1a1a1a), 0);
    
    lv_obj_t *title = lv_label_create(popup_history);
    lv_label_set_text(title, "Feed Button History (7 Days)");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    
    lv_obj_t *list = lv_list_create(popup_history);
    lv_obj_set_size(list, 430, 220);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 40);
    
    for (int i = 0; i < LOG_DAYS; i++) {
        if (feed_log_data[i].timestamp == 0) continue;
        
        char entry[128];
        struct tm *timeinfo = localtime(&feed_log_data[i].timestamp);
        snprintf(entry, sizeof(entry), "%02d/%02d %02d:%02d - Feed button click",
                 timeinfo->tm_mon + 1, timeinfo->tm_mday,
                 timeinfo->tm_hour, timeinfo->tm_min);
        lv_list_add_text(list, entry);
    }
    
    lv_obj_t *btn_close = lv_btn_create(popup_history);
    lv_obj_set_size(btn_close, 100, 40);
    lv_obj_align(btn_close, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_t *label = lv_label_create(btn_close);
    lv_label_set_text(label, "Close");
    lv_obj_center(label);
    lv_obj_add_event_cb(btn_close, [](lv_event_t *e) {
        if (popup_history) { lv_obj_del(popup_history); popup_history = NULL; }
    }, LV_EVENT_CLICKED, NULL);
    
    lv_obj_move_foreground(popup_history);
}

/**
 * @brief Create and show monthly calendar popup
 */
static void show_monthly_calendar(void) {
    if (popup_monthly_cal) {
        lv_obj_del(popup_monthly_cal);
        popup_monthly_cal = NULL;
    }
    
    time_t now_time = time(NULL);
    struct tm now_tm;
    localtime_r(&now_time, &now_tm);
    
    if (monthly_cal_display_month == 0) {
        monthly_cal_display_month = now_tm.tm_mon + 1;
        monthly_cal_display_year = now_tm.tm_year + 1900;
    }
    
    popup_monthly_cal = lv_obj_create(lv_scr_act());
    lv_obj_set_size(popup_monthly_cal, 480, 320);
    lv_obj_set_pos(popup_monthly_cal, 0, 0);
    lv_obj_set_style_bg_color(popup_monthly_cal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(popup_monthly_cal, LV_OPA_90, 0);
    lv_obj_set_style_shadow_width(popup_monthly_cal, 0, 0);
    lv_obj_clear_flag(popup_monthly_cal, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t *cal_container = lv_obj_create(popup_monthly_cal);
    lv_obj_set_size(cal_container, 460, 300);
    lv_obj_center(cal_container);
    lv_obj_set_style_bg_color(cal_container, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_color(cal_container, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_border_width(cal_container, 2, 0);
    lv_obj_set_style_radius(cal_container, 10, 0);
    lv_obj_set_style_shadow_width(cal_container, 0, 0);
    lv_obj_clear_flag(cal_container, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t *title_cont = lv_obj_create(cal_container);
    lv_obj_set_size(title_cont, 440, 40);
    lv_obj_set_pos(title_cont, 10, 5);
    lv_obj_set_style_bg_opa(title_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(title_cont, 0, 0);
    lv_obj_clear_flag(title_cont, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t *btn_prev = lv_btn_create(title_cont);
    lv_obj_set_size(btn_prev, 35, 35);
    lv_obj_align(btn_prev, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(btn_prev, lv_color_hex(0x333333), 0);
    lv_obj_add_event_cb(btn_prev, [](lv_event_t *e) {
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
            monthly_cal_display_month--;
            if (monthly_cal_display_month < 1) {
                monthly_cal_display_month = 12;
                monthly_cal_display_year--;
            }
            // show_monthly_calendar();  // Disabled
        }
    }, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *prev_label = lv_label_create(btn_prev);
    lv_label_set_text(prev_label, LV_SYMBOL_LEFT);
    lv_obj_center(prev_label);
    
    lv_obj_t *title_label = lv_label_create(title_cont);
    const char *month_names[] = {"", "January", "February", "March", "April", "May", "June",
                                  "July", "August", "September", "October", "November", "December"};
    char title_text[50];
    snprintf(title_text, sizeof(title_text), "%s %d", 
             month_names[monthly_cal_display_month], monthly_cal_display_year);
    lv_label_set_text(title_label, title_text);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
    lv_obj_align(title_label, LV_ALIGN_CENTER, 0, 0);
    
    lv_obj_t *btn_next = lv_btn_create(title_cont);
    lv_obj_set_size(btn_next, 35, 35);
    lv_obj_align(btn_next, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(btn_next, lv_color_hex(0x333333), 0);
    lv_obj_add_event_cb(btn_next, [](lv_event_t *e) {
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
            monthly_cal_display_month++;
            if (monthly_cal_display_month > 12) {
                monthly_cal_display_month = 1;
                monthly_cal_display_year++;
            }
            // show_monthly_calendar();  // Disabled
        }
    }, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *next_label = lv_label_create(btn_next);
    lv_label_set_text(next_label, LV_SYMBOL_RIGHT);
    lv_obj_center(next_label);
    
    const char *day_headers[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    int header_y = 50;
    int cell_width = 60;
    int cell_height = 38;
    
    for (int i = 0; i < 7; i++) {
        lv_obj_t *header = lv_label_create(cal_container);
        lv_label_set_text(header, day_headers[i]);
        lv_obj_set_pos(header, 15 + (i * cell_width), header_y);
        lv_obj_set_style_text_font(header, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(header, lv_palette_main(LV_PALETTE_BLUE), 0);
    }
    
    struct tm first_day = {};
    first_day.tm_year = monthly_cal_display_year - 1900;
    first_day.tm_mon = monthly_cal_display_month - 1;
    first_day.tm_mday = 1;
    mktime(&first_day);
    
    int first_weekday = first_day.tm_wday;
    
    struct tm last_day = first_day;
    last_day.tm_mon++;
    last_day.tm_mday = 0;
    mktime(&last_day);
    int days_in_month = last_day.tm_mday;
    
    time_t today_time = time(NULL);
    struct tm today_tm;
    localtime_r(&today_time, &today_tm);
    
    int grid_y = header_y + 25;
    int day_num = 1;
    
    for (int row = 0; row < 6 && day_num <= days_in_month; row++) {
        // Yield to other tasks every row to prevent watchdog timeout
        vTaskDelay(pdMS_TO_TICKS(1));
        
        for (int col = 0; col < 7; col++) {
            if (row == 0 && col < first_weekday) continue;
            if (day_num > days_in_month) break;
            
            lv_obj_t *day_cell = lv_obj_create(cal_container);
            lv_obj_set_size(day_cell, cell_width - 5, cell_height - 3);
            lv_obj_set_pos(day_cell, 10 + (col * cell_width), grid_y + (row * cell_height));
            
            bool is_today = (day_num == today_tm.tm_mday && 
                           monthly_cal_display_month == today_tm.tm_mon + 1 &&
                           monthly_cal_display_year == today_tm.tm_year + 1900);
            
            if (is_today) {
                lv_obj_set_style_bg_color(day_cell, lv_color_hex(0x004080), 0);
                lv_obj_set_style_border_color(day_cell, lv_palette_main(LV_PALETTE_BLUE), 0);
                lv_obj_set_style_border_width(day_cell, 2, 0);
            } else {
                lv_obj_set_style_bg_color(day_cell, lv_color_hex(0x2a2a2a), 0);
                lv_obj_set_style_border_color(day_cell, lv_color_hex(0x4a4a4a), 0);
                lv_obj_set_style_border_width(day_cell, 1, 0);
            }
            lv_obj_set_style_radius(day_cell, 3, 0);
            lv_obj_set_style_shadow_width(day_cell, 0, 0);
            lv_obj_clear_flag(day_cell, LV_OBJ_FLAG_SCROLLABLE);
            
            lv_obj_t *day_label = lv_label_create(day_cell);
            char day_text[4];
            snprintf(day_text, sizeof(day_text), "%d", day_num);
            lv_label_set_text(day_label, day_text);
            lv_obj_set_style_text_font(day_label, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(day_label, lv_color_white(), 0);
            lv_obj_align(day_label, LV_ALIGN_TOP_MID, 0, 2);
            
            struct tm this_day = {};
            this_day.tm_year = monthly_cal_display_year - 1900;
            this_day.tm_mon = monthly_cal_display_month - 1;
            this_day.tm_mday = day_num;
            time_t day_timestamp = mktime(&this_day);
            
            struct tm day_tm;
            localtime_r(&day_timestamp, &day_tm);
            
            bool water_done = false;
            for (int j = 0; j < LOG_DAYS; j++) {
                if (water_change_log[j].timestamp == 0) continue;
                struct tm water_tm_buf;
                struct tm *water_tm = localtime_r(&water_change_log[j].timestamp, &water_tm_buf);
                if (water_tm->tm_yday == day_tm.tm_yday && water_tm->tm_year == day_tm.tm_year) {
                    water_done = true;
                    break;
                }
            }
            
            bool water_planned = false;
            if (planned_water_change_interval > 0) {
                time_t last_water_change_time = 0;
                for (int j = 0; j < LOG_DAYS; j++) {
                    if (water_change_log[j].timestamp > last_water_change_time) {
                        last_water_change_time = water_change_log[j].timestamp;
                    }
                }
                
                if (last_water_change_time > 0) {
                    struct tm last_change_tm;
                    localtime_r(&last_water_change_time, &last_change_tm);
                    last_change_tm.tm_hour = 0;
                    last_change_tm.tm_min = 0;
                    last_change_tm.tm_sec = 0;
                    time_t last_change_day = mktime(&last_change_tm);
                    time_t next_due_date = last_change_day + (planned_water_change_interval * 86400);
                    
                    struct tm day_start_tm = day_tm;
                    day_start_tm.tm_hour = 0;
                    day_start_tm.tm_min = 0;
                    day_start_tm.tm_sec = 0;
                    time_t day_start = mktime(&day_start_tm);
                    
                    struct tm today_start_tm = today_tm;
                    today_start_tm.tm_hour = 0;
                    today_start_tm.tm_min = 0;
                    today_start_tm.tm_sec = 0;
                    time_t today_start = mktime(&today_start_tm);
                    
                    if (day_start == next_due_date || (day_start == today_start && today_start > next_due_date)) {
                        water_planned = true;
                    }
                }
            }
            
            if (water_done) {
                lv_obj_t *water_dot = lv_obj_create(day_cell);
                lv_obj_set_size(water_dot, 5, 5);
                lv_obj_align(water_dot, LV_ALIGN_BOTTOM_MID, 0, -2);
                lv_obj_set_style_bg_color(water_dot, lv_palette_main(LV_PALETTE_CYAN), 0);
                lv_obj_set_style_bg_opa(water_dot, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(water_dot, 0, 0);
                lv_obj_set_style_radius(water_dot, LV_RADIUS_CIRCLE, 0);
                lv_obj_clear_flag(water_dot, LV_OBJ_FLAG_SCROLLABLE);
            } else if (water_planned) {
                lv_obj_t *water_dot = lv_obj_create(day_cell);
                lv_obj_set_size(water_dot, 5, 5);
                lv_obj_align(water_dot, LV_ALIGN_BOTTOM_MID, 0, -2);
                lv_obj_set_style_bg_opa(water_dot, LV_OPA_TRANSP, 0);
                lv_obj_set_style_border_color(water_dot, lv_palette_main(LV_PALETTE_CYAN), 0);
                lv_obj_set_style_border_width(water_dot, 1, 0);
                lv_obj_set_style_radius(water_dot, LV_RADIUS_CIRCLE, 0);
                lv_obj_clear_flag(water_dot, LV_OBJ_FLAG_SCROLLABLE);
            }
            
            int planned_feed_count = 0;
            for (int j = 0; j < MAX_FEED_TIMES; j++) {
                if (planned_feed_times[j].enabled) {
                    planned_feed_count++;
                }
            }
            
            int logged_feed_count = 0;
            for (int j = 0; j < LOG_DAYS; j++) {
                if (feed_log_data[j].timestamp == 0) continue;
                struct tm feed_tm_buf;
                struct tm *feed_tm = localtime_r(&feed_log_data[j].timestamp, &feed_tm_buf);
                if (feed_tm->tm_yday == day_tm.tm_yday && feed_tm->tm_year == day_tm.tm_year) {
                    logged_feed_count++;
                }
            }
            
            int total_feeds = (logged_feed_count > planned_feed_count) ? logged_feed_count : planned_feed_count;
            if (total_feeds > 3) total_feeds = 3;
            
            if (total_feeds > 0) {
                int dot_spacing = 7;
                int total_width = (total_feeds * 5) + ((total_feeds - 1) * 2);
                int start_x = (cell_width - 5 - total_width) / 2;
                
                for (int j = 0; j < total_feeds; j++) {
                    lv_obj_t *feed_dot = lv_obj_create(day_cell);
                    lv_obj_set_size(feed_dot, 5, 5);
                    lv_obj_set_pos(feed_dot, start_x + (j * dot_spacing), 17);
                    
                    if (j < logged_feed_count) {
                        lv_obj_set_style_bg_color(feed_dot, lv_palette_main(LV_PALETTE_RED), 0);
                        lv_obj_set_style_bg_opa(feed_dot, LV_OPA_COVER, 0);
                        lv_obj_set_style_border_width(feed_dot, 0, 0);
                    } else {
                        lv_obj_set_style_bg_opa(feed_dot, LV_OPA_TRANSP, 0);
                        lv_obj_set_style_border_color(feed_dot, lv_palette_main(LV_PALETTE_RED), 0);
                        lv_obj_set_style_border_width(feed_dot, 1, 0);
                    }
                    lv_obj_set_style_radius(feed_dot, LV_RADIUS_CIRCLE, 0);
                    lv_obj_clear_flag(feed_dot, LV_OBJ_FLAG_SCROLLABLE);
                }
            }
            
            lv_obj_add_flag(day_cell, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_user_data(day_cell, (void*)(intptr_t)day_timestamp);
            lv_obj_add_event_cb(day_cell, [](lv_event_t *e) {
                if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
                    time_t day_ts = (time_t)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
                    show_day_history(day_ts);
                }
            }, LV_EVENT_CLICKED, NULL);
            
            day_num++;
        }
    }
    
    lv_obj_t *close_btn = lv_btn_create(cal_container);
    lv_obj_set_size(close_btn, 60, 30);
    lv_obj_align(close_btn, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x555555), 0);
    lv_obj_add_event_cb(close_btn, [](lv_event_t *e) {
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
            if (popup_monthly_cal) {
                lv_obj_del(popup_monthly_cal);
                popup_monthly_cal = NULL;
            }
        }
    }, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *close_label = lv_label_create(close_btn);
    lv_label_set_text(close_label, "Close");
    lv_obj_set_style_text_color(close_label, lv_color_white(), 0);
    lv_obj_center(close_label);
}

/**
 * @brief Create parameter log popup with 5 input fields
 */
static void create_param_popup(void) {
    if (popup_param) return;
    
    popup_param = lv_obj_create(panel_content);
    lv_obj_set_size(popup_param, 460, 310);
    lv_obj_center(popup_param);
    lv_obj_set_style_bg_color(popup_param, lv_color_hex(0x1a1a3a), 0);
    lv_obj_set_style_border_color(popup_param, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_border_width(popup_param, 2, 0);
    
    lv_obj_t *title = lv_label_create(popup_param);
    lv_label_set_text(title, LV_SYMBOL_EDIT " Parameter Log");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    
    const char *param_names[] = {"Ammonia (ppm)", "Nitrate (ppm)", "Nitrite (ppm)", "pH", "pH (unused)"};
    // Load current dashboard values (not from log history)
    float latest_values[] = {
        ammonia_ppm,     // Current ammonia value
        nitrate_ppm,     // Current nitrate value
        nitrite_ppm,     // Current nitrite value
        ph_level,        // Current pH value
        ph_level         // Placeholder (only one pH field needed)
    };
    
    for (int i = 0; i < 5; i++) {
        lv_obj_t *label = lv_label_create(popup_param);
        lv_label_set_text(label, param_names[i]);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_align(label, LV_ALIGN_TOP_LEFT, 20, 50 + i * 38);
        
        lv_obj_t *input = lv_textarea_create(popup_param);
        lv_obj_set_size(input, 100, 32);
        lv_obj_align(input, LV_ALIGN_TOP_RIGHT, -20, 45 + i * 38);
        lv_textarea_set_one_line(input, true);
        
        // Set to latest saved value or 0.0 if no data
        char val_str[16];
        snprintf(val_str, sizeof(val_str), "%.2f", latest_values[i]);
        lv_textarea_set_text(input, val_str);
        
        lv_obj_add_event_cb(input, input_field_event_cb, LV_EVENT_CLICKED, NULL);
    }
    
    lv_obj_t *btn_hist = lv_btn_create(popup_param);
    lv_obj_set_size(btn_hist, 80, 40);
    lv_obj_align(btn_hist, LV_ALIGN_BOTTOM_LEFT, 20, -10);
    lv_obj_t *label_hist = lv_label_create(btn_hist);
    lv_label_set_text(label_hist, "History");
    lv_obj_center(label_hist);
    lv_obj_add_event_cb(btn_hist, show_param_history_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *btn_save = lv_btn_create(popup_param);
    lv_obj_set_size(btn_save, 100, 40);
    lv_obj_align(btn_save, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_t *label_save = lv_label_create(btn_save);
    lv_label_set_text(label_save, "Save");
    lv_obj_center(label_save);
    lv_obj_add_event_cb(btn_save, save_param_log_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *btn_close = lv_btn_create(popup_param);
    lv_obj_set_size(btn_close, 100, 40);
    lv_obj_align(btn_close, LV_ALIGN_BOTTOM_RIGHT, -20, -10);
    lv_obj_t *label_close = lv_label_create(btn_close);
    lv_label_set_text(label_close, "Close");
    lv_obj_center(label_close);
    lv_obj_add_event_cb(btn_close, [](lv_event_t *e) { close_popup(); }, LV_EVENT_CLICKED, NULL);
    
    lv_obj_move_foreground(popup_param);
}

/**
 * @brief Create water change log popup
 */
static void create_water_popup(void) {
    if (popup_water) return;
    
    popup_water = lv_obj_create(panel_content);
    lv_obj_set_size(popup_water, 400, 220);
    lv_obj_center(popup_water);
    lv_obj_set_style_bg_color(popup_water, lv_color_hex(0x1a1a3a), 0);
    lv_obj_set_style_border_color(popup_water, lv_palette_main(LV_PALETTE_CYAN), 0);
    lv_obj_set_style_border_width(popup_water, 2, 0);
    
    lv_obj_t *title = lv_label_create(popup_water);
    lv_label_set_text(title, LV_SYMBOL_REFRESH " Water Change Log");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    
    lv_obj_t *label = lv_label_create(popup_water);
    lv_label_set_text(label, "Change water every (days):");
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_pos(label, 20, 60);
    
    lv_obj_t *input = lv_textarea_create(popup_water);
    lv_obj_set_size(input, 80, 35);
    lv_obj_set_pos(input, 280, 55);
    lv_textarea_set_one_line(input, true);
    
    // Set to current planned interval
    char val_str[16];
    snprintf(val_str, sizeof(val_str), "%lu", (unsigned long)planned_water_change_interval);
    lv_textarea_set_text(input, val_str);
    
    lv_obj_add_event_cb(input, input_field_event_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *btn_hist = lv_btn_create(popup_water);
    lv_obj_set_size(btn_hist, 80, 40);
    lv_obj_align(btn_hist, LV_ALIGN_BOTTOM_LEFT, 20, -15);
    lv_obj_t *label_hist = lv_label_create(btn_hist);
    lv_label_set_text(label_hist, "History");
    lv_obj_center(label_hist);
    lv_obj_add_event_cb(btn_hist, show_water_history_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *btn_save = lv_btn_create(popup_water);
    lv_obj_set_size(btn_save, 120, 40);
    lv_obj_align(btn_save, LV_ALIGN_BOTTOM_MID, 0, -15);
    lv_obj_t *label_save = lv_label_create(btn_save);
    lv_label_set_text(label_save, "Save Schedule");
    lv_obj_center(label_save);
    lv_obj_add_event_cb(btn_save, [](lv_event_t *e) {
        lv_obj_t *popup = lv_event_get_current_target(e);
        lv_obj_t *input = lv_obj_get_child(lv_obj_get_parent(popup), -4);
        const char *text = lv_textarea_get_text(input);
        int interval = atoi(text);
        if (interval > 0 && interval <= 365) {
            planned_water_change_interval = interval;
            current_water_interval_days = interval;
            ESP_LOGI(TAG, "Water change interval updated: %lu days", (unsigned long)planned_water_change_interval);
            
            // Save to SD card
            save_water_change_to_sd(interval);
            
            // Refresh calendar dots to update hollow circles
            refresh_weekly_calendar_dots();
        }
        close_popup(); 
    }, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *btn_close = lv_btn_create(popup_water);
    lv_obj_set_size(btn_close, 100, 40);
    lv_obj_align(btn_close, LV_ALIGN_BOTTOM_RIGHT, -20, -15);
    lv_obj_t *label_close = lv_label_create(btn_close);
    lv_label_set_text(label_close, "Close");
    lv_obj_center(label_close);
    lv_obj_add_event_cb(btn_close, [](lv_event_t *e) { close_popup(); }, LV_EVENT_CLICKED, NULL);
    
    lv_obj_move_foreground(popup_water);
}

/**
 * @brief Create feed log popup
 */
static void create_feed_popup(void) {
    if (popup_feed) return;
    
    popup_feed = lv_obj_create(panel_content);
    lv_obj_set_size(popup_feed, 400, 320);
    lv_obj_center(popup_feed);
    lv_obj_set_style_bg_color(popup_feed, lv_color_hex(0x1a1a3a), 0);
    lv_obj_set_style_border_color(popup_feed, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_set_style_border_width(popup_feed, 2, 0);
    
    lv_obj_t *title = lv_label_create(popup_feed);
    lv_label_set_text(title, LV_SYMBOL_IMAGE " Feed Management");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    
    // Schedule section
    lv_obj_t *schedule_label = lv_label_create(popup_feed);
    lv_label_set_text(schedule_label, "Planned Schedule:");
    lv_obj_set_style_text_color(schedule_label, lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_set_pos(schedule_label, 20, 45);
    
    // Number of feeds input
    lv_obj_t *feeds_label = lv_label_create(popup_feed);
    lv_label_set_text(feeds_label, "Feeds per day:");
    lv_obj_set_style_text_color(feeds_label, lv_color_white(), 0);
    lv_obj_set_pos(feeds_label, 20, 75);
    
    lv_obj_t *feeds_input = lv_textarea_create(popup_feed);
    lv_obj_set_size(feeds_input, 60, 35);
    lv_obj_set_pos(feeds_input, 150, 70);
    lv_textarea_set_one_line(feeds_input, true);
    
    // Count currently enabled feeds
    int active_feeds = 0;
    for (int i = 0; i < MAX_FEED_TIMES; i++) {
        if (planned_feed_times[i].enabled) active_feeds++;
    }
    if (active_feeds == 0) active_feeds = 3; // Default
    
    char feeds_str[8];
    snprintf(feeds_str, sizeof(feeds_str), "%d", active_feeds);
    lv_textarea_set_text(feeds_input, feeds_str);
    lv_obj_add_event_cb(feeds_input, input_field_event_cb, LV_EVENT_CLICKED, NULL);
    
    // Configure times button
    lv_obj_t *btn_config = lv_btn_create(popup_feed);
    lv_obj_set_size(btn_config, 150, 40);
    lv_obj_set_pos(btn_config, 225, 70);
    lv_obj_t *label_config = lv_label_create(btn_config);
    lv_label_set_text(label_config, "Set Schedule");
    lv_obj_center(label_config);
    lv_obj_add_event_cb(btn_config, [](lv_event_t *e) {
        lv_obj_t *input = (lv_obj_t *)lv_event_get_user_data(e);
        const char *text = lv_textarea_get_text(input);
        int num_feeds = atoi(text);
        
        if (num_feeds < 1) num_feeds = 1;
        if (num_feeds > MAX_FEED_TIMES) num_feeds = MAX_FEED_TIMES;
        
        // Configure feed times based on number
        // Define all time arrays outside the loop
        const uint8_t times_1[] = {12};
        const uint8_t times_2[] = {8, 20};
        const uint8_t times_3[] = {8, 14, 20};
        const uint8_t times_4[] = {7, 12, 17, 22};
        const uint8_t times_5[] = {6, 10, 14, 18, 22};
        const uint8_t times_6[] = {6, 9, 12, 15, 18, 21};
        
        for (int i = 0; i < MAX_FEED_TIMES; i++) {
            if (i < num_feeds) {
                // Select the appropriate time based on number of feeds
                if (num_feeds == 1) {
                    planned_feed_times[i].hour = times_1[i];
                } else if (num_feeds == 2) {
                    planned_feed_times[i].hour = times_2[i];
                } else if (num_feeds == 3) {
                    planned_feed_times[i].hour = times_3[i];
                } else if (num_feeds == 4) {
                    planned_feed_times[i].hour = times_4[i];
                } else if (num_feeds == 5) {
                    planned_feed_times[i].hour = times_5[i];
                } else if (num_feeds == 6) {
                    planned_feed_times[i].hour = times_6[i];
                }
                planned_feed_times[i].minute = 0;
                planned_feed_times[i].enabled = true;
            } else {
                planned_feed_times[i].enabled = false;
            }
        }
        
        current_feeds_per_day = num_feeds;
        ESP_LOGI(TAG, "Feed schedule updated: %d feeds per day", num_feeds);
        // Refresh calendar dots to update hollow circles
        refresh_weekly_calendar_dots();
        close_popup();
    }, LV_EVENT_CLICKED, feeds_input);
    
    lv_obj_t *btn_hist = lv_btn_create(popup_feed);
    lv_obj_set_size(btn_hist, 80, 40);
    lv_obj_align(btn_hist, LV_ALIGN_BOTTOM_LEFT, 20, -15);
    lv_obj_t *label_hist = lv_label_create(btn_hist);
    lv_label_set_text(label_hist, "History");
    lv_obj_center(label_hist);
    lv_obj_add_event_cb(btn_hist, show_feed_history_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *btn_save = lv_btn_create(popup_feed);
    lv_obj_set_size(btn_save, 100, 40);
    lv_obj_align(btn_save, LV_ALIGN_BOTTOM_MID, 0, -15);
    lv_obj_t *label_save = lv_label_create(btn_save);
    lv_label_set_text(label_save, "Save");
    lv_obj_center(label_save);
    lv_obj_add_event_cb(btn_save, [](lv_event_t *e) {
        // Shift existing entries down
        for (int i = LOG_DAYS - 1; i > 0; i--) {
            feed_log_data[i] = feed_log_data[i - 1];
        }
        
        // Save new entry at index 0 (most recent)
        feed_log_data[0].timestamp = time(NULL);
        feed_log_data[0].feeds_per_day = 2;  // TODO: Read from input field
        current_feeds_per_day = 2;
        
        ESP_LOGI(TAG, "Feeds per day saved: %d (timestamp: %ld)", 
                 feed_log_data[0].feeds_per_day, feed_log_data[0].timestamp);
        
        // Save to SD card
        save_feed_to_sd(feed_log_data[0].feeds_per_day);
        
        evaluate_and_update_mood();
        close_popup();
    }, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *btn_close = lv_btn_create(popup_feed);
    lv_obj_set_size(btn_close, 100, 40);
    lv_obj_align(btn_close, LV_ALIGN_BOTTOM_RIGHT, -20, -15);
    lv_obj_t *label_close = lv_label_create(btn_close);
    lv_label_set_text(label_close, "Close");
    lv_obj_center(label_close);
    lv_obj_add_event_cb(btn_close, [](lv_event_t *e) { close_popup(); }, LV_EVENT_CLICKED, NULL);
    
    lv_obj_move_foreground(popup_feed);
}

/**
 * @brief Calendar page button event callbacks
 */
static void calendar_button_event_cb(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    
    if (btn == btn_param_log) {
        create_param_popup();
    } else if (btn == btn_water_log) {
        create_water_popup();
    } else if (btn == btn_feed_log) {
        create_feed_popup();
    } else if (btn == btn_med_calc) {
        ESP_LOGI(TAG, "Med Calc button clicked - opening popup");
        show_med_calculator_popup();
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
        if (btn == btn_home) {
            // Scroll back to top (animation section)
            lv_obj_scroll_to_y(scroll_container, 0, LV_ANIM_ON);
            ESP_LOGI(TAG, "Scrolling back to home (top)");
        }
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
 * @brief Create a circular button for Feed/Water actions
 */
static lv_obj_t* create_circular_button(lv_obj_t *parent, const char *label_text, 
                                        lv_align_t align, lv_coord_t x_ofs, lv_coord_t y_ofs)
{
    // Create circular button
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 72, 72);
    lv_obj_align(btn, align, x_ofs, y_ofs);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xFFFF00), LV_PART_MAIN);  // Default yellow
    lv_obj_set_style_shadow_width(btn, 10, LV_PART_MAIN);
    lv_obj_set_style_shadow_spread(btn, 2, LV_PART_MAIN);
    
    // Add label
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, label_text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_black(), LV_PART_MAIN);
    lv_obj_center(label);
    
    return btn;
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
    
    // ═══════════════════════════════════════════════════════════════════════
    // CRITICAL: NO SPIFFS ACCESS ALLOWED IN LVGL CONTEXT
    // ═══════════════════════════════════════════════════════════════════════
    // All file I/O occurs ONLY in storage_task (task_coordinator.cpp)
    // LVGL callbacks NEVER open/read files or block on I/O
    ESP_LOGI(TAG, "✓ LVGL context is I/O-free - all file loading in storage_task");
    
    // Perform initial mood evaluation
    evaluate_and_update_mood();
    
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);
    
    // Create a scrollable container that's taller than the screen (landscape: 480×790 total)
    // Layout: Animation+Gauges (0-320px) + AI Assistant (320-470px) + Panel (470-790px)
    scroll_container = lv_obj_create(scr);
    lv_obj_set_size(scroll_container, 480, 790);  // 480 wide, 790 tall
    lv_obj_set_pos(scroll_container, 0, 0);
    lv_obj_set_style_bg_color(scroll_container, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_border_width(scroll_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scroll_container, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(scroll_container, LV_DIR_VER);  // Vertical scrolling only
    lv_obj_set_scrollbar_mode(scroll_container, LV_SCROLLBAR_MODE_OFF);  // Hide scrollbar
    
    // ===== ANIMATION + GAUGES SECTION (0-320px) - HOME VIEW =====
    
    // Create animation image widget
    animation_img = lv_img_create(scroll_container);
    lv_obj_set_pos(animation_img, 0, 0);  // Y=0 for home view
    
    // STEP 3: Request initial frames from storage_task (non-blocking)
    buffer_a_ready = false;
    buffer_b_ready = false;
    
    // Request initial frame 0 from storage_task
    ESP_LOGI(TAG, "[INIT] Requesting frame 0 for initial display");
    anim_frame_request_msg_t request = { .frame_index = 0 };
    xQueueOverwrite(queue_anim_frame_request, &request);
    
    // Wait briefly for frame 0 to load (initial display)
    // Polling is acceptable here during one-time init
    ESP_LOGI(TAG, "[INIT] Waiting for frame 0 to load...");
    for (int i = 0; i < 100 && !buffer_a_ready && !buffer_b_ready; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));  // Max 1 second wait
    }
    
    // Display frame 0 if ready (use anim_dsc_a for initial frame)
    if (buffer_a_ready && buffer_a_frame_index == 0) {
        anim_dsc_a.data = frame_buffer_a;
        active_dsc = &anim_dsc_a;
        lv_img_set_src(animation_img, active_dsc);
        ESP_LOGI(TAG, "[INIT] ✓ Frame 0 displayed from buffer_a");
        
        // Now request frame 1 for the timer callback
        ESP_LOGI(TAG, "[INIT] Requesting frame 1 for animation start");
        request.frame_index = 1;
        xQueueOverwrite(queue_anim_frame_request, &request);
        
    } else if (buffer_b_ready && buffer_b_frame_index == 0) {
        anim_dsc_a.data = frame_buffer_b;
        active_dsc = &anim_dsc_a;
        lv_img_set_src(animation_img, active_dsc);
        ESP_LOGI(TAG, "[INIT] ✓ Frame 0 displayed from buffer_b");
        
        // Now request frame 1 for the timer callback
        ESP_LOGI(TAG, "[INIT] Requesting frame 1 for animation start");
        request.frame_index = 1;
        xQueueOverwrite(queue_anim_frame_request, &request);
        
    } else {
        // Fallback: frame 0 not ready yet
        anim_dsc_a.data = frame_buffer_a;
        active_dsc = &anim_dsc_a;
        lv_img_set_src(animation_img, active_dsc);
        ESP_LOGW(TAG, "[INIT] ⚠ Frame 0 not ready, using placeholder");
        
        // Still request frame 1 for when timer starts
        ESP_LOGI(TAG, "[INIT] Requesting frame 1 anyway");
        request.frame_index = 1;
        xQueueOverwrite(queue_anim_frame_request, &request);
    }
    
    // Mood face icon next to animation
    mood_face = lv_label_create(scroll_container);
    lv_label_set_text(mood_face, LV_SYMBOL_OK);  // Default happy
    lv_obj_set_pos(mood_face, 390, 10);
    lv_obj_set_style_text_font(mood_face, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(mood_face, lv_palette_main(LV_PALETTE_GREEN), 0);
    
    // Date label shadow/outline (black text behind main label)
    date_shadow = lv_label_create(scroll_container);  // Use global static variable
    lv_label_set_text(date_shadow, "01 JAN");  // Default, will be updated
    lv_obj_set_pos(date_shadow, 16, 11);  // Offset by 1px down and right
    lv_obj_set_style_text_font(date_shadow, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(date_shadow, lv_color_black(), 0);  // Black shadow
    lv_obj_set_style_bg_opa(date_shadow, LV_OPA_TRANSP, 0);  // No background
    lv_obj_set_style_text_letter_space(date_shadow, 1, 0);
    
    // Date label on top of animation (format: "14 DEC")
    date_label = lv_label_create(scroll_container);
    lv_label_set_text(date_label, "01 JAN");  // Default, updated when WiFi connects
    lv_obj_set_pos(date_label, 15, 10);  // Top-left corner
    lv_obj_set_style_text_font(date_label, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(date_label, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(date_label, LV_OPA_TRANSP, 0);  // No background
    lv_obj_set_style_text_letter_space(date_label, 1, 0);  // Slight letter spacing for cleaner look
    
    // Note: Date will be updated by date_update_timer_cb when WiFi connects and time syncs
    
    // ===== AI ASSISTANT SECTION (320-470px) - SCROLL DOWN TO VIEW =====
    
    // Create AI assistant background
    lv_obj_t *ai_bg = lv_obj_create(scroll_container);
    lv_obj_set_size(ai_bg, 480, 150);
    lv_obj_set_pos(ai_bg, 0, 320);  // After animation section
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
    lv_obj_set_size(ai_text_label, 465, 60);
    lv_obj_set_pos(ai_text_label, 0, 35);
    lv_obj_set_style_text_color(ai_text_label, lv_color_white(), 0);
    lv_label_set_long_mode(ai_text_label, LV_LABEL_LONG_WRAP);
    
    // Medication calculator result display in AI section
    ai_med_result_label = lv_label_create(ai_bg);
    lv_label_set_text(ai_med_result_label, "");
    lv_obj_set_size(ai_med_result_label, 465, 40);
    lv_obj_set_pos(ai_med_result_label, 0, 100);
    lv_obj_set_style_text_color(ai_med_result_label, lv_palette_main(LV_PALETTE_CYAN), 0);
    lv_label_set_long_mode(ai_med_result_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    
    // Create circular buttons at Y=190 (replacing gauges)
    btn_feed_main = create_circular_button(scroll_container, "FEED", LV_ALIGN_TOP_LEFT, 30, 220);
    lv_obj_add_event_cb(btn_feed_main, main_button_event_cb, LV_EVENT_CLICKED, NULL);
    
    btn_water_main = create_circular_button(scroll_container, "CLEAN", LV_ALIGN_TOP_RIGHT, -30, 220);
    lv_obj_add_event_cb(btn_water_main, main_button_event_cb, LV_EVENT_CLICKED, NULL);
    
    // Move buttons to front (on top of animation)
    lv_obj_move_foreground(btn_feed_main);
    lv_obj_move_foreground(btn_water_main);
    
    // ===== PANEL SECTION (470-790px) =====
    
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
    
    // Create 7-day week calendar at the top (replacing Home button)
    int week_y = 10;
    int week_height = 60;
    int day_width = 55;
    int day_spacing = 5;
    int total_week_width = (7 * day_width) + (6 * day_spacing);  // 7 days + 6 gaps = 415px
    int content_width = 405;  // Usable content area
    int week_start_x = (content_width - total_week_width) / 2;  // Center in content area
    
    // Get current time
    time_t now_time = time(NULL);
    struct tm now_tm;
    localtime_r(&now_time, &now_tm);
    
    // Create 7 day boxes (current day in middle, index 3)
    for (int i = 0; i < 7; i++) {
        // Calculate date for this day (3 days before to 3 days after)
        time_t day_time = now_time + ((i - 3) * 86400);
        struct tm day_tm;
        localtime_r(&day_time, &day_tm);
        
        // Create day box
        lv_obj_t *day_box = lv_obj_create(panel_content);
        week_day_boxes[i] = day_box;  // Store reference
        lv_obj_set_size(day_box, day_width, week_height);
        lv_obj_set_pos(day_box, week_start_x + (i * (day_width + day_spacing)), week_y);
        
        // Highlight current day with different color
        if (i == 3) {
            lv_obj_set_style_bg_color(day_box, lv_color_hex(0x3a4a5a), 0);  // Steel blue
            lv_obj_set_style_border_color(day_box, lv_palette_main(LV_PALETTE_LIGHT_BLUE), 0);
        } else {
            lv_obj_set_style_bg_color(day_box, lv_color_hex(0x2a2a2a), 0);
            lv_obj_set_style_border_color(day_box, lv_color_hex(0x4a4a4a), 0);
        }
        lv_obj_set_style_border_width(day_box, 2, 0);
        lv_obj_set_style_radius(day_box, 5, 0);
        lv_obj_clear_flag(day_box, LV_OBJ_FLAG_SCROLLABLE);
        
        // Day name (e.g., "MON")
        const char *day_names[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
        lv_obj_t *day_name = lv_label_create(day_box);
        lv_label_set_text(day_name, day_names[day_tm.tm_wday]);
        lv_obj_set_style_text_font(day_name, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(day_name, lv_color_white(), 0);
        lv_obj_align(day_name, LV_ALIGN_CENTER, 0, 0);
        
        // Calculate log index for this day using day-of-year
        int log_index = day_tm.tm_yday % LOG_DAYS;
        
        // Blue water dot - centered horizontally at bottom
        if (water_log[log_index] > 0) {
            lv_obj_t *water_dot = lv_obj_create(day_box);
            lv_obj_set_size(water_dot, 6, 6);
            lv_obj_set_pos(water_dot, (day_width - 6) / 2, 38);  // Below day name, above bottom
            lv_obj_set_style_bg_color(water_dot, lv_palette_main(LV_PALETTE_CYAN), 0);
            lv_obj_set_style_bg_opa(water_dot, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(water_dot, 0, 0);
            lv_obj_set_style_radius(water_dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_pad_all(water_dot, 0, 0);
            lv_obj_clear_flag(water_dot, LV_OBJ_FLAG_SCROLLABLE);
        }
        
        // Red feed dots - arranged horizontally at top
        uint32_t feed_count = feed_log[log_index];
        if (feed_count > 4) feed_count = 4;  // Max 4 dots
        if (feed_count > 0) {
            // Calculate total width and center the row (same centering approach as blue dot)
            int total_dots_width = (feed_count * 6) + ((feed_count - 1) * 2);  // dots + 2px gaps
            int start_x = (day_width - total_dots_width) / 2;
            for (uint32_t j = 0; j < feed_count; j++) {
                lv_obj_t *feed_dot = lv_obj_create(day_box);
                lv_obj_set_size(feed_dot, 6, 6);
                lv_obj_set_pos(feed_dot, start_x + (j * 8), 3);  // Use start_x for proper centering
                lv_obj_set_style_bg_color(feed_dot, lv_palette_main(LV_PALETTE_RED), 0);
                lv_obj_set_style_border_width(feed_dot, 0, 0);
                lv_obj_set_style_radius(feed_dot, LV_RADIUS_CIRCLE, 0);
            }
        }
        
        // Make clickable - store day timestamp in user data
        lv_obj_add_flag(day_box, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(day_box, (void*)(intptr_t)day_time);
        lv_obj_add_event_cb(day_box, [](lv_event_t *e) {
            if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
                time_t day_timestamp = (time_t)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
                show_day_history(day_timestamp);
            }
        }, LV_EVENT_CLICKED, NULL);
    }
    
    // Refresh calendar dots to show planned activities (hollow circles)
    refresh_weekly_calendar_dots();
    
    // Update calendar_y position for elements below
    int calendar_y = week_y + week_height + 15;
    
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
    
    // Monthly calendar disabled
    // lv_obj_add_flag(panel_calendar, LV_OBJ_FLAG_CLICKABLE);
    // lv_obj_add_event_cb(panel_calendar, [](lv_event_t *e) {
    //     if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
    //         time_t now = time(NULL);
    //         struct tm now_tm;
    //         localtime_r(&now, &now_tm);
    //         monthly_cal_display_month = now_tm.tm_mon + 1;
    //         monthly_cal_display_year = now_tm.tm_year + 1900;
    //         show_monthly_calendar();
    //     }
    // }, LV_EVENT_CLICKED, NULL);
    
    // Create 3 buttons for log systems (Parameters, Water Change, Feed) - vertical on right side
    int btn_x = 240;  // Right side position
    int btn_start_y = calendar_y + 0;  // Start below calendar
    int btn_spacing = 55;  // Vertical spacing between buttons
    
    btn_param_log = lv_btn_create(panel_content);
    lv_obj_set_size(btn_param_log, 100, 45);
    lv_obj_set_pos(btn_param_log, btn_x, btn_start_y);
    lv_obj_t *label1 = lv_label_create(btn_param_log);
    lv_label_set_text(label1, "Parameters");
    lv_obj_center(label1);
    lv_obj_add_event_cb(btn_param_log, calendar_button_event_cb, LV_EVENT_CLICKED, NULL);

    btn_water_log = lv_btn_create(panel_content);
    lv_obj_set_size(btn_water_log, 100, 45);
    lv_obj_set_pos(btn_water_log, btn_x, btn_start_y + btn_spacing);
    lv_obj_t *label2 = lv_label_create(btn_water_log);
    lv_label_set_text(label2, "Water");
    lv_obj_center(label2);
    lv_obj_add_event_cb(btn_water_log, calendar_button_event_cb, LV_EVENT_CLICKED, NULL);

    btn_feed_log = lv_btn_create(panel_content);
    lv_obj_set_size(btn_feed_log, 100, 45);
    lv_obj_set_pos(btn_feed_log, btn_x, btn_start_y + btn_spacing * 2);
    lv_obj_t *label3 = lv_label_create(btn_feed_log);
    lv_label_set_text(label3, "Feed");
    lv_obj_center(label3);
    lv_obj_add_event_cb(btn_feed_log, calendar_button_event_cb, LV_EVENT_CLICKED, NULL);

    // Add Medication Calculator button - vertical tall button to the right of all 3 buttons
    int med_calc_height = btn_spacing * 2 + 45;  // Spans all 3 buttons (Parameters, Water, Feed)
    btn_med_calc = lv_btn_create(panel_content);
    lv_obj_set_size(btn_med_calc, 38, med_calc_height);  // Narrow width, tall height
    lv_obj_set_pos(btn_med_calc, btn_x + 105, btn_start_y);  // To the right with 5px gap
    lv_obj_set_style_bg_color(btn_med_calc, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_t *label4 = lv_label_create(btn_med_calc);
    lv_label_set_text(label4, "M\ne\nd\n\nC\na\nl\nc");  // Vertical text with breaks
    lv_obj_set_style_text_align(label4, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label4);
    lv_obj_add_event_cb(btn_med_calc, calendar_button_event_cb, LV_EVENT_CLICKED, NULL);
    
    ESP_LOGI(TAG, "Scrollable dashboard with animation and panel created successfully");
    
    // Initialize water quality values to ideal ranges (Happy mood - cycled tank)
    dashboard_update_ammonia(0.0f);   // Ammonia: 0 ppm (must be 0)
    dashboard_update_nitrite(0.0f);   // Nitrite: 0 ppm (must be 0)
    dashboard_update_nitrate(10.0f);  // Nitrate: 10 ppm (safe level)
    dashboard_update_ph(7.0f);        // pH: 7.0 (neutral, ideal)
    
    // CRITICAL FIX: Create animation timer from LVGL context using one-shot initializer
    // Ensures timer is registered after LVGL task is fully running
    lv_timer_t *init_timer = lv_timer_create(animation_init_timer_cb, 100, NULL);
    if (!init_timer) {
        ESP_LOGE(TAG, "Failed to create initialization timer!");
    } else {
        lv_timer_set_repeat_count(init_timer, 1);  // One-shot
        ESP_LOGI(TAG, "Animation initialization timer created - will fire in 100ms");
    }
    
    // STEP 2: Start mood result handler (checks queue every 50ms for <100ms latency)
    lv_timer_create(mood_result_handler, 50, NULL);
    
    // STEP 4: Start AI result handler (checks queue every 100ms for AI responses)
    lv_timer_create(ai_result_handler, 100, NULL);
    
    // STEP 5: Start Blynk snapshot publisher (updates every 30 seconds)
    lv_timer_create(blynk_snapshot_publisher, 30000, NULL);
    
    // Date update timer (updates every 10 minutes)
    lv_timer_create(date_update_timer_cb, 600000, NULL);
    
    // Initialize AI assistant
    update_ai_assistant();
    
    ESP_LOGI(TAG, "Dashboard initialized successfully - Animation section is default view");
    ESP_LOGI(TAG, "Animation enabled - cycling through 8 C array frames at 12 FPS");
    ESP_LOGI(TAG, "Swipe right from left edge to open side panel, swipe down to see AI Assistant");
}

/**
 * @brief Open the side panel with slide animation
 */
/**
 * @brief Update ammonia level (ppm)
 * @param value Ammonia in ppm (0 is ideal, >0.5 is critical)
 */
void dashboard_update_ammonia(float value)
{
    if (value < 0.0f) value = 0.0f;
    if (value > 5.0f) value = 5.0f;  // Cap at reasonable max for display
    
    ammonia_ppm = value;
    
    // Re-evaluate mood when ammonia changes
    evaluate_and_update_mood();
    update_ai_assistant();
}

/**
 * @brief Update nitrite level (ppm)
 * @param value Nitrite in ppm (0 is ideal, >0.5 is critical)
 */
void dashboard_update_nitrite(float value)
{
    if (value < 0.0f) value = 0.0f;
    if (value > 5.0f) value = 5.0f;  // Cap at reasonable max for display
    
    nitrite_ppm = value;
    
    // Re-evaluate mood when nitrite changes
    evaluate_and_update_mood();
    update_ai_assistant();
}

/**
 * @brief Update nitrate level (ppm)
 * @param value Nitrate in ppm (<20 safe, 20-40 warning, >40 critical)
 */
void dashboard_update_nitrate(float value)
{
    if (value < 0.0f) value = 0.0f;
    if (value > 200.0f) value = 200.0f;  // Cap at reasonable max for display
    
    nitrate_ppm = value;
    
    // Re-evaluate mood when nitrate changes
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
 * ═════════════════════════════════════════════════════════════════════════════
 * Set Animation Category (Mood Change Handler)
 * ═════════════════════════════════════════════════════════════════════════════
 * @param category Category to switch to (0=Happy, 1=Sad, 2=Angry)
 * 
 * CRITICAL: Resets frame index to 0 when mood changes.
 * This ensures each emotion starts from frame 0, not mid-sequence.
 * 
 * NON-BLOCKING: Requests frame via queue, doesn't wait for load completion.
 */
void dashboard_set_animation_category(uint8_t category)
{
    if (category >= 3) {  // 3 total categories (0, 1, 2)
        ESP_LOGW(TAG, "Invalid category %d, must be 0-2", category);
        return;
    }
    
    ESP_LOGI(TAG, "═════════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "MOOD CHANGE: %s → %s",
             current_category == 0 ? "HAPPY" : (current_category == 1 ? "SAD" : "ANGRY"),
             category == 0 ? "HAPPY" : (category == 1 ? "SAD" : "ANGRY"));
    ESP_LOGI(TAG, "═════════════════════════════════════════════════════════");
    
    // ═════════════════════════════════════════════════════════════════════════
    // STEP 1: Reset frame index to 0 (start of new emotion sequence)
    // ═════════════════════════════════════════════════════════════════════════
    current_category = category;
    current_frame = 0;
    
    // Reset frame update timer so first frame shows immediately
    last_frame_update_time = (uint32_t)(esp_timer_get_time() / 1000000) - 3;  // Force immediate update
    
    // ═════════════════════════════════════════════════════════════════════════
    // STEP 2: Invalidate old buffers (they contain wrong emotion frames)
    // ═════════════════════════════════════════════════════════════════════════
    buffer_a_ready = false;
    buffer_b_ready = false;
    
    // ═════════════════════════════════════════════════════════════════════════
    // STEP 3: Request frame 0 of new emotion (NON-BLOCKING queue send)
    // ═════════════════════════════════════════════════════════════════════════
    uint8_t abs_frame = category * 8;  // Each emotion has 8 frames
    
    anim_frame_request_msg_t request = { .frame_index = abs_frame };
    if (xQueueOverwrite(queue_anim_frame_request, &request) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to request frame 0 of new emotion");
    } else {
        ESP_LOGI(TAG, "Requested frame 0 of %s emotion (abs_frame=%d)",
                 category == 0 ? "HAPPY" : (category == 1 ? "SAD" : "ANGRY"), abs_frame);
    }
    
    // Note: Display will update when storage_task finishes loading frame 0
    // static_frame_timer_cb will detect buffer_ready flag and swap pointer
}

/**
 * @brief Update calendar with current date/time from system
 */
void dashboard_update_calendar(void)
{
    // Call the shared date update function to sync both displays
    date_update_timer_cb(NULL);
    ESP_LOGI(TAG, "Calendar synchronized via date timer");
}

/**
 * @brief Get current animation category
 * @return Current category (0=Happy, 1=Sad, 2=Angry)
 */
uint8_t dashboard_get_animation_category(void)
{
    return (uint8_t)current_category;
}

/**
 * @brief Simulate feeding time (for testing/demo)
 * @param hours_ago Hours since last feeding
 */
void dashboard_simulate_feed_time(float hours_ago)
{
    uint32_t current_time = get_current_time_seconds();
    last_feed_time = current_time - (uint32_t)(hours_ago * 3600.0f);
    
    // Re-evaluate mood and update button colors
    evaluate_and_update_mood();
}

/**
 * @brief Simulate cleaning time (for testing/demo)
 * @param days_ago Days since last cleaning
 */
void dashboard_simulate_clean_time(float days_ago)
{
    uint32_t current_time = get_current_time_seconds();
    last_clean_time = current_time - (uint32_t)(days_ago * 86400.0f);
    
    // Re-evaluate mood and update button colors
    evaluate_and_update_mood();
}

