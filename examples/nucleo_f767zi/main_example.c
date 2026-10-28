/**
 * @file    main_example.c
 * @brief   Example: NUCLEO-F767ZI + HLW8110, FreeRTOS, log on the ST-Link virtual COM port.
 *
 * CubeMX setup
 *  - USART2 (chip)  : Asynchronous, 9600 baud, Word length **9 bits (includes parity)**,
 *                     Parity **Even**, 1 stop bit, TX/RX, **USART2 global interrupt enabled**,
 *                     NVIC priority number >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY.
 *  - USART3 (log)   : Asynchronous, 115200, 8N1 (PD8/PD9 = ST-Link VCP). No interrupt/DMA needed.
 *  - FreeRTOS       : configUSE_MUTEXES, INCLUDE_vTaskDelete, INCLUDE_vTaskDelayUntil,
 *                     INCLUDE_xTaskGetSchedulerState, INCLUDE_xTaskGetCurrentTaskHandle.
 *
 * Wiring: MCU TX (USART2) -> chip RX (pin 7), MCU RX -> chip TX (pin 6), common GND.
 *
 * Call HLW8110_AppStart() after the kernel is created (e.g. from MX_FREERTOS_Init()).
 */
#include "hlw8110.h"

extern UART_HandleTypeDef huart2;   /* chip */
extern UART_HandleTypeDef huart3;   /* log (ST-Link VCP) */

static HLW8110_Handle_t hlw;        /* static => zero-initialised (required) */

static void HLW8110_AppTask(void *argument);
static void HLW8110_MonitorTask(void *argument);

void HLW8110_AppStart(void)
{
    (void)xTaskCreate(HLW8110_AppTask, "HLW8110App", 512U, NULL, tskIDLE_PRIORITY + 2U, NULL);
}

static void HLW8110_AppTask(void *argument)
{
    (void)argument;

    HLW8110_Config_t cfg;
    (void)HLW8110_ConfigDefault(&cfg, &huart2);

    /* --- Scale factors of THIS board (see docs/hardware_and_calibration.md) --- */
    cfg.k1_current = 1.0f;                                   /* shunt R001 = 1 mOhm            */
    cfg.k2_voltage = HLW8110_CalcK2(1000000.0f, 1650.0f);    /* 5 x 200k on top, 1.65k bottom  */
    /* Optional: fix the frequency reading of the inaccurate internal oscillator. */
    /* cfg.clki_hz = 3632000.0f; */

    /* --- Logging --- */
    cfg.log_huart = &huart3;
    cfg.log_enabled = true;
    cfg.log_level = HLW8110_LOG_INFO;     /* HLW8110_LOG_DEBUG shows every register access */
    cfg.log_measure_every = 5U;           /* one measurement line per 5 reads (~0.75 s)    */

    if (HLW8110_Init(&hlw, &cfg) != HLW8110_OK) {
        vTaskDelete(NULL);
    }

    /* The chip may power up later than the MCU: retry. */
    for (;;) {
        HLW8110_Status_t st = HLW8110_AppInit(&hlw);
        if (st == HLW8110_OK) break;
        if (st == HLW8110_ERR_UNCALIBRATED) {   /* permanent: no factory coefficients */
            vTaskDelete(NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }

    if (HLW8110_StartTask(&hlw, "HLW8110", 1024U, tskIDLE_PRIORITY + 2U,
                          cfg.measurement_period_ticks) != HLW8110_OK) {
        vTaskDelete(NULL);
    }
    (void)xTaskCreate(HLW8110_MonitorTask, "HLW8110Mon", 512U, NULL, tskIDLE_PRIORITY + 1U, NULL);
    vTaskDelete(NULL);
}

/* Forward the HAL callbacks. If another module already owns them, call the
 * HLW8110_UART_* functions from there instead. */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) { HLW8110_UART_RxCpltCallback(&hlw, huart); }
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)  { HLW8110_UART_ErrorCallback(&hlw, huart); }

static void HLW8110_MonitorTask(void *argument)
{
    (void)argument;
    HLW8110_Measurements_t m;
    for (;;) {
        if (HLW8110_GetMeasurements(&hlw, &m) == HLW8110_OK &&
            (TickType_t)(xTaskGetTickCount() - m.timestamp) < pdMS_TO_TICKS(1000U)) {
            /* Fresh data: use m.voltage_v, m.current_a, m.active_power_w, m.power_factor ... */
        } else {
            /* No valid / stale data: show "--" or raise an alarm. */
        }
        vTaskDelay(pdMS_TO_TICKS(500U));
    }
}
