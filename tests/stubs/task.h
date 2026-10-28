#ifndef TASK_STUB_H
#define TASK_STUB_H
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
#define taskSCHEDULER_RUNNING 2
BaseType_t xTaskGetSchedulerState(void);
TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t t);
void vTaskDelayUntil(TickType_t *last, TickType_t inc);
void vTaskDelete(TaskHandle_t t);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
BaseType_t xTaskCreate(TaskFunction_t f, const char *n, uint16_t s, void *a, UBaseType_t p, TaskHandle_t *h);
#endif
