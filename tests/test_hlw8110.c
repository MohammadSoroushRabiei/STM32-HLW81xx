/*
 * Host-side test of the HLW8110 driver against a simulated chip.
 * Build & run:  make -C tests test
 *
 * The simulated chip implements the UART protocol of datasheet 12.2
 * (read / write / special commands, checksum, write protection) on a register
 * file, so the whole driver stack is exercised without hardware.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "hlw8110.h"

/* ---------------- FreeRTOS / HAL stubs ---------------- */
static int live_sems_ext;
SemaphoreHandle_t xSemaphoreCreateMutex(void)  { StubSem *s = calloc(1, sizeof *s); s->count = 1; s->is_mutex = 1; live_sems_ext++; return s; }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { live_sems_ext++; return calloc(1, sizeof(StubSem)); }
void vSemaphoreDelete(SemaphoreHandle_t s) { live_sems_ext--; free(s); }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) { (void)t; if (s->count > 0) { s->count--; return pdTRUE; } return pdFALSE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { s->count = 1; return pdTRUE; }
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s, BaseType_t *w) { (void)w; s->count = 1; return pdTRUE; }
static TickType_t g_tick;
BaseType_t xTaskGetSchedulerState(void) { return taskSCHEDULER_RUNNING; }
TickType_t xTaskGetTickCount(void) { return g_tick++; }
void vTaskDelay(TickType_t t) { (void)t; }
void vTaskDelayUntil(TickType_t *l, TickType_t i) { (void)l; (void)i; }
void vTaskDelete(TaskHandle_t t) { (void)t; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return NULL; }
BaseType_t xTaskCreate(TaskFunction_t f, const char *n, uint16_t s, void *a, UBaseType_t p, TaskHandle_t *h)
{ (void)f; (void)n; (void)s; (void)a; (void)p; if (h) *h = (void *)1; return pdPASS; }
void HAL_Delay(uint32_t ms) { (void)ms; }

/* ---------------- simulated chips (two, one per UART) ---------------- */
typedef struct {
    uint32_t reg[0x80];
    bool wren;
    UART_HandleTypeDef *uart;
    uint8_t *rxb; uint16_t rxl;
} Chip;

static USART_TypeDef inst;
static UART_HandleTypeDef huart = { &inst }, huartb = { &inst }, hlog = { &inst };
static HLW8110_Handle_t hlw, hlw2;
static Chip chips[2] = { { .uart = &huart }, { .uart = &huartb } };
#define regs chips[0].reg            /* single-chip tests use chip 0 */
static bool inject_bad_checksum;
static bool chip_silent;
static char logbuf[16384]; static size_t loglen;
#define live_sems live_sems_ext      /* leak check for semaphores */

static uint8_t cks(const uint8_t *b, size_t n) { uint8_t s = 0; for (size_t i = 0; i < n; i++) s = (uint8_t)(s + b[i]); return (uint8_t)~s; }

static void chip_reset(Chip *c)
{
    /* factory coefficients (0x6F..0x77) live in OTP and survive a reset */
    memset(c->reg, 0, 0x6F * sizeof c->reg[0]);
    c->reg[HLW8110_REG_SYSCON] = 0x0A04; c->reg[HLW8110_REG_EMUCON2] = 0x0001;
    c->reg[HLW8110_REG_SYSSTATUS] = 0x01; /* RST */
    c->wren = false;
}

HAL_StatusTypeDef HAL_UART_Receive_IT(UART_HandleTypeDef *h, uint8_t *p, uint16_t n)
{ for (int i = 0; i < 2; i++) if (chips[i].uart == h) { chips[i].rxb = p; chips[i].rxl = n; } return HAL_OK; }
HAL_StatusTypeDef HAL_UART_AbortReceive(UART_HandleTypeDef *h)
{ for (int i = 0; i < 2; i++) if (chips[i].uart == h) chips[i].rxb = NULL; return HAL_OK; }
HAL_StatusTypeDef HAL_UART_Receive(UART_HandleTypeDef *h, uint8_t *p, uint16_t n, uint32_t t) { (void)h; (void)p; (void)n; (void)t; return HAL_TIMEOUT; }

HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *h, uint8_t *p, uint16_t n, uint32_t t)
{
    (void)t;
    if (h == &hlog) {                                            /* log UART */
        if (loglen + n < sizeof logbuf) { memcpy(logbuf + loglen, p, n); loglen += n; logbuf[loglen] = 0; }
        return HAL_OK;
    }
    Chip *c = (h == &huart) ? &chips[0] : (h == &huartb) ? &chips[1] : NULL;
    if (!c || n < 2 || p[0] != HLW8110_UART_HEADER) return HAL_OK;
    uint8_t cmd = p[1];
    if (cmd == HLW8110_CMD_SPECIAL) {                            /* special command frame */
        if (cks(p, 3) != p[3]) return HAL_OK;
        if (p[2] == HLW8110_CMD_WRITE_ENABLE) c->wren = true;
        else if (p[2] == HLW8110_CMD_WRITE_DISABLE) c->wren = false;
        else if (p[2] == HLW8110_CMD_RESET) chip_reset(c);
    } else if (cmd & 0x80U) {                                    /* write */
        uint8_t reg = cmd & 0x7FU; uint8_t len = (uint8_t)(n - 3);
        if (cks(p, (size_t)n - 1) != p[n - 1]) return HAL_OK;
        bool prot = (reg <= 0x1F) || reg == HLW8110_REG_IE;
        if (!prot || c->wren) { uint32_t v = 0; for (uint8_t i = 0; i < len; i++) v = (v << 8) | p[2 + i]; c->reg[reg] = v; }
    } else if (c->rxb && !chip_silent) {                         /* read request */
        uint8_t reg = cmd; uint8_t len = (uint8_t)(c->rxl - 1); uint8_t chk[8];
        chk[0] = p[0]; chk[1] = p[1];
        uint32_t v = c->reg[reg];
        if (reg == HLW8110_REG_SYSSTATUS) c->reg[reg] &= ~1U;    /* RST clears on read */
        for (uint8_t i = 0; i < len; i++) { c->rxb[i] = (uint8_t)(v >> (8 * (len - 1 - i))); chk[2 + i] = c->rxb[i]; }
        c->rxb[len] = cks(chk, (size_t)len + 2U);
        if (inject_bad_checksum) c->rxb[len] ^= 0x55;
        c->rxb = NULL;
        /* what the application's HAL_UART_RxCpltCallback does: forward to EVERY instance */
        HLW8110_UART_RxCpltCallback(&hlw, h);
        HLW8110_UART_RxCpltCallback(&hlw2, h);
    }
    return HAL_OK;
}

/* ---------------- tiny test framework ---------------- */
static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)
#define NEAR(a, b, tol) CHECK(fabsf((float)(a) - (float)(b)) <= (tol))

static HLW8110_Config_t make_cfg(void)
{
    HLW8110_Config_t c; HLW8110_ConfigDefault(&c, &huart);
    c.k1_current = 1.0f; c.k2_voltage = HLW8110_CalcK2(1000000.0f, 1650.0f);
    return c;
}

static void load_factory_c(Chip *c);
static void load_factory(void) { load_factory_c(&chips[0]); }
static void load_factory_c(Chip *c)
{
    c->reg[HLW8110_REG_RMSIA_C] = 0x1B2C; c->reg[HLW8110_REG_RMSIB_C] = 0x1B2C; c->reg[HLW8110_REG_RMSU_C] = 0x8F10;
    c->reg[HLW8110_REG_POWERPA_C] = 0x9A3E; c->reg[HLW8110_REG_POWERPB_C] = 0x9A3E; c->reg[HLW8110_REG_POWERS_C] = 0x9A3E;
    c->reg[HLW8110_REG_ENERGYA_C] = 0x1111; c->reg[HLW8110_REG_ENERGYB_C] = 0x2222; c->reg[HLW8110_REG_CHECKSUM] = 0xC4D1;
    c->reg[HLW8110_REG_EMUSTATUS] = 0x00B32F;
}

static void test_helpers(void)
{
    puts("helpers");
    NEAR(HLW8110_CalcK2(1000000.0f, 1650.0f), 1.6473f, 0.0005f);
    NEAR(HLW8110_CalcK2(1000000.0f, 1000.0f), 0.999f, 0.001f);
    NEAR(HLW8110_CorrectK(2.0f, 190.4f, 231.0f), 1.6485f, 0.001f);
    NEAR(HLW8110_CorrectK(1.0f, 0.5f, 0.0f), 1.0f, 0.0f);  /* invalid input keeps K */
    CHECK(HLW8110_Signed24(0x800000) == -8388608);
    CHECK(HLW8110_Signed24(0x7FFFFF) == 8388607);
    NEAR(HLW8110_PowerFactorFromRaw(0x7FFFFF), 1.0f, 1e-6f);
    NEAR(HLW8110_PowerFactorFromRaw(0x400000), 0.5f, 1e-3f);
    CHECK(strcmp(HLW8110_StatusToString(HLW8110_ERR_TIMEOUT), "ERR_TIMEOUT") == 0);
    HLW8110_Config_t c; CHECK(HLW8110_ConfigDefault(NULL, &huart) == HLW8110_ERR_ARG);
    CHECK(HLW8110_ConfigDefault(&c, &huart) == HLW8110_OK && c.dupsel == 1 && c.log_huart == NULL);
}

