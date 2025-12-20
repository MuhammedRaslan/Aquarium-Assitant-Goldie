#ifndef __DASHBOARD_H__
#define __DASHBOARD_H__

#include <stdio.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the IoT dashboard with animation and gauges
 */
void dashboard_init(void);

/**
 * @brief Update sensor 1 gauge value (Temperature in Celsius)
 * @param value Temperature value (0-100)
 */
void dashboard_update_sensor1(float value);

/**
 * @brief Update sensor 2 gauge value (Oxygen level in mg/L)
 * @param value Oxygen value (0-100)
 */
void dashboard_update_sensor2(float value);

/**
 * @brief Update pH level
 * @param value pH value (0-14)
 */
void dashboard_update_ph(float value);

/**
 * @brief Get feed log for a specific day
 * @param day Day index (0-6 for last 7 days)
 * @return Number of feed events
 */
uint32_t dashboard_get_feed_log(uint8_t day);

/**
 * @brief Get water cleaning log for a specific day
 * @param day Day index (0-6 for last 7 days)
 * @return Number of water cleaning events
 */
uint32_t dashboard_get_water_log(uint8_t day);

/**
 * @brief Print all logs to console
 */
void dashboard_print_logs(void);

/**
 * @brief Set animation category (Happy, Sad, or Angry)
 * @param category Category to switch to (0=Happy, 1=Sad, 2=Angry)
 */
void dashboard_set_animation_category(uint8_t category);

/**
 * @brief Get current animation category
 * @return Current category (0=Happy, 1=Sad, 2=Angry)
 */
uint8_t dashboard_get_animation_category(void);

/**
 * @brief Update calendar display with current date/time
 */
void dashboard_update_calendar(void);

#ifdef __cplusplus
}
#endif

#endif // __DASHBOARD_H__
