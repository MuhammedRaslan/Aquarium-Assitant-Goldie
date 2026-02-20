# LVGL Scrolling Control Guide

## Available Scroll Functions

### 1. Scroll to specific Y position
```cpp
lv_obj_scroll_to_y(scroll_container, 150, LV_ANIM_ON);   // With animation
lv_obj_scroll_to_y(scroll_container, 0, LV_ANIM_OFF);    // Instant jump
```

### 2. Scroll by relative amount
```cpp
lv_obj_scroll_by(scroll_container, 0, 100, LV_ANIM_ON);  // Scroll down 100px
lv_obj_scroll_by(scroll_container, 0, -100, LV_ANIM_ON); // Scroll up 100px
```

### 3. Scroll to specific object
```cpp
lv_obj_scroll_to_view(ai_bg, LV_ANIM_ON);       // Scroll to AI section
lv_obj_scroll_to_view(animation_img, LV_ANIM_ON); // Scroll to animation
```

### 4. Get current scroll position
```cpp
lv_coord_t y = lv_obj_get_scroll_y(scroll_container);
ESP_LOGI("SCROLL", "Current Y position: %d", y);
```

### 5. Enable/Disable scrolling
```cpp
lv_obj_add_flag(scroll_container, LV_OBJ_FLAG_SCROLLABLE);    // Enable
lv_obj_clear_flag(scroll_container, LV_OBJ_FLAG_SCROLLABLE);  // Disable
```

### 6. Set scroll direction
```cpp
lv_obj_set_scroll_dir(scroll_container, LV_DIR_VER);   // Vertical only
lv_obj_set_scroll_dir(scroll_container, LV_DIR_HOR);   // Horizontal only
lv_obj_set_scroll_dir(scroll_container, LV_DIR_ALL);   // Both directions
lv_obj_set_scroll_dir(scroll_container, LV_DIR_NONE);  // Disable scrolling
```

## Your Dashboard Layout

### Section Positions:
- **AI Assistant**: Y = 0-150px
- **Animation**: Y = 150-470px  
- **Panel**: Y = 470-800px

### Quick Navigation Examples:

**Jump to AI section:**
```cpp
lv_obj_scroll_to_y(scroll_container, 0, LV_ANIM_ON);
```

**Jump to Animation (default):**
```cpp
lv_obj_scroll_to_y(scroll_container, 150, LV_ANIM_ON);
```

**Jump to Panel:**
```cpp
lv_obj_scroll_to_y(scroll_container, 470, LV_ANIM_ON);
```

**Smooth scroll down 50px:**
```cpp
lv_obj_scroll_by(scroll_container, 0, 50, LV_ANIM_ON);
```

## Touch Gestures

Users can:
- **Swipe up**: Scroll down (see content below)
- **Swipe down**: Scroll up (see content above)
- **Drag**: Continuous scrolling

## Advanced: Snap Scrolling

To make sections "snap" into place:

```cpp
lv_obj_set_scroll_snap_y(scroll_container, LV_SCROLL_SNAP_CENTER);
```

Options:
- `LV_SCROLL_SNAP_NONE`: Free scrolling (current)
- `LV_SCROLL_SNAP_START`: Snap to section tops
- `LV_SCROLL_SNAP_CENTER`: Center sections
- `LV_SCROLL_SNAP_END`: Snap to section bottoms

## Button-Controlled Navigation Example

```cpp
// Button to scroll to AI
static void btn_ai_event_cb(lv_event_t *e) {
    lv_obj_scroll_to_y(scroll_container, 0, LV_ANIM_ON);
}

// Button to scroll to Animation
static void btn_anim_event_cb(lv_event_t *e) {
    lv_obj_scroll_to_y(scroll_container, 150, LV_ANIM_ON);
}

// Button to scroll to Panel
static void btn_panel_event_cb(lv_event_t *e) {
    lv_obj_scroll_to_y(scroll_container, 470, LV_ANIM_ON);
}

// Create navigation buttons
lv_obj_t *btn_ai = lv_btn_create(scr);
lv_obj_add_event_cb(btn_ai, btn_ai_event_cb, LV_EVENT_CLICKED, NULL);
```

## Scroll Events

Detect when user scrolls:

```cpp
static void scroll_event_cb(lv_event_t *e) {
    lv_coord_t y = lv_obj_get_scroll_y(scroll_container);
    
    if (y < 75) {
        ESP_LOGI("SCROLL", "Viewing AI section");
    } else if (y < 320) {
        ESP_LOGI("SCROLL", "Viewing Animation");
    } else {
        ESP_LOGI("SCROLL", "Viewing Panel");
    }
}

lv_obj_add_event_cb(scroll_container, scroll_event_cb, 
                    LV_EVENT_SCROLL, NULL);
```

## Current Startup Behavior

On device boot:
```cpp
lv_obj_scroll_to_y(scroll_container, 150, LV_ANIM_OFF);
```

This instantly positions the view at the animation section, hiding AI above.
