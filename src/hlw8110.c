/**
 * @file    hlw8110.c
 * @brief   HLW8110/HLW8112 UART driver implementation.
 * @details Public API and documentation: hlw8110.h.
 *
 * Internal structure:
 *  - helpers      : checksum, delays, mutex wrappers
 *  - logging      : optional text log on a second UART
 *  - transport    : one UART transaction (RX armed before TX, interrupt driven)
 *  - registers    : ReadReg / WriteReg with write-protect handling
 *  - configuration: coefficients, SYSCON/EMUCON2, checksum baseline, recovery
 *  - measurements : raw -> engineering units (datasheet chapter 10)
 *  - task         : periodic reads + health checks
 */
#include "hlw8110.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#define HLW8110_CFG_LOCK_MS 2000U

/* ------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* ------------------------------------------------------------------------- */

static uint8_t checksum8(const uint8_t *bytes, size_t count)
{
    uint8_t sum = 0U;
    for (size_t i = 0U; i < count; ++i) sum = (uint8_t)(sum + bytes[i]);
    return (uint8_t)(~sum);
}

static bool scheduler_running(void)
{
    return xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
}

static void delay_ms(uint32_t ms)
{
    if (scheduler_running()) vTaskDelay(pdMS_TO_TICKS(ms));
    else HAL_Delay(ms);
}

static HLW8110_Status_t lock_dev(HLW8110_Handle_t *d)
{
    if (!d || !d->mutex) return HLW8110_ERR_MUTEX;
    TickType_t wait = pdMS_TO_TICKS(d->cfg.uart_timeout_ms * 2U + 20U);
    if (wait == 0U) wait = 1U;
    return (xSemaphoreTake(d->mutex, wait) == pdTRUE) ? HLW8110_OK : HLW8110_ERR_MUTEX;
}

static void unlock_dev(HLW8110_Handle_t *d)
{
    (void)xSemaphoreGive(d->mutex);
}

static HLW8110_Status_t lock_cfg(HLW8110_Handle_t *d)
{
    if (!d || !d->config_mutex) return HLW8110_ERR_MUTEX;
    return (xSemaphoreTake(d->config_mutex, pdMS_TO_TICKS(HLW8110_CFG_LOCK_MS)) == pdTRUE)
        ? HLW8110_OK : HLW8110_ERR_MUTEX;
}

static void unlock_cfg(HLW8110_Handle_t *d)
{
    (void)xSemaphoreGive(d->config_mutex);
}

/* ------------------------------------------------------------------------- */
/* Logging                                                                    */
/* ------------------------------------------------------------------------- */

#define HLW8110_LOG_LINE_MAX 160U

#if HLW8110_LOG_ENABLE
#define LOGE(d, ...) HLW8110_Log((d), HLW8110_LOG_ERROR, __VA_ARGS__)
#define LOGW(d, ...) HLW8110_Log((d), HLW8110_LOG_WARN,  __VA_ARGS__)
#define LOGI(d, ...) HLW8110_Log((d), HLW8110_LOG_INFO,  __VA_ARGS__)
#define LOGD(d, ...) HLW8110_Log((d), HLW8110_LOG_DEBUG, __VA_ARGS__)
#else
#define LOGE(d, ...) ((void)0)
#define LOGW(d, ...) ((void)0)
#define LOGI(d, ...) ((void)0)
#define LOGD(d, ...) ((void)0)
#endif

/* One log mutex shared by ALL driver instances so that several chips can
 * use the same log UART. Reference counted; created by the first Init and
 * deleted by the last Deinit. */
static SemaphoreHandle_t s_log_mutex;
static uint8_t s_log_users;

static bool log_mutex_acquire(void)
{
    SemaphoreHandle_t fresh;
    taskENTER_CRITICAL();
    if (s_log_users > 0U) { s_log_users++; taskEXIT_CRITICAL(); return true; }
    taskEXIT_CRITICAL();

    fresh = xSemaphoreCreateMutex();
    if (!fresh) return false;
    taskENTER_CRITICAL();
    if (s_log_users == 0U) { s_log_mutex = fresh; fresh = NULL; }
    s_log_users++;
    taskEXIT_CRITICAL();
    if (fresh) vSemaphoreDelete(fresh); /* another instance won the race */
    return true;
}

static void log_mutex_release(void)
{
    SemaphoreHandle_t doomed = NULL;
    taskENTER_CRITICAL();
    if (s_log_users > 0U && --s_log_users == 0U) { doomed = s_log_mutex; s_log_mutex = NULL; }
    taskEXIT_CRITICAL();
    if (doomed) vSemaphoreDelete(doomed);
}

const char *HLW8110_StatusToString(HLW8110_Status_t st)
{
    switch (st) {
    case HLW8110_OK:                return "OK";
    case HLW8110_ERR_ARG:           return "ERR_ARG";
    case HLW8110_ERR_HAL:           return "ERR_HAL";
    case HLW8110_ERR_TIMEOUT:       return "ERR_TIMEOUT";
    case HLW8110_ERR_MUTEX:         return "ERR_MUTEX";
    case HLW8110_ERR_CHECKSUM:      return "ERR_CHECKSUM";
    case HLW8110_ERR_NOT_READY:     return "ERR_NOT_READY";
    case HLW8110_ERR_NOMEM:         return "ERR_NOMEM";
    case HLW8110_ERR_UNCALIBRATED:  return "ERR_UNCALIBRATED";
    case HLW8110_ERR_UNSUPPORTED:   return "ERR_UNSUPPORTED";
    case HLW8110_ERR_BUSY:          return "ERR_BUSY";
    case HLW8110_ERR_CONFIG_MISMATCH: return "ERR_CONFIG_MISMATCH";
    default:                        return "?";
    }
}

