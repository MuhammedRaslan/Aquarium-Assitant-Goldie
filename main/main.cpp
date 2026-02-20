#include <stdio.h>

#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_timer.h"

#include "driver/gpio.h"

#include "driver/i2c_master.h"

#include "esp_io_expander_tca9554.h"

#include "esp_spiffs.h"

#include "lvgl.h"
#include "demos/lv_demos.h"

#include "esp_lvgl_port.h"

#include "dashboard.h"
#include "gemini_api.h"
#include "blynk_integration.h"

#include "esp_check.h"
#include "esp_log.h"

#include "iot_button.h"
#include "button_gpio.h"

#include "esp_axp2101_port.h"
#include "esp_camera_port.h"
#include "esp_es8311_port.h"
#include "esp_pcf85063_port.h"
#include "esp_qmi8658_port.h"
#include "esp_sdcard_port.h"
#include "esp_wifi_port.h"
#include "esp_3inch5_lcd_port.h"

#include "task_coordinator.h"

#define EXAMPLE_PIN_I2C_SDA GPIO_NUM_8
#define EXAMPLE_PIN_I2C_SCL GPIO_NUM_7

#define EXAMPLE_PIN_BUTTON GPIO_NUM_0

#define EXAMPLE_DISPLAY_ROTATION 90

#if EXAMPLE_DISPLAY_ROTATION == 90 || EXAMPLE_DISPLAY_ROTATION == 270
#define EXAMPLE_LCD_H_RES 480
#define EXAMPLE_LCD_V_RES 320
#else
#define EXAMPLE_LCD_H_RES 320
#define EXAMPLE_LCD_V_RES 480
#endif

#define LCD_BUFFER_SIZE EXAMPLE_LCD_H_RES *EXAMPLE_LCD_V_RES / 8

#define I2C_PORT_NUM 0

static const char *TAG = "lvgl_example";

i2c_master_bus_handle_t i2c_bus_handle;

esp_lcd_panel_io_handle_t io_handle = NULL;
esp_lcd_panel_handle_t panel_handle = NULL;
esp_io_expander_handle_t expander_handle = NULL;
esp_lcd_touch_handle_t touch_handle = NULL;
lv_disp_drv_t disp_drv;

lv_display_t *lvgl_disp = NULL;
lv_indev_t *lvgl_touch_indev = NULL;

bool touch_test_done = false;
// sdmmc_card_t *card = NULL;

void i2c_bus_init(void);
void io_expander_init(void);
void lv_port_init(void);
void spiffs_init(void);

extern "C" void app_main(void)
{
    // Initialize NVS (graceful failure - system can run without NVS)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "NVS needs erase, attempting recovery...");
        ret = nvs_flash_erase();
        if (ret == ESP_OK) {
            ret = nvs_flash_init();
        }
    }
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed (%s) - WiFi config will not persist", esp_err_to_name(ret));
        // Continue anyway - system can run without NVS
    } else {
        ESP_LOGI(TAG, "NVS initialized successfully");
    }
    
    // Initialize SPIFFS for image storage
    spiffs_init();
    
    // WiFi initialization moved to background task (non-blocking)
    // System will start in OFFLINE mode and transition to ONLINE when ready
    ESP_LOGI(TAG, "WiFi will initialize in background - UI starting immediately");
    
    i2c_bus_init();
    io_expander_init();
    esp_3inch5_display_port_init(&io_handle, &panel_handle, LCD_BUFFER_SIZE);
    esp_3inch5_touch_port_init(&touch_handle, i2c_bus_handle, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES, EXAMPLE_DISPLAY_ROTATION);
    esp_axp2101_port_init(i2c_bus_handle);
    vTaskDelay(pdMS_TO_TICKS(100));
    // esp_es8311_port_init(i2c_bus_handle);
    // esp_qmi8658_port_init(i2c_bus_handle);
    // esp_pcf85063_port_init(i2c_bus_handle);
    
    // Initialize SD card for animation frames
    esp_sdcard_port_init();
    
    // esp_camera_port_init(I2C_PORT_NUM);
    // esp_wifi_port_init("WSTEST", "waveshare0755");

    esp_3inch5_brightness_port_init();
    esp_3inch5_brightness_port_set(80);
    lv_port_init();
    
    // Initialize task coordinator (Step 0 - creates idle background tasks)
    // NO BEHAVIORAL CHANGES - tasks are stubs, queues unused
    task_coordinator_init();
    
    if (lvgl_port_lock(0))
    {
        // Initialize IoT Dashboard with gauges and animation
        dashboard_init();
        
        // WiFi/Blynk initialization happens in background
        // Calendar and Blynk will activate automatically when WiFi connects
        // Dashboard starts immediately in OFFLINE mode
        
        lvgl_port_unlock();
    }
    
    // Real deployment mode - values come from Parameter Menu or future sensors
    ESP_LOGI(TAG, "=== REAL DEPLOYMENT MODE - Use Parameter Menu to set values ===");
    ESP_LOGI(TAG, "To update water parameters:");
    ESP_LOGI(TAG, "  1. Tap 'Parameters' button on dashboard");
    ESP_LOGI(TAG, "  2. Enter Ammonia, Nitrite, Nitrate, pH values");
    ESP_LOGI(TAG, "  3. Values will be used for mood calculation and Blynk updates");
    
    // STEP 5: Blynk sync moved to wifi_task (via dashboard snapshot publisher)
    // Blynk updates now occur automatically every 30 seconds from LVGL timer
    // See: dashboard.cpp - blynk_snapshot_publisher() and task_coordinator.cpp - wifi_task()
    ESP_LOGI(TAG, "Blynk sync running on wifi_task (30s automatic updates)");
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));  // Keep alive delay
        
        // In real deployment, integrate sensor readings here:
        // TODO: Replace manual parameter entry with actual sensor readings:
        //   - Read ammonia from sensor → dashboard_update_ammonia()
        //   - Read nitrite from sensor → dashboard_update_nitrite()
        //   - Read nitrate from sensor → dashboard_update_nitrate()
        //   - Read pH from sensor → dashboard_update_ph()
        
        // Note: Blynk updates are now handled by wifi_task automatically
        // No need to call blynk_send_all_data() here
    }
}

