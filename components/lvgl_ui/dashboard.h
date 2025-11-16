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
 * @brief Update sensor 1 gauge value
 * @param value Sensor value (0-100)
 */
void dashboard_update_sensor1(float value);

/**
 * @brief Update sensor 2 gauge value
 * @param value Sensor value (0-100)
 */
void dashboard_update_sensor2(float value);

#ifdef __cplusplus
}
#endif

#endif // __DASHBOARD_H__
