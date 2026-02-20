#ifndef GEMINI_API_H
#define GEMINI_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize WiFi and connect to network
 * @return true if connected successfully
 */
bool gemini_init_wifi(void);

/**
 * @brief Check if WiFi is currently connected
 * @return true if connected, false otherwise
 */
bool gemini_is_wifi_connected(void);

/**
 * @brief Get current time in seconds since epoch
 * @return Current Unix timestamp, or 0 if time not synced
 */
uint32_t gemini_get_current_time(void);

/**
 * @brief Query Gemini API with aquarium parameters
 * @param ammonia_ppm Current ammonia level in ppm (must be 0)
 * @param nitrite_ppm Current nitrite level in ppm (must be 0)
 * @param nitrate_ppm Current nitrate level in ppm (<20 safe)
 * @param hours_since_feed Hours since last feeding
 * @param days_since_clean Days since last tank cleaning
 * @param feeds_per_day Number of scheduled feeds per day
 * @param water_change_interval Days between water changes
 * @param response_buffer Buffer to store AI response (min 256 bytes)
 * @param buffer_size Size of response buffer
 * @return true if successful
 */
bool gemini_query_aquarium(float ammonia_ppm, float nitrite_ppm, float nitrate_ppm, 
                          float hours_since_feed, float days_since_clean,
                          int feeds_per_day, int water_change_interval,
                          char *response_buffer, size_t buffer_size);

#ifdef __cplusplus
}
#endif

#endif // GEMINI_API_H