void HLW8110_Log(HLW8110_Handle_t *d, HLW8110_LogLevel_t level, const char *fmt, ...)
{
    static const char tag[] = {'-', 'E', 'W', 'I', 'D'};
    char line[HLW8110_LOG_LINE_MAX];
    va_list ap;
    int n;
    size_t len;

    if (!d || !d->initialized || !d->log_enabled || !d->cfg.log_huart || !fmt) return;
    if (level == HLW8110_LOG_NONE || level > HLW8110_LOG_DEBUG || (uint8_t)level > d->cfg.log_level) return;

    if (d->log_name[0] != '\0')
        n = snprintf(line, sizeof line, "[%lu][%c][%s] ", (unsigned long)xTaskGetTickCount(), tag[level], d->log_name);
    else
        n = snprintf(line, sizeof line, "[%lu][%c] ", (unsigned long)xTaskGetTickCount(), tag[level]);
    if (n < 0) return;
    len = (size_t)n;
    if (len < sizeof line) {
        va_start(ap, fmt);
        n = vsnprintf(line + len, sizeof line - len, fmt, ap);
        va_end(ap);
        if (n > 0) len += (size_t)n;
    }
    if (len > sizeof line - 3U) len = sizeof line - 3U; /* truncated: keep room for CRLF */
    line[len++] = '\r';
    line[len++] = '\n';

    if (!d->log_ref || !s_log_mutex) return;
    {
        TickType_t wait = scheduler_running() ? pdMS_TO_TICKS(100U) : 0U;
        if (xSemaphoreTake(s_log_mutex, wait) != pdTRUE) return;
    }
    (void)HAL_UART_Transmit(d->cfg.log_huart, (uint8_t *)line, (uint16_t)len, 100U);
    (void)xSemaphoreGive(s_log_mutex);
}

void HLW8110_LogEnable(HLW8110_Handle_t *d, bool enable)
{
    if (!d || !d->initialized) return;
    d->log_enabled = enable && (d->cfg.log_huart != NULL);
}

void HLW8110_LogSetUart(HLW8110_Handle_t *d, UART_HandleTypeDef *huart)
{
    if (!d || !d->initialized) return;
    bool locked = (d->log_ref && s_log_mutex &&
                   xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(200U)) == pdTRUE);
    d->cfg.log_huart = huart;
    if (!huart) d->log_enabled = false;
    if (locked) (void)xSemaphoreGive(s_log_mutex);
}

void HLW8110_LogSetLevel(HLW8110_Handle_t *d, HLW8110_LogLevel_t level)
{
    if (!d || !d->initialized || level > HLW8110_LOG_DEBUG) return;
    d->cfg.log_level = (uint8_t)level;
}

/** Fixed-point float printing (works without -u _printf_float). */
static void fx(char *out, size_t n, float v, uint8_t dec)
{
    uint32_t scale = 1U;
    char frac[6];
    bool neg = (v < 0.0f);
    if (dec > 4U) dec = 4U;
    for (uint8_t k = 0U; k < dec; ++k) scale *= 10U;
    if (neg) v = -v;
    uint32_t sc = (uint32_t)(v * (float)scale + 0.5f);
    uint32_t ip = sc / scale, fp = sc % scale;
    for (int k = (int)dec - 1; k >= 0; --k) { frac[k] = (char)('0' + (fp % 10U)); fp /= 10U; }
    frac[dec] = '\0';
    (void)snprintf(out, n, "%s%lu%s%s", neg ? "-" : "", (unsigned long)ip, dec ? "." : "", frac);
}

void HLW8110_LogMeasurements(HLW8110_Handle_t *d, const HLW8110_Measurements_t *m)
{
    char u[16], i[16], p[16], s[16], pf[12], f[12], a[12];
    if (!d || !m || !d->log_enabled) return;
    fx(u, sizeof u, m->voltage_v, 2U);
    fx(i, sizeof i, m->current_a, 3U);
    fx(p, sizeof p, m->active_power_w, 2U);
    fx(s, sizeof s, m->apparent_power_va, 2U);
    fx(pf, sizeof pf, m->power_factor, 3U);
    fx(f, sizeof f, m->frequency_hz, 2U);
    fx(a, sizeof a, m->phase_deg, 1U);
    HLW8110_Log(d, HLW8110_LOG_INFO, "#%lu U=%sV I=%sA P=%sW S=%sVA PF=%s f=%sHz ang=%s",
                (unsigned long)m->sequence, u, i, p, s, pf, f, a);
}

static void uart_flush_errors(UART_HandleTypeDef *huart)
{
    if (!huart) return;
    __HAL_UART_CLEAR_OREFLAG(huart);
    __HAL_UART_CLEAR_FEFLAG(huart);
    __HAL_UART_CLEAR_NEFLAG(huart);
    __HAL_UART_CLEAR_PEFLAG(huart);
#if defined(__HAL_UART_FLUSH_DRREGISTER)
    __HAL_UART_FLUSH_DRREGISTER(huart);
#else
    (void)huart->Instance->RDR;
#endif
}

static HLW8110_Status_t tx_blocking(HLW8110_Handle_t *d, const uint8_t *data, uint16_t len)
{
    HAL_StatusTypeDef s = HAL_UART_Transmit(d->cfg.huart, (uint8_t *)data, len, d->cfg.uart_timeout_ms);
    if (s == HAL_OK) return HLW8110_OK;
    return (s == HAL_TIMEOUT) ? HLW8110_ERR_TIMEOUT : HLW8110_ERR_HAL;
}

/** Polling RX (used only when use_rx_it == false). */
static HLW8110_Status_t rx_blocking(HLW8110_Handle_t *d, uint8_t *data, uint16_t len)
{
    HAL_StatusTypeDef s = HAL_UART_Receive(d->cfg.huart, data, len, d->cfg.uart_timeout_ms);
    if (s == HAL_OK) return HLW8110_OK;
    (void)HAL_UART_AbortReceive(d->cfg.huart);
    uart_flush_errors(d->cfg.huart);
    return (s == HAL_TIMEOUT) ? HLW8110_ERR_TIMEOUT : HLW8110_ERR_HAL;
}

