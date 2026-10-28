#ifndef FREERTOS_STUB_H
#define FREERTOS_STUB_H
#include <stdint.h>
typedef uint32_t TickType_t;
typedef int32_t BaseType_t;
typedef uint32_t UBaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define tskIDLE_PRIORITY 0U
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL()  ((void)0)
#define portYIELD_FROM_ISR(x) ((void)(x))
#endif
