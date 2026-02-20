# Calendar Page Redesign - Implementation Plan

## Overview
Complete redesign of calendar page with 3 log systems and pop-up interfaces.

## Changes Required

### 1. Data Structures (COMPLETED ✓)
Added to dashboard.cpp (after line 88):
- `param_log_t` structure (5 parameters + timestamp)
- `water_change_log_t` structure (interval + timestamp)  
- `feed_log_t` structure (feeds_per_day + timestamp)
- 7-day circular buffers for each
- `current_feeds_per_day` (default: 2)
- `current_water_interval_days` (default: 7)

### 2. UI Objects (COMPLETED ✓)
Added to dashboard.cpp (after line 48):
- `btn_param_log`, `btn_water_log`, `btn_feed_log`
- `popup_param`, `popup_water`, `popup_feed`
- `popup_history`, `popup_keypad`
- `active_input_field`

### 3. Helper Functions (COMPLETED ✓)
Added `get_next_feed_time()` - calculates next scheduled feed based on feeds_per_day

### 4. Event Callbacks (NEEDS INTEGRATION)
**Location: Insert after line 557 (after main_button_event_cb)**

Functions to add:
1. `close_popup()` - Closes all popups
2. `keypad_event_cb()` - Handles decimal keypad input
3. `show_keypad()` - Creates decimal keypad popup
4. `input_field_event_cb()` - Shows keypad when input clicked
5. `save_param_log_cb()` - Saves parameter log entry
6. `show_param_history_cb()` - Shows parameter history
7. `show_water_history_cb()` - Shows water change history
8. `show_feed_history_cb()` - Shows feed history
9. `create_param_popup()` - Creates parameter log popup (5 inputs)
10. `create_water_popup()` - Creates water change popup (1 input)
11. `create_feed_popup()` - Creates feed log popup (1 input)
12. `calendar_button_event_cb()` - Handles 3 calendar buttons

### 5. Calendar Page Redesign (NEEDS MODIFICATION)
**Location: Lines 928-932 (remove dropdown)**

Current code:
```cpp
panel_dropdown = lv_dropdown_create(panel_content);
lv_obj_set_size(panel_dropdown, 220, 35);
lv_obj_set_pos(panel_dropdown, 190, calendar_y + 50);
lv_dropdown_set_options(panel_dropdown, "Feed Amount\npH Calibration\nFlow Rate");
lv_obj_add_event_cb(panel_dropdown, panel_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
```

Replace with:
```cpp
// Create 3 buttons for log systems
btn_param_log = lv_btn_create(panel_content);
lv_obj_set_size(btn_param_log, 140, 40);
lv_obj_set_pos(btn_param_log, 10, calendar_y + 50);
lv_obj_t *label1 = lv_label_create(btn_param_log);
lv_label_set_text(label1, "Parameters");
lv_obj_center(label1);
lv_obj_add_event_cb(btn_param_log, calendar_button_event_cb, LV_EVENT_CLICKED, NULL);

btn_water_log = lv_btn_create(panel_content);
lv_obj_set_size(btn_water_log, 140, 40);
lv_obj_set_pos(btn_water_log, 160, calendar_y + 50);
lv_obj_t *label2 = lv_label_create(btn_water_log);
lv_label_set_text(label2, "Water");
lv_obj_center(label2);
lv_obj_add_event_cb(btn_water_log, calendar_button_event_cb, LV_EVENT_CLICKED, NULL);

btn_feed_log = lv_btn_create(panel_content);
lv_obj_set_size(btn_feed_log, 140, 40);
lv_obj_set_pos(btn_feed_log, 310, calendar_y + 50);
lv_obj_t *label3 = lv_label_create(btn_feed_log);
lv_label_set_text(label3, "Feed");
lv_obj_center(label3);
lv_obj_add_event_cb(btn_feed_log, calendar_button_event_cb, LV_EVENT_CLICKED, NULL);
```

### 6. Feed Button Color Logic Update (NEEDS MODIFICATION)
**Location: update_button_colors() function**

Current feed scoring based on hours since last feed.
New logic needed: Compare current time to next scheduled feed time.

```cpp
// In update_button_colors()
time_t next_feed = get_next_feed_time(current_feeds_per_day);
time_t now = time(NULL);
float hours_until_feed = (next_feed - now) / 3600.0f;

if (hours_until_feed < -2.0f) {  // Missed by 2+ hours
    current_mood_scores.feed_score = -2;
    feed_color = score_to_rgb_color(-2);  // Red
} else if (hours_until_feed < 0.0f) {  // Slightly overdue
    current_mood_scores.feed_score = -1;
    feed_color = score_to_rgb_color(-1);  // Orange
} else if (hours_until_feed < 1.0f) {  // Feeding time soon
    current_mood_scores.feed_score = 1;
    feed_color = score_to_rgb_color(1);  // Light green
} else {
    current_mood_scores.feed_score = 2;
    feed_color = score_to_rgb_color(2);  // Green
}
```

### 7. Remove Old Code
**Lines to delete or comment out:**
- Line 238: `panel_dropdown_event_cb` forward declaration
- Lines 582-608: `panel_dropdown_event_cb` function implementation
- Lines 928-932: Dropdown creation

## File Size Impact
- Current dashboard.cpp: ~1150 lines
- After redesign: ~1650 lines (+500 lines)
- Memory impact: ~260 bytes for 7-day logs (negligible)

## Testing Checklist
- [ ] Build compiles without errors
- [ ] Calendar page shows 3 buttons
- [ ] Parameter log popup opens with 5 inputs
- [ ] Decimal keypad appears when input clicked
- [ ] Water change popup opens with 1 input
- [ ] Feed log popup opens with 1 input
- [ ] History views display scrollable lists
- [ ] Feed button color changes based on schedule
- [ ] Saved values persist in memory

## Next Steps
Given the complexity, I recommend:
**Option A**: I implement everything in one large update (risky, harder to debug)
**Option B**: Implement in 3 phases:
  - Phase 1: Calendar buttons + basic popups (no keypad yet)
  - Phase 2: Add decimal keypad functionality
  - Phase 3: Add history views and feed scheduling logic

Which approach would you prefer?
