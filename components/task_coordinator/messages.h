#ifndef MESSAGES_H
#define MESSAGES_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * STEP 2: Real message types for mood calculation
 * 
 * CONSTRAINT: No bulk data in queues (metadata only)
 * These structures contain parameter values, not bulk image data
 */

// Aquarium parameters for mood calculation
typedef struct {
    float ammonia_ppm;
    float nitrite_ppm;
    float nitrate_ppm;
    float ph_level;
    uint32_t last_feed_time;
    uint32_t last_clean_time;
    uint32_t planned_feed_interval;
    uint32_t planned_water_change_interval;
} aquarium_params_t;

// Mood calculation result
typedef struct {
    int ammonia_score;
    int nitrite_score;
    int nitrate_score;
    int ph_score;
    int feed_score;
    int clean_score;
    int total_score;
    uint8_t category;  // 0=HAPPY, 1=SAD, 2=ANGRY
} mood_result_t;

// Placeholder: Animation frame request (index only)
typedef struct {
    uint8_t frame_index;   // Absolute frame number (0-23)
} anim_frame_request_msg_t;

// Placeholder: Animation frame ready notification (index only)
typedef struct {
    uint8_t frame_index;   // Which frame is ready
    uint8_t buffer_slot;   // Which pool buffer contains it
} anim_frame_ready_msg_t;

// STEP 4: AI request (parameters for cloud query)
typedef struct {
    float ammonia_ppm;
    float nitrite_ppm;
    float nitrate_ppm;
    float hours_since_feed;
    float days_since_clean;
    int feeds_per_day;
    int water_change_interval;
    uint32_t timestamp;  // For rate limiting
} ai_request_msg_t;

// STEP 4: AI result (advice text)
typedef struct {
    bool success;
    char advice[512];  // AI response text
} ai_result_msg_t;

// STEP 5: Blynk sync data (snapshot of current state)
typedef struct {
    float ammonia_ppm;
    float nitrite_ppm;
    float nitrate_ppm;
    float ph_level;
    float feed_hours;      // Hours since last feed
    float clean_days;      // Days since last clean
    char mood[16];         // "HAPPY", "SAD", or "ANGRY"
    char ai_advice[512];   // Latest AI advice text
} blynk_sync_msg_t;

#ifdef __cplusplus
}
#endif

#endif // MESSAGES_H
