# Animation PNG Images Setup Guide

This guide explains how to add your 3 PNG animation frames to the dashboard.

## Directory Structure

Place your PNG files in this directory:
```
components/lvgl_ui/images/
├── anim_frame_0.png
├── anim_frame_1.png
├── anim_frame_2.png
└── README_ANIMATION.md (this file)
```

## Image Specifications

- **Format**: PNG (with transparency support)
- **Resolution**: Recommended 320x480 pixels (full screen) or smaller
- **Color Depth**: RGB565 (will be converted automatically)
- **File Naming**: 
  - `anim_frame_0.png` - First frame
  - `anim_frame_1.png` - Second frame
  - `anim_frame_2.png` - Third frame

## Converting PNG to C Array (Option 1: Using LVGL's Online Converter)

1. Go to: https://lvgl.io/tools/imageconverter
2. Upload each PNG file
3. Settings:
   - **Color format**: CF_TRUE_COLOR_ALPHA or CF_RGB565
   - **Output format**: C array
4. Download the generated `.c` files
5. Place them in `components/lvgl_ui/images/`
6. Update `dashboard.cpp` to use them (see below)

## Converting PNG to C Array (Option 2: Using Python Script)

Use the LVGL image converter script:
```bash
python lv_img_conv.py anim_frame_0.png --color-format CF_TRUE_COLOR_ALPHA --output-format c_array > anim_frame_0.c
```

## Integration in Code

### Step 1: Declare External Variables
In `dashboard.cpp`, add after includes:
```cpp
// External image declarations (generated from PNG files)
LV_IMG_DECLARE(anim_frame_0);
LV_IMG_DECLARE(anim_frame_1);
LV_IMG_DECLARE(anim_frame_2);
```

### Step 2: Update Animation Array
Replace the NULL initialization:
```cpp
static const void *anim_frames[3] = {
    &anim_frame_0,
    &anim_frame_1,
    &anim_frame_2
};
```

### Step 3: Update Animation Timer Callback
Replace the placeholder code in `animation_timer_cb()`:
```cpp
static void animation_timer_cb(lv_timer_t *timer)
{
    current_frame = (current_frame + 1) % 3;
    
    if (animation_img && anim_frames[current_frame]) {
        lv_img_set_src(animation_img, anim_frames[current_frame]);
        ESP_LOGI(TAG, "Animation frame: %d", current_frame);
    }
}
```

### Step 4: Update Dashboard Init
In `dashboard_init()`, change animation_img creation from obj to img:
```cpp
// Create animation image (full screen)
animation_img = lv_img_create(scr);
lv_obj_set_size(animation_img, LV_HOR_RES, LV_VER_RES);
lv_obj_set_pos(animation_img, 0, 0);
lv_img_set_src(animation_img, anim_frames[0]); // Set initial frame
```

## Alternative: Load PNG Files from SD Card (Runtime Loading)

If you want to load PNG files at runtime from SD card instead of embedding them:

```cpp
// In dashboard.cpp, update animation_timer_cb():
static void animation_timer_cb(lv_timer_t *timer)
{
    current_frame = (current_frame + 1) % 3;
    
    char filepath[64];
    sprintf(filepath, "S:/animation/frame_%d.png", current_frame);
    
    if (animation_img) {
        lv_img_set_src(animation_img, filepath);
        ESP_LOGI(TAG, "Loading frame: %s", filepath);
    }
}
```

Note: Make sure PNG decoder is enabled in sdkconfig (already done):
```
CONFIG_LV_USE_PNG=y
```

## Animation Timing

Current animation speed: **500ms per frame** (2 FPS)

To change, modify in `dashboard_init()`:
```cpp
// Change 500 to desired milliseconds between frames
anim_timer = lv_timer_create(animation_timer_cb, 500, NULL);
```

## Triggering Different Sequences Based on Sensor Values

To implement sensor-based animation sequences, modify `dashboard.cpp`:

```cpp
// Add sequence state
static uint8_t current_sequence = 0; // 0=normal, 1=alert, 2=critical

// In dashboard_update_sensor1() or dashboard_update_sensor2():
void dashboard_update_sensor1(float value)
{
    // ... existing code ...
    
    // Determine animation sequence based on sensor value
    if (value < 33.0f) {
        current_sequence = 0; // Normal sequence (frames 0,1,2)
    } else if (value < 66.0f) {
        current_sequence = 1; // Alert sequence (different frames)
    } else {
        current_sequence = 2; // Critical sequence
    }
}

// Update animation_timer_cb() to use different frame sets:
static void animation_timer_cb(lv_timer_t *timer)
{
    current_frame = (current_frame + 1) % 3;
    
    // Select frame based on sequence
    uint8_t frame_index = current_sequence * 3 + current_frame;
    
    if (animation_img && anim_frames[frame_index]) {
        lv_img_set_src(animation_img, anim_frames[frame_index]);
    }
}
```

## Testing Without PNG Files

The current implementation uses colored rectangles as placeholders:
- **Blue** → Frame 0
- **Green** → Frame 1  
- **Orange** → Frame 2

This will cycle automatically to verify the animation system works before adding actual images.