void i2c_bus_init(void)
{
    i2c_master_bus_config_t i2c_mst_config = {};
    i2c_mst_config.clk_source = I2C_CLK_SRC_DEFAULT;
    i2c_mst_config.i2c_port = (i2c_port_num_t)I2C_PORT_NUM;
    i2c_mst_config.scl_io_num = EXAMPLE_PIN_I2C_SCL;
    i2c_mst_config.sda_io_num = EXAMPLE_PIN_I2C_SDA;
    i2c_mst_config.glitch_ignore_cnt = 7;
    i2c_mst_config.flags.enable_internal_pullup = 1;

    esp_err_t ret = i2c_new_master_bus(&i2c_mst_config, &i2c_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C master bus init failed (%s) - hardware peripherals unavailable", esp_err_to_name(ret));
        // Don't reboot - display may still work
        i2c_bus_handle = NULL;
    } else {
        ESP_LOGI(TAG, "I2C bus initialized successfully");
    }
}

void io_expander_init(void)
{
    // Graceful failure handling - IO expander is optional
    if (i2c_bus_handle == NULL) {
        ESP_LOGW(TAG, "I2C bus not available - skipping IO expander init");
        expander_handle = NULL;
        return;
    }
    
    esp_err_t ret = esp_io_expander_new_i2c_tca9554(i2c_bus_handle, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &expander_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "IO expander init failed (%s) - display power control unavailable", esp_err_to_name(ret));
        expander_handle = NULL;
        return;
    }
    
    ret = esp_io_expander_set_dir(expander_handle, IO_EXPANDER_PIN_NUM_1, IO_EXPANDER_OUTPUT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "IO expander set_dir failed");
        return;
    }
    
    ret = esp_io_expander_set_level(expander_handle, IO_EXPANDER_PIN_NUM_1, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "IO expander set_level(0) failed");
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    
    ret = esp_io_expander_set_level(expander_handle, IO_EXPANDER_PIN_NUM_1, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "IO expander set_level(1) failed");
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    
    ESP_LOGI(TAG, "IO expander initialized successfully");
}

void lv_port_init(void)
{
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    /* Reduce priority from 4 to 2 to prevent IDLE0 starvation
     * Priority 2 is sufficient for UI responsiveness while ensuring
     * IDLE task (priority 0) can run and service the task watchdog */
    port_cfg.task_priority = 2;
    port_cfg.task_affinity = 0;  // Pin to Core 0 (explicit)
    lvgl_port_init(&port_cfg);
    ESP_LOGI(TAG, "Adding LCD screen");
    lvgl_port_display_cfg_t display_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .control_handle = NULL,
        .buffer_size = LCD_BUFFER_SIZE,
        .double_buffer = true,
        .trans_size = 0,
        .hres = EXAMPLE_LCD_H_RES,
        .vres = EXAMPLE_LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = 0,
            .mirror_x = 1,
            .mirror_y = 0,
        },
        .flags = {
            .buff_dma = 0,
            .buff_spiram = 1,
            .sw_rotate = 1,
            .full_refresh = 0,
            .direct_mode = 0,
        },
    };

#if EXAMPLE_DISPLAY_ROTATION == 90
    display_cfg.rotation.swap_xy = 1;
    display_cfg.rotation.mirror_x = 1;
    display_cfg.rotation.mirror_y = 1;
#elif EXAMPLE_DISPLAY_ROTATION == 180
    display_cfg.rotation.swap_xy = 0;
    display_cfg.rotation.mirror_x = 0;
    display_cfg.rotation.mirror_y = 1;

#elif EXAMPLE_DISPLAY_ROTATION == 270
    display_cfg.rotation.swap_xy = 1;
    display_cfg.rotation.mirror_x = 0;
    display_cfg.rotation.mirror_y = 0;
#endif

    lvgl_disp = lvgl_port_add_disp(&display_cfg);
    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = lvgl_disp,
        .handle = touch_handle,
    };
    lvgl_touch_indev = lvgl_port_add_touch(&touch_cfg);
}

void spiffs_init(void)
{
    ESP_LOGI(TAG, "Initializing SPIFFS");
    
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = false
    };
    
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return;
    }
    
    size_t total = 0, used = 0;
    ret = esp_spiffs_info("storage", &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "SPIFFS: %d KB total, %d KB used", total / 1024, used / 1024);
    }
}