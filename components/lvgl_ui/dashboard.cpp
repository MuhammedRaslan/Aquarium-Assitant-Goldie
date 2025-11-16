#include "dashboard.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "dashboard";

// Declare external image arrays (from converted C files)
LV_IMG_DECLARE(frame1);
LV_IMG_DECLARE(frame2);
LV_IMG_DECLARE(frame3);

// UI Objects
static lv_obj_t *animation_img = NULL;
static lv_obj_t *gauge1 = NULL;
static lv_obj_t *gauge2 = NULL;
static lv_obj_t *gauge1_label = NULL;
static lv_obj_t *gauge2_label = NULL;

// Animation state
static lv_timer_t *anim_timer = NULL;
static uint8_t current_frame = 0;

// Animation frames (C array image references)
static const lv_img_dsc_t *anim_frames[3] = {
    &frame1,
    &frame2,
    &frame3
};

// Sensor values
static float sensor1_value = 0.0f;
static float sensor2_value = 50.0f;

/**
 * @brief Animation timer callback - cycles through animation frames
 */
static void animation_timer_cb(lv_timer_t *timer)
{
    // Cycle through frames
    current_frame = (current_frame + 1) % 3;
    
    // Display the frame from C array
    if (animation_img) {
        lv_img_set_src(animation_img, anim_frames[current_frame]);
        ESP_LOGI(TAG, "Displaying animation frame %d", current_frame);
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
    
    // NOTE: Rotation disabled - transform causes rendering issues in LVGL 8.4
    // lv_obj_set_style_transform_angle(meter, 900, LV_PART_MAIN);
    
    // Remove the default background
    lv_obj_set_style_bg_opa(meter, LV_OPA_0, LV_PART_MAIN);
    lv_obj_set_style_border_width(meter, 0, LV_PART_MAIN);
    
    // Add a scale
    lv_meter_scale_t *scale = lv_meter_add_scale(meter);
    lv_meter_set_scale_ticks(meter, scale, 21, 2, 10, lv_palette_main(LV_PALETTE_GREY));
    lv_meter_set_scale_major_ticks(meter, scale, 5, 4, 15, lv_color_black(), 15);
    // Rotate the scale by changing start angle: 225° makes it rotated 90° clockwise from original 135°
    lv_meter_set_scale_range(meter, scale, 0, 100, 270, 225);
    
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
 * @brief Create a label for gauge value display
 */
static lv_obj_t* create_gauge_label(lv_obj_t *parent, lv_obj_t *gauge_container)
{
    // Get the actual meter widget (first child of the container)
    lv_obj_t *meter = lv_obj_get_child(gauge_container, 0);
    lv_obj_t *label = lv_label_create(meter);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(label, "0.0");
    lv_obj_center(label);
    
    return label;
}

/**
 * @brief Initialize the dashboard UI
 */
void dashboard_init(void)
{
    ESP_LOGI(TAG, "Initializing IoT Dashboard");
    
    lv_obj_t *scr = lv_scr_act();
    
    // Set screen background to dark
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);
    
    // Create Gauge 1 (Bottom Left corner) - spread further apart
    // Container is 170x170, screen is 320 wide - gauges at far edges
    gauge1 = create_gauge(scr, LV_ALIGN_BOTTOM_LEFT, -30, +20);
    gauge1_label = create_gauge_label(scr, gauge1);
    
    // Create Gauge 2 (Bottom Right corner, moved up 700 units total)
    gauge2 = create_gauge(scr, LV_ALIGN_BOTTOM_RIGHT, -180, -330);
    gauge2_label = create_gauge_label(scr, gauge2);
    
    // Add labels to the left of gauges
    lv_obj_t *label1_title = lv_label_create(scr);
    lv_obj_set_style_text_font(label1_title, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(label1_title, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(label1_title, "Sensor 1");
    lv_obj_align_to(label1_title, gauge1, LV_ALIGN_OUT_LEFT_MID, -5, 0);
    
    lv_obj_t *label2_title = lv_label_create(scr);
    lv_obj_set_style_text_font(label2_title, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(label2_title, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(label2_title, "Sensor 2");
    lv_obj_align_to(label2_title, gauge2, LV_ALIGN_OUT_LEFT_MID, -5, 0);
    
    // Create animation image widget LAST so it's on top layer
    animation_img = lv_img_create(scr);
    lv_obj_align(animation_img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(animation_img, LV_OBJ_FLAG_SCROLLABLE);
    
    // Load the first frame from C array
    lv_img_set_src(animation_img, anim_frames[0]);
    
    // Center the image
    lv_obj_align(animation_img, LV_ALIGN_CENTER, 0, 0);
    
    ESP_LOGI(TAG, "Loaded initial animation frame from C array");
    
    // Move gauges to front (on top of animation)
    lv_obj_move_foreground(gauge1);
    lv_obj_move_foreground(gauge2);
    lv_obj_move_foreground(label1_title);
    lv_obj_move_foreground(label2_title);
    
    // Initialize sensor values
    dashboard_update_sensor1(0.0f);
    dashboard_update_sensor2(50.0f);
    
    // Start animation timer (change frame every 500ms)
    anim_timer = lv_timer_create(animation_timer_cb, 500, NULL);
    
    ESP_LOGI(TAG, "Dashboard initialized successfully");
    ESP_LOGI(TAG, "Animation enabled - cycling through 3 C array frames at 2 FPS");
}

/**
 * @brief Update sensor 1 gauge value
 */
void dashboard_update_sensor1(float value)
{
    if (value < 0.0f) value = 0.0f;
    if (value > 100.0f) value = 100.0f;
    
    sensor1_value = value;
    
    if (gauge1 && gauge1_label) {
        // Get the actual meter widget (first child of the container)
        lv_obj_t *meter = lv_obj_get_child(gauge1, 0);
        // Update needle
        lv_meter_indicator_t *indic = (lv_meter_indicator_t*)lv_obj_get_user_data(meter);
        if (indic) {
            lv_meter_set_indicator_value(meter, indic, (int32_t)value);
        }
        
        // Update center label
        lv_label_set_text_fmt(gauge1_label, "%.1f", value);
    }
}

/**
 * @brief Update sensor 2 gauge value
 */
void dashboard_update_sensor2(float value)
{
    if (value < 0.0f) value = 0.0f;
    if (value > 100.0f) value = 100.0f;
    
    sensor2_value = value;
    
    if (gauge2 && gauge2_label) {
        // Get the actual meter widget (first child of the container)
        lv_obj_t *meter = lv_obj_get_child(gauge2, 0);
        // Update needle
        lv_meter_indicator_t *indic = (lv_meter_indicator_t*)lv_obj_get_user_data(meter);
        if (indic) {
            lv_meter_set_indicator_value(meter, indic, (int32_t)value);
        }
        
        // Update center label
        lv_label_set_text_fmt(gauge2_label, "%.1f", value);
    }
}