/**
 * Interrupt-driven transaction: RX is armed BEFORE the request is sent, so the
 * first response byte cannot be missed. Caller holds the bus mutex.
 */
static HLW8110_Status_t transact_it(HLW8110_Handle_t *d, const uint8_t *req, uint16_t req_len,
                                    uint8_t *rx, uint16_t rx_len)
{
    if (!d->rx_done || !scheduler_running()) return HLW8110_ERR_NOT_READY;

    (void)xSemaphoreTake(d->rx_done, 0U);
    d->rx_error = false;
    d->rx_active = true;

    HLW8110_Status_t st = (HAL_UART_Receive_IT(d->cfg.huart, rx, rx_len) == HAL_OK)
                              ? HLW8110_OK : HLW8110_ERR_HAL;
    if (st == HLW8110_OK) st = tx_blocking(d, req, req_len);
    if (st == HLW8110_OK) {
        TickType_t wait = pdMS_TO_TICKS(d->cfg.uart_timeout_ms);
        if (wait == 0U) wait = 1U;
        if (xSemaphoreTake(d->rx_done, wait) != pdTRUE) st = HLW8110_ERR_TIMEOUT;
        else if (d->rx_error) st = HLW8110_ERR_HAL;
    }
    d->rx_active = false;
    if (st != HLW8110_OK) {
        (void)HAL_UART_AbortReceive(d->cfg.huart);
        uart_flush_errors(d->cfg.huart);
    }
    return st;
}

static HLW8110_Status_t send_frame(HLW8110_Handle_t *d, const uint8_t *payload, uint8_t payload_len)
{
    uint8_t frame[7];
    if (!d || !payload || payload_len > 5U) return HLW8110_ERR_ARG;
    frame[0] = HLW8110_UART_HEADER;
    memcpy(&frame[1], payload, payload_len);
    frame[payload_len + 1U] = checksum8(frame, (size_t)payload_len + 1U);
    return tx_blocking(d, frame, (uint16_t)payload_len + 2U);
}

static bool is_write_protected(uint8_t reg)
{
    return ((reg <= 0x1FU) || (reg == HLW8110_REG_PFCNT_PA) ||
            (reg == HLW8110_REG_PFCNT_PB) || (reg == HLW8110_REG_IE));
}

static HLW8110_Status_t special_cmd_locked(HLW8110_Handle_t *d, uint8_t command)
{
    const uint8_t p[] = {HLW8110_CMD_SPECIAL, command};
    return send_frame(d, p, 2U);
}

static void invalidate_measurement(HLW8110_Handle_t *d)
{
    taskENTER_CRITICAL();
    d->measurement.valid = false;
    taskEXIT_CRITICAL();
}

int32_t HLW8110_Signed24(uint32_t raw)
{
    raw &= 0xFFFFFFU;
    return (raw & 0x800000U) ? (int32_t)(raw | 0xFF000000U) : (int32_t)raw;
}

float HLW8110_PowerFactorFromRaw(uint32_t raw_pf)
{
    float pf = (float)HLW8110_Signed24(raw_pf) / 8388607.0f;
    if (pf > 1.0f) pf = 1.0f;
    if (pf < -1.0f) pf = -1.0f;
    return pf;
}

float HLW8110_CalcK2(float r_top_ohm, float r_sense_ohm)
{
    if (r_top_ohm < 0.0f || r_sense_ohm <= 0.0f) return 0.0f;
    return 1000.0f * r_sense_ohm / (r_top_ohm + r_sense_ohm);
}

float HLW8110_CorrectK(float k_old, float displayed, float actual)
{
    if (k_old <= 0.0f || displayed <= 0.0f || actual <= 0.0f) return k_old;
    return k_old * displayed / actual;
}

HLW8110_Status_t HLW8110_ConfigDefault(HLW8110_Config_t *cfg, UART_HandleTypeDef *huart)
{
    if (!cfg || !huart) return HLW8110_ERR_ARG;
    memset(cfg, 0, sizeof(*cfg));
    cfg->huart = huart;
    cfg->uart_timeout_ms = 20U;
    cfg->k1_current = 1.0f;     /* placeholder: set from your shunt */
    cfg->k2_voltage = 1.0f;     /* placeholder: set from your divider */
    cfg->clki_hz = 3579545.0f;
    cfg->variant = HLW8110_VARIANT_HLW8110;
    cfg->use_rx_it = true;
    cfg->configure_emucon2 = true;
    cfg->enable_channel_b = false;
    cfg->dupsel = 1U;           /* 6.8 Hz */
    cfg->measurement_period_ticks = pdMS_TO_TICKS(150U);
    cfg->max_consecutive_errors = 5U;
    cfg->config_check_period_ticks = pdMS_TO_TICKS(5000U);
    cfg->log_huart = NULL;
    cfg->log_enabled = false;
    cfg->log_level = (uint8_t)HLW8110_LOG_INFO;
    cfg->log_measure_every = 0U;
    return HLW8110_OK;
}

/* ------------------------------------------------------------------------- */
/* Init / register access                                                     */
/* ------------------------------------------------------------------------- */

