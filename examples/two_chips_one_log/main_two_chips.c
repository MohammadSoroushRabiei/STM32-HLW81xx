/**
 * @file    main_two_chips.c
 * @brief   Example: two HLW8110 chips (one UART each) and ONE shared log UART.
 *
 * CubeMX: USART2 and UART4 for the chips (each 9600, 9 bit incl. parity, Even, 1 stop,
 *         global interrupt enabled, NVIC priority number >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY),
 *         USART3 for the log (115200 8N1, e.g. the ST-Link virtual COM port).
 *
 * Log output (both chips on the same UART, tagged):
 *   [1520][I][PDU1] #5 U=229.87V I=0.412A ...
 *   [1522][I][PDU2] #5 U=230.10V I=1.020A ...
 */
#include "hlw8110.h"

extern UART_HandleTypeDef huart2;   /* chip 1 */
extern UART_HandleTypeDef huart4;   /* chip 2 */
extern UART_HandleTypeDef huart3;   /* log    */

static HLW8110_Handle_t hlw1;       /* static => zero-initialised (required) */
static HLW8110_Handle_t hlw2;

typedef struct {
    HLW8110_Handle_t *dev;
    UART_HandleTypeDef *huart;
    const char *name;               /* log tag and task name */
    float k1, k2;                   /* per-board scale factors */
} ChipInit_t;

static ChipInit_t chip1 = { &hlw1, &huart2, "PDU1", 1.0f, 1.6473f };
static ChipInit_t chip2 = { &hlw2, &huart4, "PDU2", 1.0f, 1.6473f };

static void ChipTask(void *argument)
{
    const ChipInit_t *ci = (const ChipInit_t *)argument;
    HLW8110_Config_t cfg;

    (void)HLW8110_ConfigDefault(&cfg, ci->huart);
    cfg.k1_current = ci->k1;
    cfg.k2_voltage = ci->k2;
    cfg.log_huart = &huart3;              /* the SAME log UART for both chips */
    cfg.log_enabled = true;
    cfg.log_level = HLW8110_LOG_INFO;
    cfg.log_measure_every = 10U;          /* ~1.5 s per chip: keeps the log UART calm */
    cfg.log_name = ci->name;              /* tag in every log line */

    if (HLW8110_Init(ci->dev, &cfg) != HLW8110_OK) vTaskDelete(NULL);

    for (;;) {
        HLW8110_Status_t st = HLW8110_AppInit(ci->dev);
        if (st == HLW8110_OK) break;
        if (st == HLW8110_ERR_UNCALIBRATED) vTaskDelete(NULL);
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
    if (HLW8110_StartTask(ci->dev, ci->name, 1024U, tskIDLE_PRIORITY + 2U,
                          cfg.measurement_period_ticks) != HLW8110_OK) {
        vTaskDelete(NULL);
    }
    vTaskDelete(NULL);   /* the driver task keeps running */
}

void HLW8110_AppStart(void)
{
    (void)xTaskCreate(ChipTask, "Init1", 512U, &chip1, tskIDLE_PRIORITY + 2U, NULL);
    (void)xTaskCreate(ChipTask, "Init2", 512U, &chip2, tskIDLE_PRIORITY + 2U, NULL);
}

/* Forward the HAL callbacks to EVERY instance; each one ignores other UARTs. */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    HLW8110_UART_RxCpltCallback(&hlw1, huart);
    HLW8110_UART_RxCpltCallback(&hlw2, huart);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    HLW8110_UART_ErrorCallback(&hlw1, huart);
    HLW8110_UART_ErrorCallback(&hlw2, huart);
}

/* Reading the results from any task: */
float total_power_w(void)
{
    HLW8110_Measurements_t a, b;
    float sum = 0.0f;
    if (HLW8110_GetMeasurements(&hlw1, &a) == HLW8110_OK) sum += a.active_power_w;
    if (HLW8110_GetMeasurements(&hlw2, &b) == HLW8110_OK) sum += b.active_power_w;
    return sum;
}
