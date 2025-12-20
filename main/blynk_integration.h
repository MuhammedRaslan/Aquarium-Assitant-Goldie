#ifndef BLYNK_INTEGRATION_H
#define BLYNK_INTEGRATION_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize Blynk (call after WiFi is connected)
bool blynk_init(void);

// Send sensor data to Blynk
void blynk_update_temperature(float value);
void blynk_update_oxygen(float value);
void blynk_update_ph(float value);
void blynk_update_feeding(float hours);
void blynk_update_cleaning(float days);
void blynk_update_mood(const char *mood);  // "HAPPY" or "SAD"
void blynk_update_ai_advice(const char *advice);

// Send all sensor data at once
void blynk_send_all_data(float temp, float oxygen, float ph, 
                         float feed_hours, float clean_days,
                         const char *mood, const char *ai_advice);

#ifdef __cplusplus
}
#endif

#endif // BLYNK_INTEGRATION_H
