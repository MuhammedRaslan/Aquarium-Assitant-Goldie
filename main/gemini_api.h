#ifndef GEMINI_API_H
#define GEMINI_API_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize WiFi and connect to network
 * @return true if connected successfully
 */
bool gemini_init_wifi(void);

/**
 * @brief Query Gemini API with aquarium parameters
 * @param temperature Current temperature in Celsius
 * @param oxygen Current oxygen level in mg/L
 * @param ph Current pH level
 * @param hours_since_feed Hours since last feeding
 * @param days_since_clean Days since last tank cleaning
 * @param response_buffer Buffer to store AI response (min 256 bytes)
 * @param buffer_size Size of response buffer
 * @return true if successful
 */
bool gemini_query_aquarium(float temperature, float oxygen, float ph, 
                          float hours_since_feed, float days_since_clean,
                          char *response_buffer, size_t buffer_size);

#ifdef __cplusplus
}
#endif

#endif // GEMINI_API_H