HLW8110_Status_t HLW8110_Init(HLW8110_Handle_t *d, const HLW8110_Config_t *cfg)
{
    if (!d || !cfg || !cfg->huart) return HLW8110_ERR_ARG;
    if (d->initialized) return HLW8110_ERR_BUSY;
    if (cfg->k1_current <= 0.0f || cfg->k2_voltage <= 0.0f) return HLW8110_ERR_ARG;
    if (cfg->dupsel > 3U) return HLW8110_ERR_ARG;
    if (cfg->variant != HLW8110_VARIANT_HLW8110 && cfg->variant != HLW8110_VARIANT_HLW8112)
        return HLW8110_ERR_ARG;

    memset(d, 0, sizeof(*d));
    d->cfg = *cfg;
    if (d->cfg.uart_timeout_ms == 0U) d->cfg.uart_timeout_ms = 100U;
    if (d->cfg.clki_hz <= 0.0f) d->cfg.clki_hz = 3579545.0f;
    if (d->cfg.measurement_period_ticks == 0U) d->cfg.measurement_period_ticks = pdMS_TO_TICKS(100U);
    if (d->cfg.max_consecutive_errors == 0U) d->cfg.max_consecutive_errors = 5U;
    if (d->cfg.config_check_period_ticks == 0U) d->cfg.config_check_period_ticks = pdMS_TO_TICKS(5000U);

    d->mutex = xSemaphoreCreateMutex();
    d->config_mutex = xSemaphoreCreateMutex();
    d->rx_done = xSemaphoreCreateBinary();
    d->log_ref = log_mutex_acquire();
    if (!d->mutex || !d->config_mutex || !d->rx_done || !d->log_ref) {
        if (d->mutex) vSemaphoreDelete(d->mutex);
        if (d->config_mutex) vSemaphoreDelete(d->config_mutex);
        if (d->rx_done) vSemaphoreDelete(d->rx_done);
        if (d->log_ref) log_mutex_release();
        d->log_ref = false;
        d->mutex = NULL;
        d->config_mutex = NULL;
        d->rx_done = NULL;
        return HLW8110_ERR_NOMEM;
    }
    if (d->cfg.log_level == (uint8_t)HLW8110_LOG_NONE) d->cfg.log_level = (uint8_t)HLW8110_LOG_INFO;
    if (d->cfg.log_level > (uint8_t)HLW8110_LOG_DEBUG) d->cfg.log_level = (uint8_t)HLW8110_LOG_DEBUG;
    d->log_enabled = d->cfg.log_enabled && (d->cfg.log_huart != NULL);
    if (d->cfg.log_name) {
        strncpy(d->log_name, d->cfg.log_name, sizeof d->log_name - 1U);
        d->log_name[sizeof d->log_name - 1U] = '\0';
        d->cfg.log_name = d->log_name;  /* do not keep a pointer to the caller's string */
    }
    d->initialized = true;
    d->last_status = HLW8110_OK;
    d->measurement.valid = false;
    LOGI(d, "HLW8110 driver init: variant=%s uart_timeout=%lums dupsel=%u",
         (d->cfg.variant == HLW8110_VARIANT_HLW8112) ? "HLW8112" : "HLW8110",
         (unsigned long)d->cfg.uart_timeout_ms, (unsigned)d->cfg.dupsel);
    return HLW8110_OK;
}

HLW8110_Status_t HLW8110_Deinit(HLW8110_Handle_t *d)
{
    if (!d || !d->initialized) return HLW8110_ERR_ARG;
    if (d->task) {
        HLW8110_Status_t st = HLW8110_StopTask(d, pdMS_TO_TICKS(1000U));
        if (st != HLW8110_OK) return st;
    }
    d->initialized = false;
    if (d->mutex) vSemaphoreDelete(d->mutex);
    if (d->config_mutex) vSemaphoreDelete(d->config_mutex);
    if (d->rx_done) vSemaphoreDelete(d->rx_done);
    if (d->log_ref) log_mutex_release();
    memset(d, 0, sizeof(*d));
    return HLW8110_OK;
}

HLW8110_Status_t HLW8110_ReadReg(HLW8110_Handle_t *d, uint8_t reg, uint32_t *value, uint8_t len)
{
    uint8_t request[2];
    uint8_t rxbuf[5] = {0};
    uint8_t checkbuf[6] = {0};
    HLW8110_Status_t st;

    if (!d || !d->initialized || !value || len < 1U || len > 4U || reg > 0x7FU) return HLW8110_ERR_ARG;
    st = lock_dev(d);
    if (st != HLW8110_OK) return st;

    request[0] = HLW8110_UART_HEADER;
    request[1] = (uint8_t)(reg & 0x7FU);

    uart_flush_errors(d->cfg.huart);
    if (d->cfg.use_rx_it) {
        st = transact_it(d, request, 2U, rxbuf, (uint16_t)len + 1U);
    } else {
        st = tx_blocking(d, request, 2U);
        if (st == HLW8110_OK) st = rx_blocking(d, rxbuf, (uint16_t)len + 1U);
    }

    if (st == HLW8110_OK) {
        checkbuf[0] = HLW8110_UART_HEADER;
        checkbuf[1] = request[1];
        memcpy(&checkbuf[2], rxbuf, len);
        if (checksum8(checkbuf, (size_t)len + 2U) != rxbuf[len]) st = HLW8110_ERR_CHECKSUM;
    }
    if (st == HLW8110_OK) {
        uint32_t v = 0U;
        for (uint8_t i = 0U; i < len; ++i) v = (v << 8) | rxbuf[i];
        *value = v;
    }

    unlock_dev(d);
    d->last_status = st;
    if (st == HLW8110_OK) LOGD(d, "RD reg=0x%02X len=%u val=0x%lX", (unsigned)reg, (unsigned)len, (unsigned long)*value);
    else LOGW(d, "RD reg=0x%02X failed: %s", (unsigned)reg, HLW8110_StatusToString(st));
    return st;
}