static void test_init_and_config(void)
{
    puts("init / AppInit / configuration");
    memset(&hlw, 0, sizeof hlw); chip_reset(&chips[0]); load_factory();
    HLW8110_Config_t c = make_cfg();
    c.log_huart = &hlog; c.log_enabled = true; c.log_level = HLW8110_LOG_DEBUG; c.log_measure_every = 1;
    CHECK(HLW8110_Init(&hlw, &c) == HLW8110_OK);
    CHECK(HLW8110_Init(&hlw, &c) == HLW8110_ERR_BUSY);
    CHECK(HLW8110_AppInit(&hlw) == HLW8110_OK);
    CHECK(regs[HLW8110_REG_SYSCON] == 0x0A04);       /* README bench values */
    CHECK(regs[HLW8110_REG_EMUCON2] == 0x0165);
    CHECK(hlw.coefficients_valid && hlw.config_checksum_valid);
}

static void set_raw(uint32_t ia, uint32_t u, int32_t pa, int32_t s, uint32_t pf, uint16_t freq, uint16_t ang)
{
    regs[HLW8110_REG_RMSIA] = ia; regs[HLW8110_REG_RMSU] = u;
    regs[HLW8110_REG_POWER_PA] = (uint32_t)pa; regs[HLW8110_REG_POWER_S] = (uint32_t)s;
    regs[HLW8110_REG_PF] = pf; regs[HLW8110_REG_UFREQ] = freq; regs[HLW8110_REG_ANGLE] = ang;
}

static void test_conversion(void)
{
    puts("measurement conversion (datasheet chapter 10)");
    HLW8110_Measurements_t m;
    /* Choose raw values so the expected physical values are exact by the datasheet formulas:
     *  I = RmsIA*C / (K1*2^23) mA -> raw = I_mA*K1*2^23 / C */
    const float k1 = 1.0f, k2 = HLW8110_CalcK2(1000000.0f, 1650.0f);
    float i_a = 2.5f, u_v = 230.0f, p_w = 575.0f;
    uint32_t ia = (uint32_t)(i_a * 1000.0f * k1 * 8388608.0f / 0x1B2C);
    uint32_t ur = (uint32_t)(u_v * 100.0f * k2 * 4194304.0f / 0x8F10);
    int32_t  pa = (int32_t)(p_w * k1 * k2 * 2147483648.0f / 0x9A3E);
    set_raw(ia, ur, pa, pa, 0x7FFFFF, (uint16_t)(3579545.0f / 8.0f / 50.0f + 0.5f), 0);
    CHECK(HLW8110_ReadMeasurements(&hlw, &m) == HLW8110_OK);
    NEAR(m.current_a, 2.5f, 0.002f); NEAR(m.voltage_v, 230.0f, 0.05f);
    NEAR(m.active_power_w, 575.0f, 0.5f); NEAR(m.apparent_power_va, 575.0f, 0.5f);
    NEAR(m.power_factor, 1.0f, 1e-4f); NEAR(m.frequency_hz, 50.0f, 0.01f);
    CHECK(m.valid && m.raw_rms_ia == ia);

    /* phase angle: 36 deg at 50 Hz -> ANGLE = 36/0.0805 ; result must not depend on clki_hz */
    uint16_t freq = (uint16_t)(3579545.0f / 8.0f / 50.0f + 0.5f);
    set_raw(ia, ur, pa, pa, 0x7FFFFF, freq, (uint16_t)(36.0f / 0.0805f + 0.5f));
    HLW8110_ReadMeasurements(&hlw, &m); NEAR(m.phase_deg, 36.0f, 0.1f);
    hlw.cfg.clki_hz = 3629000.0f; HLW8110_ReadMeasurements(&hlw, &m); NEAR(m.phase_deg, 36.0f, 0.1f);
    NEAR(m.frequency_hz, 50.0f * 3629000.0f / 3579545.0f, 0.02f); /* frequency does follow clki */
    hlw.cfg.clki_hz = 3579545.0f;

    /* invalid RMS (MSB set) is treated as zero, negative power */
    set_raw(0x800123, 0x800123, -1000000, 0, 0x7FFFFF, freq, 0);
    HLW8110_ReadMeasurements(&hlw, &m); NEAR(m.current_a, 0.0f, 1e-6f); NEAR(m.voltage_v, 0.0f, 1e-6f); CHECK(m.active_power_w < 0.0f);
    /* 60 Hz line */
    set_raw(ia, ur, pa, pa, 0x7FFFFF, (uint16_t)(3579545.0f / 8.0f / 60.0f + 0.5f), (uint16_t)(36.0f / 0.0965f + 0.5f));
    HLW8110_ReadMeasurements(&hlw, &m); NEAR(m.frequency_hz, 60.0f, 0.02f); NEAR(m.phase_deg, 36.0f, 0.1f);
}

