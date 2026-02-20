#ifndef TASK_COORDINATOR_H
#define TASK_COORDINATOR_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Task Coordinator - Step 0 Infrastructure
 * 
 * Creates background tasks and message queues.
 * Tasks are currently stubs (no logic implemented).
 * 
 * NO BEHAVIORAL CHANGES - system functions identically with or without init.
 */

/**
 * @brief Initialize task coordinator (creates tasks and queues)
 * 
 * Called once after LVGL initialization.
 * Safe to call - makes no changes to existing behavior.
 */
void task_coordinator_init(void);

/**
 * Placeholder queues (minimal - will be expanded in later steps)
 * Currently unused - tasks are idle stubs.
 */
extern QueueHandle_t queue_param_update;
extern QueueHandle_t queue_mood_result;
extern QueueHandle_t queue_anim_frame_request;
extern QueueHandle_t queue_anim_frame_ready;
extern QueueHandle_t queue_ai_request;
extern QueueHandle_t queue_ai_result;
extern QueueHandle_t queue_blynk_sync;

#ifdef __cplusplus
}
#endif

#endif // TASK_COORDINATOR_H