HLW8110_Status_t HLW8110_WriteReg(HLW8110_Handle_t *d, uint8_t reg, uint32_t value, uint8_t len)
{
    uint8_t payload[5] = {0};
    HLW8110_Status_t st;
    bool protected_reg;

    if (!d || !d->initialized || len < 1U || len > 4U || reg > 0x7FU) return HLW8110_ERR_ARG;
    st = lock_dev(d);
    if (st != HLW8110_OK) return st;

    protected_reg = is_write_protected(reg);
    if (protected_reg) st = special_cmd_locked(d, HLW8110_CMD_WRITE_ENABLE);

    if (st == HLW8110_OK) {
        payload[0] = (uint8_t)(0x80U | reg);
        for (uint8_t i = 0U; i < len; ++i)
            payload[1U + i] = (uint8_t)(value >> (8U * (len - 1U - i)));
        st = send_frame(d, payload, (uint8_t)(len + 1U));

        if (protected_reg) {
            HLW8110_Status_t disable_st = special_cmd_locked(d, HLW8110_CMD_WRITE_DISABLE);
            if (st == HLW8110_OK) st = disable_st;
        }
    }

    unlock_dev(d);
    d->last_status = st;
    if (st == HLW8110_OK) LOGD(d, "WR reg=0x%02X len=%u val=0x%lX", (unsigned)reg, (unsigned)len, (unsigned long)value);
    else LOGW(d, "WR reg=0x%02X failed: %s", (unsigned)reg, HLW8110_StatusToString(st));
    return st;
}

HLW8110_Status_t HLW8110_Reset(HLW8110_Handle_t *d)
{
    HLW8110_Status_t st;
    if (!d || !d->initialized) return HLW8110_ERR_ARG;
    st = lock_dev(d);
    if (st != HLW8110_OK) return st;
    st = special_cmd_locked(d, HLW8110_CMD_RESET);
    unlock_dev(d);
    if (st == HLW8110_OK) delay_ms(70U);
    d->last_status = st;
    if (st == HLW8110_OK) LOGI(d, "chip reset command sent");
    else LOGE(d, "chip reset failed: %s", HLW8110_StatusToString(st));
    return st;
}

HLW8110_Status_t HLW8110_LoadCoefficients(HLW8110_Handle_t *d)
{
    uint32_t v = 0U;
    HLW8110_Status_t st;
    if (!d || !d->initialized) return HLW8110_ERR_ARG;

#define READ16(REG, FIELD) do { st = HLW8110_ReadReg(d, (REG), &v, 2U); \
                                if (st != HLW8110_OK) return st; \
                                d->FIELD = (uint16_t)v; } while (0)
    READ16(HLW8110_REG_RMSIA_C, coeff_rms_ia);
    READ16(HLW8110_REG_RMSIB_C, coeff_rms_ib);
    READ16(HLW8110_REG_RMSU_C, coeff_rms_u);
    READ16(HLW8110_REG_POWERPA_C, coeff_power_pa);
    READ16(HLW8110_REG_POWERPB_C, coeff_power_pb);
    READ16(HLW8110_REG_POWERS_C, coeff_power_s);
    READ16(HLW8110_REG_ENERGYA_C, coeff_energy_a);
    READ16(HLW8110_REG_ENERGYB_C, coeff_energy_b);
    READ16(HLW8110_REG_CHECKSUM, coeff_checksum);
#undef READ16

    d->coefficients_valid = (d->coeff_rms_ia != 0xFFFFU &&
                             d->coeff_rms_u != 0xFFFFU &&
                             d->coeff_power_pa != 0xFFFFU &&
                             d->coeff_power_s != 0xFFFFU &&
                             (d->cfg.variant == HLW8110_VARIANT_HLW8110 ||
                              (d->coeff_rms_ib != 0xFFFFU && d->coeff_power_pb != 0xFFFFU)));
    LOGI(d, "coeff: RMS_IA=0x%04X RMS_U=0x%04X P_A=0x%04X P_S=0x%04X chk=0x%04X",
         (unsigned)d->coeff_rms_ia, (unsigned)d->coeff_rms_u, (unsigned)d->coeff_power_pa,
         (unsigned)d->coeff_power_s, (unsigned)d->coeff_checksum);
    if (!d->coefficients_valid) LOGE(d, "chip is not factory calibrated (coefficients 0xFFFF)");
    return d->coefficients_valid ? HLW8110_OK : HLW8110_ERR_UNCALIBRATED;
}

/* ------------------------------------------------------------------------- */
/* Configuration / health checks                                              */
/* ------------------------------------------------------------------------- */

HLW8110_Status_t HLW8110_CheckCalibration(HLW8110_Handle_t *d)
{
    uint32_t v = 0U;
    HLW8110_Status_t st;
    if (!d || !d->initialized) return HLW8110_ERR_ARG;
    st = HLW8110_ReadReg(d, HLW8110_REG_EMUSTATUS, &v, 3U);
    if (st != HLW8110_OK) return st;
    d->config_checksum = (uint16_t)(v & 0xFFFFU);
    if ((v & (1UL << 16)) != 0U) return HLW8110_ERR_BUSY; /* ChksumBusy */
    if (d->config_checksum_valid && d->config_checksum != d->expected_config_checksum)
        return HLW8110_ERR_CONFIG_MISMATCH;
    return HLW8110_OK;
}

static HLW8110_Status_t check_calibration_retry(HLW8110_Handle_t *d, uint8_t attempts, uint32_t delay)
{
    HLW8110_Status_t st = HLW8110_ERR_BUSY;
    for (uint8_t i = 0U; i < attempts; ++i) {
        st = HLW8110_CheckCalibration(d);
        if (st != HLW8110_ERR_BUSY) return st;
        delay_ms(delay);
    }
    return st;
}

/** Reading SYSSTATUS returns RST (bit0) and clears it. */
static HLW8110_Status_t check_reset_flag(HLW8110_Handle_t *d, bool *reset_seen)
{
    uint32_t v = 0U;
    HLW8110_Status_t st = HLW8110_ReadReg(d, HLW8110_REG_SYSSTATUS, &v, 1U);
    if (st == HLW8110_OK && reset_seen) *reset_seen = ((v & 0x01U) != 0U);
    return st;
}

