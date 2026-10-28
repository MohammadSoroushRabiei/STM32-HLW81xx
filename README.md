# HLW8110 / HLW8112 Driver for STM32 (HAL + FreeRTOS)

[![CI](https://github.com/MohammadSoroushRabiei/STM32-HLW81xx/actions/workflows/ci.yml/badge.svg)](https://github.com/MohammadSoroushRabiei/STM32-HLW81xx/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

A small, thread-safe C driver for the **HLW8110** (SOP8, channel A) and **HLW8112** (SSOP16, channels A + B)
single-phase power-metering ICs, talking **UART** to an STM32 running **FreeRTOS**.

It gives you **voltage, current, active power, apparent power, power factor, line frequency and phase angle**
as floating-point values, refreshed in the background, with automatic error recovery and an optional text log
on a second UART.

```
[1520][I][PDU1] #5  U=229.87V I=0.412A P=94.12W S=94.80VA PF=0.993 f=50.01Hz ang=12.3
```

More detail: [Hardware and calibration guide](docs/hardware_and_calibration.md) ·
[Troubleshooting](docs/troubleshooting.md) · [Changelog](CHANGELOG.md)

## Features

- Interrupt-driven UART (no DMA, therefore no D-Cache problems on Cortex-M7); RX is armed *before* TX so the first answer byte is never lost.
- Background FreeRTOS task publishes a **snapshot** (`HLW8110_GetMeasurements`) that any task can read safely and quickly.
- Uses the **factory coefficients** stored in the chip (datasheet chapter 10, "calibration-free" method).
- Self-healing: detects chip reset (RST flag), configuration corruption (EMUStatus checksum) and repeated communication errors; reconfigures the chip automatically.
- Stale data is never served after a persistent fault (`valid = false`, `timestamp` for age checks).
- Optional **log** on a second UART: levels, run-time on/off, compile-time removal, no `%f` needed.
- Helpers to compute and correct the scale factors (`HLW8110_CalcK2`, `HLW8110_CorrectK`).
- Full Doxygen documentation, host-side tests with a simulated chip, MIT license.

## Status – what is verified

| Area | State |
|---|---|
| Host tests (simulated chip: protocol, checksum, write protection, conversions, errors, log) | ✅ 70 checks, run in CI |
| Hardware: NUCLEO-F767ZI + HLW8110, init, coefficients, configuration read-back | ✅ |
| Hardware: voltage and line frequency compared with a multimeter | ✅ within meter accuracy (~±1 %) |
| Hardware: current / power / power factor **under load** | ⚠️ not yet verified – calibrate K1 with a known load (see [guide](docs/hardware_and_calibration.md)) |
| HLW8112 / channel B code paths | ⚠️ written from the datasheet, **untested on hardware** |
| Other STM32 families (F4, H7, L4, G4 …) | ⚠️ expected to work (see "Porting to another STM32" below), untested |

## Hardware

| Item | Value |
|---|---|
| MCU TX → chip **RX** (HLW8110 pin 7) | UART |
| MCU RX ← chip **TX** (HLW8110 pin 6) | UART |
| Baud | 9600 (fixed on HLW8110) |
| Frame | 1 start + 8 data + **even parity** + 1 stop → HAL: `UART_WORDLENGTH_9B` + `UART_PARITY_EVEN` |
| Supply | 3.3 V or 5 V (HLW8110 pin 8), 0.1 µF decoupling |
| Internal oscillator | 3.507 … 3.65 MHz (±2 %) |

HLW8112: set `SPIEN = 0` for UART; `SCLK`/`SCSN` select the baud rate (`SCLK=0,SCSN=1` → 19200, `SCLK=1,SCSN=0` → **9600**, `SCLK=1,SCSN=1` → 38400).

The analog front end follows the datasheet reference circuit (Figure 3): a **shunt** on IAP/IAN (with 100 Ω + 330 nF filters) and a
**resistor divider** on VP. Their values define the two scale factors K1 and K2 you must give to the driver.

> ⚠️ **Mains safety.** The chip's ground is normally connected to the mains neutral. Never connect such a board
> to a PC (USB, ST-Link) without an **isolated** USB link or an isolation transformer.

## Quick start

**1. Add the library** – copy `include/` and `src/hlw8110.c` into your project (or add the folders as a submodule) and add `include/` to the include path.

**2. CubeMX**

| UART | Purpose | Settings |
|---|---|---|
| USART2 | chip | 9600, **9-bit word (includes parity)**, **Even**, 1 stop, **global interrupt ON**, NVIC priority number ≥ `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` |
| USART3 | log (optional) | 115200 8N1, no interrupt/DMA (on NUCLEO-144 it is the ST-Link virtual COM port) |

FreeRTOS: `configUSE_MUTEXES`, `INCLUDE_vTaskDelete`, `INCLUDE_vTaskDelayUntil`, `INCLUDE_xTaskGetSchedulerState`, `INCLUDE_xTaskGetCurrentTaskHandle`.

**3. Code** (complete example: [`examples/nucleo_f767zi/main_example.c`](examples/nucleo_f767zi/main_example.c))

```c
#include "hlw8110.h"

static HLW8110_Handle_t hlw;                       /* MUST be static */

void app_task(void *arg)                           /* runs after the scheduler started */
{
    HLW8110_Config_t cfg;
    HLW8110_ConfigDefault(&cfg, &huart2);          /* chip UART */
    cfg.k1_current = 1.0f;                                    /* shunt R001 = 1 mOhm            */
    cfg.k2_voltage = HLW8110_CalcK2(1000000.0f, 1650.0f);     /* 1 MOhm top, 1.65 kOhm bottom   */

    cfg.log_huart = &huart3;  cfg.log_enabled = true;         /* optional log */
    cfg.log_level = HLW8110_LOG_INFO;  cfg.log_measure_every = 5;

    HLW8110_Init(&hlw, &cfg);
    while (HLW8110_AppInit(&hlw) != HLW8110_OK)    /* chip may power up later than the MCU */
        vTaskDelay(pdMS_TO_TICKS(1000));
    HLW8110_StartTask(&hlw, "HLW8110", 1024, tskIDLE_PRIORITY + 2, cfg.measurement_period_ticks);

    for (;;) {
        HLW8110_Measurements_t m;
        if (HLW8110_GetMeasurements(&hlw, &m) == HLW8110_OK &&
            (xTaskGetTickCount() - m.timestamp) < pdMS_TO_TICKS(1000)) {
            /* m.voltage_v, m.current_a, m.active_power_w, m.power_factor, m.frequency_hz ... */
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* Forward the HAL callbacks (the driver ignores UARTs that are not its own). */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *h) { HLW8110_UART_RxCpltCallback(&hlw, h); }
void HAL_UART_ErrorCallback (UART_HandleTypeDef *h) { HLW8110_UART_ErrorCallback (&hlw, h); }
```

**4. Check the start-up log** (open the ST-Link virtual COM port at 115200). A healthy start looks like:

```
[102][I] HLW8110 driver init: variant=HLW8110 uart_timeout=20ms dupsel=1
[275][I] chip reset command sent
[290][I] coeff: RMS_IA=0x1B2C RMS_U=0x8F10 P_A=0x9A3E P_S=0x9A3E chk=0xC4D1
[305][I] configured: EMUCON2=0x0165 (read-back OK)
[312][I] configured: SYSCON=0x0A04 (read-back OK)
[320][I] config checksum baseline = 0x5B8D
[321][I] AppInit OK
```

(Coefficients and checksum differ from chip to chip – the values above are examples.) `SYSCON=0x0A04` and `EMUCON2=0x0165` are the expected values for `dupsel = 1`.

## Configuration reference

Start with `HLW8110_ConfigDefault()`; change only what you need.

| Field | Default | Meaning |
|---|---|---|
| `huart` | – | UART connected to the chip |
| `uart_timeout_ms` | 20 | Timeout of one TX and of one RX answer |
| **`k1_current`** | 1.0 | **Shunt in milliohm** (R001 → 1.0). Must match your board |
| **`k2_voltage`** | 1.0 | `1000·Rsense/(Rtop+Rsense)`, see `HLW8110_CalcK2()`. Must match your board |
| `clki_hz` | 3579545 | Real oscillator frequency; only changes the frequency reading |
| `variant` | HLW8110 | `HLW8110_VARIANT_HLW8110` or `HLW8110_VARIANT_HLW8112` |
| `use_rx_it` | true | RX by interrupt (recommended) |
| `configure_emucon2` | true | Driver writes SYSCON/EMUCON2 (PGA, ADCs, update rate, PF, zero-cross) |
| `enable_channel_b` | false | HLW8112 only |
| `dupsel` | 1 | Chip update rate: 0=3.4, **1=6.8**, 2=13.65, 3=27.3 Hz |
| `measurement_period_ticks` | 150 ms | Task period (keep ≥ the chip update period) |
| `max_consecutive_errors` | 5 | Errors in a row before reset + reconfigure |
| `config_check_period_ticks` | 5 s | RST-flag / checksum health check |
| `log_huart` / `log_enabled` / `log_level` / `log_measure_every` | NULL / off / INFO / 0 | See the "Logging" chapter |
| `log_name` | NULL | Tag in every log line (max 7 chars), e.g. `"PDU1"`; essential with several chips |

## Measurement values

`HLW8110_Measurements_t`:

| Field | Unit | Notes |
|---|---|---|
| `voltage_v` | V | RMS |
| `current_a` | A | RMS, channel A |
| `active_power_w` | W | negative = reverse flow |
| `apparent_power_va` | VA | |
| `power_factor` | −1 … 1 | **Locked to 1.0 by the chip below its start power** (no load) |
| `frequency_hz` | Hz | Accuracy follows the internal oscillator (±2 %) unless `clki_hz` is corrected |
| `phase_deg` | ° | Meaningless without load (noise) |
| `current_b`, `active_power_b_w` | A, W | HLW8112 only (0 on HLW8110) |
| `raw_*` | – | Raw register values for debugging |
| `sequence`, `timestamp`, `valid` | – | Counter, `xTaskGetTickCount()` at capture, validity. **Check the age of `timestamp`.** |

Very small signals (a few mA, a few tenths of a watt) are dominated by offset/noise of the shunt input. This is a property of the chip; see the [guide](docs/hardware_and_calibration.md) for offset registers.

## K1 / K2 scale factors

The chip returns raw numbers; the driver converts them with the factory coefficients **and two factors describing your board**:

| Factor | Meaning | Formula |
|---|---|---|
| **K1** | shunt resistance in milliohm | `R001` → `1.0`, `R002` → `2.0`, `0.5 mΩ` → `0.5` |
| **K2** | voltage divider | `1000 · Rsense / (Rtop + Rsense)` → `HLW8110_CalcK2(rtop, rsense)` |

Readings are **inversely proportional** to K. Correct a factor with a reference instrument:

```c
cfg.k2_voltage = HLW8110_CorrectK(cfg.k2_voltage, /*displayed*/ 190.4f, /*actual*/ 230.0f);
cfg.k1_current = HLW8110_CorrectK(cfg.k1_current, /*displayed*/ 0.82f,  /*actual*/ 0.80f);
```

Full procedure with SMD marking codes (`R001`, `2003`, `1651`) and examples: [docs/hardware_and_calibration.md](docs/hardware_and_calibration.md).

## Logging

Configure in `HLW8110_Config_t`, change at run time, or remove at compile time:

```c
cfg.log_huart = &huart3;            /* NULL = no log */
cfg.log_enabled = true;
cfg.log_level = HLW8110_LOG_DEBUG;  /* ERROR < WARN < INFO < DEBUG */
cfg.log_measure_every = 5;          /* one measurement line per 5 good reads, 0 = never */
cfg.log_name = "PDU1";              /* optional tag, see Multiple chips */

HLW8110_LogEnable(&hlw, false);                  /* run time: off / on */
HLW8110_LogSetLevel(&hlw, HLW8110_LOG_WARN);
HLW8110_LogSetUart(&hlw, &huart1);
/* compile time: -DHLW8110_LOG_ENABLE=0 removes every log call */
```

| Level | You see |
|---|---|
| ERROR | chip reset/recovery, init failure, read-back mismatch, uncalibrated chip |
| WARN | failed reads with the error name, config checksum mismatch, RST flag |
| INFO | start-up steps, coefficients, measurement lines |
| DEBUG | every register read/write: `RD reg=0x26 len=3 val=0x2F0A11` |

Log calls of **all** driver instances are serialised by one shared mutex, so several chips may use the same log UART. Log calls block while the UART transmits (≈1 ms per 11 characters at 115200) and must not be used from an interrupt. Use a stack of ≥ 1024 words for the driver task when logging is on.

## Multiple chips

Each chip needs its **own handle and its own UART**; nothing in the driver is global except the shared log mutex.
Complete example: [`examples/two_chips_one_log/main_two_chips.c`](examples/two_chips_one_log/main_two_chips.c).

```c
static HLW8110_Handle_t hlw1, hlw2;               /* both static */

HLW8110_Config_t c1, c2;
HLW8110_ConfigDefault(&c1, &huart2);   c1.log_name = "PDU1";
HLW8110_ConfigDefault(&c2, &huart4);   c2.log_name = "PDU2";
c1.log_huart = c2.log_huart = &huart3; /* ONE log UART for both */
c1.log_enabled = c2.log_enabled = true;
/* K1, K2 and clki_hz are per chip: set them from each board's own parts / calibration */

/* Forward the HAL callbacks to every instance; each ignores UARTs that are not its own. */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *h)
{ HLW8110_UART_RxCpltCallback(&hlw1, h); HLW8110_UART_RxCpltCallback(&hlw2, h); }
void HAL_UART_ErrorCallback(UART_HandleTypeDef *h)
{ HLW8110_UART_ErrorCallback(&hlw1, h); HLW8110_UART_ErrorCallback(&hlw2, h); }
```

- Use different task names (`"HLW8110-1"`, `"HLW8110-2"`) and ~1024 words of stack for each driver task.
- Both chip UARTs need the same settings (9600, 9 bit, even parity) and their global interrupt enabled with a FreeRTOS-safe NVIC priority.
- The two chips are read in parallel (one task and one UART each). Keep `log_measure_every` high enough that logging does not dominate the log UART bandwidth (each measurement line is ≈ 90 characters ≈ 8 ms at 115200).
- Log lines read `[1520][I][PDU1] #5 U=...` and `[1522][I][PDU2] #5 U=...`.
- The mutex that protects the log UART is created by the first `HLW8110_Init()` and deleted by the last `HLW8110_Deinit()`.

## How it works

```
 your tasks ──GetMeasurements()──► snapshot (critical section copy)
                                        ▲
                         HLW8110_Task (every measurement_period)
                          ├─ ReadMeasurements: RMSIA, RMSU, POWER_PA, POWER_S, PF, UFREQ, ANGLE
                          ├─ every config_check_period: RST flag + EMUStatus checksum
                          └─ N errors in a row → invalidate snapshot, reset chip, reconfigure
                                        │
                  transact_it():  arm RX-IT ─► TX request ─► wait semaphore ◄─ HAL RX-complete ISR
                                        │
                                  USART2 ◄──► HLW8110
```

- **Locks:** per instance, one mutex serialises the chip UART and another serialises configuration transactions; one mutex shared by all instances protects the log UART.
- **AppInit:** wait for the oscillator → reset command → read 8 factory coefficients → write/verify SYSCON (PGA A = 16, PGA U = 1, ADCs) and EMUCON2 (update rate, power factor, waveform, zero-crossing) → store the checksum baseline.
- **Uncalibrated chip** (all coefficients `0xFFFF`): `HLW8110_ERR_UNCALIBRATED`, permanent – the driver does not reset-loop.
- The "calibration-free" formulas assume PGA(A) = 16 and PGA(U) = 1, which the driver writes to SYSCON.

## Error codes

| Code | Meaning | Typical cause |
|---|---|---|
| `HLW8110_OK` | success | |
| `HLW8110_ERR_ARG` | invalid argument / config | NULL pointer, `k1/k2 ≤ 0`, bad length |
| `HLW8110_ERR_HAL` | HAL/UART error or read-back mismatch | framing/parity/overrun error |
| `HLW8110_ERR_TIMEOUT` | no answer in time | wiring, power, baud, chip busy |
| `HLW8110_ERR_CHECKSUM` | answer frame corrupted | wrong parity/word length, noise |
| `HLW8110_ERR_MUTEX` | driver mutex timeout | long blocking log / another task holds the bus |
| `HLW8110_ERR_NOT_READY` | scheduler not running / no valid snapshot | call `AppInit` after the kernel starts |
| `HLW8110_ERR_NOMEM` | FreeRTOS object/task creation failed | heap too small |
| `HLW8110_ERR_UNCALIBRATED` | chip has no factory coefficients | permanent; use own calibration |
| `HLW8110_ERR_UNSUPPORTED` | feature not on this variant | channel B on HLW8110 |
| `HLW8110_ERR_BUSY` | already initialised / checksum busy | |
| `HLW8110_ERR_CONFIG_MISMATCH` | config registers changed | chip glitch; driver reconfigures |

Use `HLW8110_StatusToString()` to print them. More: [docs/troubleshooting.md](docs/troubleshooting.md).

## Porting to another STM32

The driver uses only `HAL_UART_Transmit`, `HAL_UART_Receive[_IT]`, `HAL_UART_AbortReceive`, `HAL_Delay` and the `__HAL_UART_CLEAR_*FLAG` macros, plus standard FreeRTOS APIs.

- By default `hlw8110.h` includes `"main.h"` (CubeMX). Override: `-DHLW8110_HAL_HEADER=\"stm32h7xx_hal.h\"`.
- The chip frame is 8 data bits + even parity: with HAL set `WordLength = 9B` and `Parity = EVEN`.
- Other RTOS or LL drivers need changes in the transport functions (`tx_blocking`, `transact_it`, `rx_blocking`) and the RTOS primitives.

## Tests and documentation

```bash
make -C tests test          # builds the driver with gcc against a simulated HLW8110 and runs 70 checks
doxygen Doxyfile            # HTML API documentation -> docs/doxygen/html/index.html
```

The GitHub Actions workflow (`.github/workflows/ci.yml`) runs the tests and publishes the HTML documentation to GitHub Pages
(enable *Settings → Pages → Source: GitHub Actions*).

## Limitations and roadmap

- No energy (kWh) accumulation, overcurrent/overvoltage/sag interrupts, peak or waveform readout yet (registers are defined in `hlw8110_regs.h`).
- No built-in calibration of gain/phase/offset registers (PAGain, PhaseA, RMSIAOS …); only K1/K2 scaling.
- HLW8112 channel B and the comparator are untested on hardware.
- UART only (SPI mode of the HLW8112 is not supported).

## References

- HLW8110/HLW8112 datasheet REV 1.01 (Hiliwi) – obtain it from the manufacturer; it is not redistributed here.
- License: [MIT](LICENSE).