static void test_errors_and_recovery(void)
{
    puts("error handling / recovery");
    uint32_t v; HLW8110_Measurements_t m;
    inject_bad_checksum = true;
    CHECK(HLW8110_ReadReg(&hlw, HLW8110_REG_RMSU, &v, 3) == HLW8110_ERR_CHECKSUM);
    inject_bad_checksum = false; chip_silent = true;
    CHECK(HLW8110_ReadReg(&hlw, HLW8110_REG_RMSU, &v, 3) == HLW8110_ERR_TIMEOUT);
    chip_silent = false;
    CHECK(HLW8110_ReadReg(&hlw, 0x80, &v, 3) == HLW8110_ERR_ARG);
    CHECK(HLW8110_ReadReg(&hlw, HLW8110_REG_RMSU, &v, 5) == HLW8110_ERR_ARG);
    CHECK(HLW8110_SelectCurrentChannel(&hlw, true) == HLW8110_ERR_UNSUPPORTED);
    CHECK(HLW8110_SetUpdateRate(&hlw, 4) == HLW8110_ERR_ARG);
    CHECK(HLW8110_SetUpdateRate(&hlw, 2) == HLW8110_OK && ((regs[HLW8110_REG_EMUCON2] >> 8) & 3) == 2 && hlw.cfg.dupsel == 2);
    CHECK(HLW8110_GetMeasurements(&hlw, &m) == HLW8110_ERR_NOT_READY); /* no task snapshot yet */

    /* write protection: a protected write without the driver's unlock must be ignored by the chip */
    regs[HLW8110_REG_SYSCON] = 0x0A04; chips[0].wren = false;
    CHECK(HLW8110_WriteReg(&hlw, HLW8110_REG_SYSCON, 0x0A05, 2) == HLW8110_OK && regs[HLW8110_REG_SYSCON] == 0x0A05);
    CHECK(chips[0].wren == false); /* driver locked the registers again */
}

static void test_uncalibrated(void)
{
    puts("uncalibrated chip");
    HLW8110_Deinit(&hlw); memset(&hlw, 0, sizeof hlw); chip_reset(&chips[0]);
    for (int r = 0x6F; r <= 0x77; r++) regs[r] = 0xFFFF;
    HLW8110_Config_t c = make_cfg(); HLW8110_Init(&hlw, &c);
    CHECK(HLW8110_AppInit(&hlw) == HLW8110_ERR_UNCALIBRATED);
    HLW8110_Measurements_t m; CHECK(HLW8110_ReadMeasurements(&hlw, &m) == HLW8110_ERR_UNCALIBRATED);
}

static void test_log_and_deinit(void)
{
    puts("logging / deinit");
    CHECK(strstr(logbuf, "[I] AppInit OK") != NULL);
    CHECK(strstr(logbuf, "[D] RD reg=0x26") != NULL);
    CHECK(strstr(logbuf, "ERR_CHECKSUM") != NULL);
    CHECK(strstr(logbuf, "\r\n") != NULL);
    memset(&hlw, 0, sizeof hlw); chip_reset(&chips[0]); load_factory();
    HLW8110_Config_t c = make_cfg(); c.log_huart = &hlog; c.log_enabled = true; c.log_measure_every = 1;
    HLW8110_Init(&hlw, &c); HLW8110_AppInit(&hlw);
    HLW8110_Measurements_t m; set_raw(1000, 1000, 100, 100, 0x7FFFFF, 8948, 100);
    HLW8110_ReadMeasurements(&hlw, &m); m.sequence = 7;
    loglen = 0; logbuf[0] = 0;
    HLW8110_LogMeasurements(&hlw, &m);
    CHECK(strstr(logbuf, "#7 U=") != NULL && strstr(logbuf, "PF=1.000") != NULL);
    HLW8110_LogEnable(&hlw, false); loglen = 0; HLW8110_Log(&hlw, HLW8110_LOG_ERROR, "x"); CHECK(loglen == 0);
    HLW8110_LogEnable(&hlw, true);  HLW8110_LogSetLevel(&hlw, HLW8110_LOG_ERROR);
    HLW8110_Log(&hlw, HLW8110_LOG_INFO, "hidden"); CHECK(loglen == 0);
    HLW8110_Log(&hlw, HLW8110_LOG_ERROR, "shown"); CHECK(loglen > 0);
    HLW8110_LogSetUart(&hlw, NULL); loglen = 0; HLW8110_Log(&hlw, HLW8110_LOG_ERROR, "x"); CHECK(loglen == 0);
    CHECK(HLW8110_Deinit(&hlw) == HLW8110_OK && hlw.initialized == false);
    CHECK(HLW8110_Deinit(&hlw) == HLW8110_ERR_ARG);
}