/** Write SYSCON/EMUCON2 and verify by read-back. Caller holds config_mutex. */
static HLW8110_Status_t configure_locked(HLW8110_Handle_t *d)
{
    HLW8110_Status_t st;
    uint32_t v = 0U;
    uint32_t rb = 0U;
    const bool chb = (d->cfg.variant == HLW8110_VARIANT_HLW8112) && d->cfg.enable_channel_b;

    /* ---- EMUCON2 ---- */
    uint32_t emu_mask = HLW8110_EMUCON2_DUPSEL_MASK | HLW8110_EMUCON2_PFACTOR_EN |
                        HLW8110_EMUCON2_WAVE_EN | HLW8110_EMUCON2_ZX_EN | HLW8110_EMUCON2_CHS_IB;
    uint32_t emu_val = ((uint32_t)d->cfg.dupsel << 8) | HLW8110_EMUCON2_PFACTOR_EN |
                       HLW8110_EMUCON2_WAVE_EN | HLW8110_EMUCON2_ZX_EN;
    if (chb) emu_val |= HLW8110_EMUCON2_CHS_IB;

    st = HLW8110_ReadReg(d, HLW8110_REG_EMUCON2, &v, 2U);
    if (st != HLW8110_OK) return st;
    v = (v & ~emu_mask) | emu_val;
    st = HLW8110_WriteReg(d, HLW8110_REG_EMUCON2, v, 2U);
    if (st != HLW8110_OK) return st;
    st = HLW8110_ReadReg(d, HLW8110_REG_EMUCON2, &rb, 2U);
    if (st != HLW8110_OK) return st;
    if ((rb & emu_mask) != emu_val) {
        LOGE(d, "EMUCON2 read-back mismatch: 0x%04lX (expected 0x%04lX)", (unsigned long)rb, (unsigned long)emu_val);
        return HLW8110_ERR_HAL;
    }
    LOGI(d, "configured: EMUCON2=0x%04lX (read-back OK)", (unsigned long)rb);

    /* ---- SYSCON: PGAIA=16, PGAU=1, ADC1/ADC3 on, ADC2 (+PGAIB=16) only for ch.B ---- */
    uint32_t sys_mask = HLW8110_SYSCON_PGAIA_MASK | HLW8110_SYSCON_PGAU_MASK |
                        HLW8110_SYSCON_ADC1ON | HLW8110_SYSCON_ADC2ON | HLW8110_SYSCON_ADC3ON;
    uint32_t sys_val = HLW8110_SYSCON_ADC1ON | HLW8110_SYSCON_ADC3ON | (4UL << 0);
    if (chb) {
        sys_mask |= HLW8110_SYSCON_PGAIB_MASK;
        sys_val |= HLW8110_SYSCON_ADC2ON | (4UL << 6);
    }

    st = HLW8110_ReadReg(d, HLW8110_REG_SYSCON, &v, 2U);
    if (st != HLW8110_OK) return st;
    v = (v & ~sys_mask) | sys_val;
    st = HLW8110_WriteReg(d, HLW8110_REG_SYSCON, v, 2U);
    if (st != HLW8110_OK) return st;
    st = HLW8110_ReadReg(d, HLW8110_REG_SYSCON, &rb, 2U);
    if (st != HLW8110_OK) return st;
    if ((rb & sys_mask) != sys_val) {
        LOGE(d, "SYSCON read-back mismatch: 0x%04lX (expected 0x%04lX)", (unsigned long)rb, (unsigned long)sys_val);
        return HLW8110_ERR_HAL;
    }
    LOGI(d, "configured: SYSCON=0x%04lX (read-back OK)", (unsigned long)rb);
    return HLW8110_OK;
}

/**
 * (Re)apply configuration and take a new config-checksum baseline.
 * Caller holds config_mutex.
 */
static HLW8110_Status_t reconfigure_locked(HLW8110_Handle_t *d, bool load_coeffs)
{
    HLW8110_Status_t st;
    d->config_checksum_valid = false;

    if (load_coeffs) {
        st = HLW8110_LoadCoefficients(d);
        if (st != HLW8110_OK) return st;
    }
    if (d->cfg.configure_emucon2) {
        st = configure_locked(d);
        if (st != HLW8110_OK) return st;
    }
    st = check_calibration_retry(d, 5U, 2U);
    if (st == HLW8110_OK) {
        d->expected_config_checksum = d->config_checksum;
        d->config_checksum_valid = true;
        LOGI(d, "config checksum baseline = 0x%04X", (unsigned)d->config_checksum);
    } else {
        LOGW(d, "config checksum baseline failed: %s", HLW8110_StatusToString(st));
    }
    (void)check_reset_flag(d, NULL); /* clear RST so it is not seen again */
    return st;
}

HLW8110_Status_t HLW8110_Configure(HLW8110_Handle_t *d)
{
    HLW8110_Status_t st;
    if (!d || !d->initialized) return HLW8110_ERR_ARG;
    st = lock_cfg(d);
    if (st != HLW8110_OK) return st;
    st = configure_locked(d);
    unlock_cfg(d);
    return st;
}

HLW8110_Status_t HLW8110_AppInit(HLW8110_Handle_t *d)
{
    HLW8110_Status_t st;
    if (!d || !d->initialized) return HLW8110_ERR_ARG;
    st = lock_cfg(d);
    if (st != HLW8110_OK) return st;

    delay_ms(100U); /* oscillator start-up after power-on */
    st = HLW8110_Reset(d);
    if (st == HLW8110_OK) st = reconfigure_locked(d, true);

    unlock_cfg(d);
    if (st == HLW8110_OK) LOGI(d, "AppInit OK");
    else LOGE(d, "AppInit failed: %s", HLW8110_StatusToString(st));
    return st;
}

HLW8110_Status_t HLW8110_SelectCurrentChannel(HLW8110_Handle_t *d, bool channel_b)
{
    HLW8110_Status_t st;
    if (!d || !d->initialized) return HLW8110_ERR_ARG;
    if (channel_b && d->cfg.variant == HLW8110_VARIANT_HLW8110) return HLW8110_ERR_UNSUPPORTED;
    st = lock_dev(d);
    if (st != HLW8110_OK) return st;
    st = special_cmd_locked(d, channel_b ? HLW8110_CMD_SELECT_B : HLW8110_CMD_SELECT_A);
    unlock_dev(d);
    d->last_status = st;
    return st;
}

