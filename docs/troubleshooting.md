# Troubleshooting

Enable the log first (`log_level = HLW8110_LOG_DEBUG` shows every register access) and compare with the expected start-up sequence in the [README](../README.md#quick-start).

## No communication

| Symptom in the log | Check |
|---|---|
| `AppInit failed: ERR_TIMEOUT` (repeats) | TX/RX crossed? (MCU TX → chip **RX**, MCU RX → chip **TX**). Common GND. Chip supply 3.0–3.6 V (or 4.5–5.5 V) with decoupling. Chip powered **before/with** the MCU or retry loop in place. |
| `ERR_TIMEOUT` and the ISR never fires | **USART global interrupt enabled** in CubeMX? NVIC priority number ≥ `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`? `HAL_UART_RxCpltCallback` forwarded to `HLW8110_UART_RxCpltCallback`? |
| `ERR_CHECKSUM` on every read | Frame must be **8 data + even parity + 1 stop** → `UART_WORDLENGTH_9B` + `UART_PARITY_EVEN`, **9600 baud**. |
| `ERR_HAL` sporadically | Framing/parity/overrun – noise on the lines, wrong baud, long wires, other code using the same UART. |
| HLW8112: no answer | `SPIEN` must be low; `SCLK=1, SCSN=0` selects 9600 baud. |

## Wrong values

| Symptom | Cause / fix |
|---|---|
| Voltage far from the real value | Wrong `k2_voltage`. See [guide §3–4](hardware_and_calibration.md). |
| Current/power off by a constant factor under load | Wrong `k1_current`. See [guide §6](hardware_and_calibration.md). |
| Frequency about 1 Hz low/high | Internal oscillator tolerance: correct `clki_hz` ([guide §5](hardware_and_calibration.md)). |
| Current ≈ 2 mA and PF = 1.000 without load | Normal (offset/noise and no-load latch). |
| Readings frozen / `valid = false` | `GetMeasurements` returns `ERR_NOT_READY`; look for `measurement read failed` and `recovering chip` lines. |
| `chip is not factory calibrated` | All coefficients are `0xFFFF`: this chip cannot be used with the calibration-free method. |

## System problems

| Symptom | Cause / fix |
|---|---|
| Hard fault when the chip answers | ISR priority numerically lower than `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` (FreeRTOS API called from a too-urgent interrupt). |
| `ERR_NOT_READY` from `AppInit` | Called before `vTaskStartScheduler()`. Start it from a task. |
| Task stack overflow | Use ≥ 1024 words for the driver task when logging is enabled. |
| No log output | Wrong `log_huart`; terminal not on the ST-Link virtual COM port at 115200; `log_enabled=false`; `log_level` too low; compiled with `HLW8110_LOG_ENABLE=0`. |
| Garbage characters in the log | Terminal baud differs from the `huart3` baud. |
| Periodic `RST flag set` | Chip supply dips (brown-out below 2.7 V) – check the supply and decoupling. |