static void test_two_chips_one_log(void)
{
    puts("two chips sharing one log UART");
    const int base = live_sems;
    memset(&hlw, 0, sizeof hlw); memset(&hlw2, 0, sizeof hlw2);
    chip_reset(&chips[0]); chip_reset(&chips[1]); load_factory_c(&chips[0]); load_factory_c(&chips[1]);

    HLW8110_Config_t c1 = make_cfg(), c2 = make_cfg();
    c2.huart = &huartb;
    c2.k2_voltage = HLW8110_CalcK2(1000000.0f, 1000.0f);        /* a different board */
    c1.log_huart = c2.log_huart = &hlog; c1.log_enabled = c2.log_enabled = true;
    c1.log_level = c2.log_level = HLW8110_LOG_INFO;
    c1.log_name = "PDU1"; c2.log_name = "PDU2-long-name";        /* truncated to 7 chars */
    loglen = 0; logbuf[0] = 0;

    CHECK(HLW8110_Init(&hlw, &c1) == HLW8110_OK && HLW8110_Init(&hlw2, &c2) == HLW8110_OK);
    CHECK(hlw.log_ref && hlw2.log_ref);
    CHECK(HLW8110_AppInit(&hlw) == HLW8110_OK && HLW8110_AppInit(&hlw2) == HLW8110_OK);
    CHECK(strstr(logbuf, "[I][PDU1] AppInit OK") != NULL);
    CHECK(strstr(logbuf, "[I][PDU2-lo] AppInit OK") != NULL);

    chips[0].reg[HLW8110_REG_RMSU] = 0x100000; chips[0].reg[HLW8110_REG_RMSIA] = 0x200000;
    chips[1].reg[HLW8110_REG_RMSU] = 0x100000; chips[1].reg[HLW8110_REG_RMSIA] = 0x400000;
    chips[0].reg[HLW8110_REG_UFREQ] = chips[1].reg[HLW8110_REG_UFREQ] = 8948;
    HLW8110_Measurements_t m1, m2;
    CHECK(HLW8110_ReadMeasurements(&hlw, &m1) == HLW8110_OK);
    CHECK(HLW8110_ReadMeasurements(&hlw2, &m2) == HLW8110_OK);
    NEAR(m1.voltage_v / m2.voltage_v, c2.k2_voltage / c1.k2_voltage, 0.001f);  /* own K2 per chip */
    NEAR(m2.current_a / m1.current_a, 2.0f, 0.001f);                           /* own registers per chip */

    /* Both instances forward the same HAL callbacks, yet never complete each other's transfers
     * (the reads above would fail otherwise). Log sharing: */
    loglen = 0; logbuf[0] = 0;
    HLW8110_Log(&hlw, HLW8110_LOG_ERROR, "a"); HLW8110_Log(&hlw2, HLW8110_LOG_ERROR, "b");
    CHECK(strstr(logbuf, "[E][PDU1] a\r\n") != NULL && strstr(logbuf, "[E][PDU2-lo] b\r\n") != NULL);

    /* the shared mutex survives until the LAST instance is gone */
    CHECK(HLW8110_Deinit(&hlw) == HLW8110_OK);
    loglen = 0; HLW8110_Log(&hlw2, HLW8110_LOG_ERROR, "still"); CHECK(loglen > 0);
    CHECK(HLW8110_Deinit(&hlw2) == HLW8110_OK);
    CHECK(live_sems == base);                                                   /* no semaphore leaked */
}

int main(void)
{
    test_helpers(); test_init_and_config(); test_conversion(); test_errors_and_recovery();
    test_uncalibrated(); test_log_and_deinit(); test_two_chips_one_log();
    printf("\n%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
