#ifndef SEMPHR_STUB_H
#define SEMPHR_STUB_H
#include "FreeRTOS.h"
typedef struct { int count; int is_mutex; } StubSem;
typedef StubSem *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
void vSemaphoreDelete(SemaphoreHandle_t s);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s, BaseType_t *w);
#endif