HLW8110_Status_t HLW8110_SetUpdateRate(HLW8110_Handle_t *d, uint8_t dupsel)
{
    uint32_t v = 0U;
    HLW8110_Status_t st;
    if (!d || !d->initialized || dupsel > 3U) return HLW8110_ERR_ARG;
    st = lock_cfg(d);
    if (st != HLW8110_OK) return st;

    st = HLW8110_ReadReg(d, HLW8110_REG_EMUCON2, &v, 2U);
    if (st == HLW8110_OK) {
        v = (v & ~(uint32_t)HLW8110_EMUCON2_DUPSEL_MASK) | ((uint32_t)dupsel << 8);
        st = HLW8110_WriteReg(d, HLW8110_REG_EMUCON2, v, 2U);
    }
    if (st == HLW8110_OK) {
        /* Keep driver state consistent so recovery does not undo this change. */
        d->cfg.dupsel = dupsel;
        d->config_checksum_valid = false;
        st = check_calibration_retry(d, 5U, 2U);
        if (st == HLW8110_OK) {
            d->expected_config_checksum = d->config_checksum;
            d->config_checksum_valid = true;
        }
    }
    unlock_cfg(d);
    return st;
}

/* ------------------------------------------------------------------------- */
/* Measurements                                                               */
/* ------------------------------------------------------------------------- */

HLW8110_Status_t HLW8110_ReadMeasurements(HLW8110_Handle_t *d, HLW8110_Measurements_t *m)
{
    uint32_t v = 0U;
    HLW8110_Status_t st;
    const bool b = (d != NULL) && (d->cfg.variant == HLW8110_VARIANT_HLW8112);

    if (!d || !m || !d->initialized) return HLW8110_ERR_ARG;
    if (!d->coefficients_valid) return HLW8110_ERR_UNCALIBRATED;
    memset(m, 0, sizeof(*m));

#define RD(REG, LEN) do { st = HLW8110_ReadReg(d, (REG), &v, (LEN)); \
                          if (st != HLW8110_OK) return st; } while (0)
    RD(HLW8110_REG_RMSIA, 3U);  m->raw_rms_ia = v;
    if (b) { RD(HLW8110_REG_RMSIB, 3U); m->raw_rms_ib = v; }
    RD(HLW8110_REG_RMSU, 3U);   m->raw_rms_u = v;
    RD(HLW8110_REG_POWER_PA, 4U); m->raw_power_pa = (int32_t)v;
    if (b) { RD(HLW8110_REG_POWER_PB, 4U); m->raw_power_pb = (int32_t)v; }
    RD(HLW8110_REG_POWER_S, 4U); m->raw_power_s = (int32_t)v;
    RD(HLW8110_REG_PF, 3U);     m->raw_pf = v;
    RD(HLW8110_REG_UFREQ, 2U);  m->raw_freq = (uint16_t)v;
    RD(HLW8110_REG_ANGLE, 2U);  m->raw_angle = (uint16_t)v;
#undef RD

    /* RMS registers: MSB=1 means invalid -> treat as zero (datasheet 9.2.4). */
    uint32_t rms_ia = (m->raw_rms_ia & 0x800000U) ? 0U : m->raw_rms_ia;
    uint32_t rms_ib = (m->raw_rms_ib & 0x800000U) ? 0U : m->raw_rms_ib;
    uint32_t rms_u  = (m->raw_rms_u  & 0x800000U) ? 0U : m->raw_rms_u;
    const float kp = d->cfg.k1_current * d->cfg.k2_voltage * 2147483648.0f;

    m->current_a = (float)rms_ia * d->coeff_rms_ia / (d->cfg.k1_current * 8388608.0f) / 1000.0f;
    if (b) m->current_b = (float)rms_ib * d->coeff_rms_ib / (d->cfg.k1_current * 8388608.0f) / 1000.0f;
    m->voltage_v = (float)rms_u * d->coeff_rms_u / (d->cfg.k2_voltage * 4194304.0f) / 100.0f;
    m->active_power_w = (float)m->raw_power_pa * d->coeff_power_pa / kp;
    if (b) m->active_power_b_w = (float)m->raw_power_pb * d->coeff_power_pb / kp;
    m->apparent_power_va = (float)m->raw_power_s * d->coeff_power_s / kp;
    m->power_factor = HLW8110_PowerFactorFromRaw(m->raw_pf);
    if (m->raw_freq != 0U) m->frequency_hz = d->cfg.clki_hz / (8.0f * (float)m->raw_freq);
    /* Datasheet: 0.0805 deg/LSB @50 Hz, 0.0965 deg/LSB @60 Hz, i.e.
     * angle = ANGLE * 360 * f * 16 / clki = ANGLE * 720 / UFREQ.
     * Expressed with UFREQ it is independent of the (inaccurate) oscillator. */
    if (m->raw_freq != 0U) m->phase_deg = (float)m->raw_angle * 720.0f / (float)m->raw_freq;
    m->timestamp = xTaskGetTickCount();
    m->valid = true;
    return HLW8110_OK;
}

HLW8110_Status_t HLW8110_GetMeasurements(HLW8110_Handle_t *d, HLW8110_Measurements_t *out)
{
    if (!d || !out || !d->initialized) return HLW8110_ERR_ARG;
    taskENTER_CRITICAL();
    *out = d->measurement;
    taskEXIT_CRITICAL();
    return out->valid ? HLW8110_OK : HLW8110_ERR_NOT_READY;
}

/* ------------------------------------------------------------------------- */
/* Task                                                                       */
/* ------------------------------------------------------------------------- */

HLW8110_Status_t HLW8110_StartTask(HLW8110_Handle_t *d, const char *name,
                                    uint16_t stack_words, UBaseType_t priority,
                                    TickType_t period_ticks)
{
    if (!d || !d->initialized || d->task || period_ticks == 0U) return HLW8110_ERR_ARG;
    d->cfg.measurement_period_ticks = period_ticks;
    d->stop_request = false;
    d->task_running = true; /* set before creation so StopTask cannot race */
    if (xTaskCreate(HLW8110_Task, name ? name : "HLW8110", stack_words, d, priority, &d->task) != pdPASS) {
        d->task = NULL;
        d->task_running = false;
        return HLW8110_ERR_NOMEM;
    }
    LOGI(d, "measurement task started, period=%lu ticks", (unsigned long)period_ticks);
    return HLW8110_OK;
}

HLW8110_Status_t HLW8110_StopTask(HLW8110_Handle_t *d, TickType_t timeout_ticks)
{
    if (!d || !d->initialized || !d->task) return HLW8110_ERR_ARG;
    if (xTaskGetCurrentTaskHandle() == d->task) return HLW8110_ERR_ARG; /* cannot wait for itself */

    d->stop_request = true;
    TickType_t start = xTaskGetTickCount();
    while (d->task_running) {
        if ((TickType_t)(xTaskGetTickCount() - start) > timeout_ticks) return HLW8110_ERR_TIMEOUT;
        vTaskDelay(1U);
    }
    return HLW8110_OK;
}

/** Periodic health check: detects chip reset or corrupted config registers. */
static void periodic_config_check(HLW8110_Handle_t *d)
{
    bool reset_seen = false;
    HLW8110_Status_t st = check_reset_flag(d, &reset_seen);
    if (st != HLW8110_OK) return; /* communication problem: handled by the measurement path */
    if (lock_cfg(d) != HLW8110_OK) return;

    if (reset_seen) {
        LOGW(d, "RST flag set: chip was reset, reconfiguring");
        (void)reconfigure_locked(d, true);
    } else {
        st = check_calibration_retry(d, 5U, 2U);
        if (st == HLW8110_ERR_CONFIG_MISMATCH) {
            LOGW(d, "config checksum mismatch (now 0x%04X, expected 0x%04X): reconfiguring",
                 (unsigned)d->config_checksum, (unsigned)d->expected_config_checksum);
            (void)reconfigure_locked(d, false);
        } else if (st != HLW8110_OK) {
            LOGW(d, "config check: %s", HLW8110_StatusToString(st));
        } else {
            LOGD(d, "config check OK (0x%04X)", (unsigned)d->config_checksum);
        }
    }
    unlock_cfg(d);
}

/** Full recovery after repeated communication errors. */
static void recover_chip(HLW8110_Handle_t *d)
{
    if (lock_cfg(d) != HLW8110_OK) return;
    LOGE(d, "too many consecutive errors: resetting and reconfiguring chip");
    if (HLW8110_Reset(d) == HLW8110_OK) (void)reconfigure_locked(d, true);
    unlock_cfg(d);
}

void HLW8110_Task(void *argument)
{
    HLW8110_Handle_t *d = (HLW8110_Handle_t *)argument;
    TickType_t last = xTaskGetTickCount();
    d->last_config_check_tick = last;

    for (;;) {
        if (d->stop_request) break;

        HLW8110_Measurements_t m;
        HLW8110_Status_t st = HLW8110_ReadMeasurements(d, &m);
        d->last_status = st;

        if (st == HLW8110_OK) {
            taskENTER_CRITICAL();
            m.sequence = d->measurement.sequence + 1U;
            d->measurement = m;
            taskEXIT_CRITICAL();
            d->consecutive_errors = 0U;
            if (d->cfg.log_measure_every != 0U && (m.sequence % d->cfg.log_measure_every) == 0U)
                HLW8110_LogMeasurements(d, &m);
        } else if (st == HLW8110_ERR_UNCALIBRATED) {
            /* Not a communication error: do not reset the chip. */
            invalidate_measurement(d);
            d->consecutive_errors = 0U;
        } else {
            d->consecutive_errors++;
            d->total_errors++;
            LOGW(d, "measurement read failed: %s (consecutive=%lu total=%lu)", HLW8110_StatusToString(st),
                 (unsigned long)d->consecutive_errors, (unsigned long)d->total_errors);
            if (d->consecutive_errors >= d->cfg.max_consecutive_errors) {
                invalidate_measurement(d); /* never serve stale data after a persistent fault */
                recover_chip(d);
                d->consecutive_errors = 0U;
            }
        }

        TickType_t now = xTaskGetTickCount();
        if ((TickType_t)(now - d->last_config_check_tick) >= d->cfg.config_check_period_ticks) {
            periodic_config_check(d);
            d->last_config_check_tick = now;
        }

        vTaskDelayUntil(&last, d->cfg.measurement_period_ticks);
    }

    d->task = NULL;
    d->stop_request = false;
    d->task_running = false;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------------- */
/* HAL callbacks (forward from HAL_UART_RxCpltCallback / ErrorCallback)       */
/* ------------------------------------------------------------------------- */

void HLW8110_UART_RxCpltCallback(HLW8110_Handle_t *d, UART_HandleTypeDef *huart)
{
    if (!d || !huart || huart != d->cfg.huart || !d->rx_active) return;
    BaseType_t hpw = pdFALSE;
    (void)xSemaphoreGiveFromISR(d->rx_done, &hpw);
    portYIELD_FROM_ISR(hpw);
}

void HLW8110_UART_ErrorCallback(HLW8110_Handle_t *d, UART_HandleTypeDef *huart)
{
    if (!d || !huart || huart != d->cfg.huart || !d->rx_active) return;
    d->rx_error = true;
    BaseType_t hpw = pdFALSE;
    (void)xSemaphoreGiveFromISR(d->rx_done, &hpw);
    portYIELD_FROM_ISR(hpw);
}
