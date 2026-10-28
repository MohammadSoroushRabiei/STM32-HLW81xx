# Changelog

All notable changes are documented here. The project follows [Semantic Versioning](https://semver.org/).

## [1.1.0] - 2026-10-28

### Added
- **Several chips in one application**: every `HLW8110_Handle_t` is fully independent (own UART, K1/K2, clock, task).
- **Shared log UART**: all driver instances use one internal, reference-counted log mutex, so several chips can write to the same log UART without mixed or lost lines.
- `log_name` in `HLW8110_Config_t`: tag printed in every log line (`[1520][I][PDU1] ...`).
- Example `examples/two_chips_one_log` and a "Multiple chips" chapter in the README.
- Tests: two simulated chips with different K2/registers, shared log, semaphore leak check (70 checks).

### Changed
- `HLW8110_Handle_t` no longer contains `log_mutex` (private field; use `log_name`/`log_*` config instead).

## [1.0.0] - 2026-10-27

First public release.

### Added
- Interrupt-driven UART driver for HLW8110 (and HLW8112) for STM32 HAL + FreeRTOS.
- Background measurement task with thread-safe snapshot (`HLW8110_GetMeasurements`).
- Automatic health checks: chip reset detection (RST flag), configuration checksum, recovery after repeated errors.
- Optional text log on a second UART (levels, run-time on/off, compile-time removal).
- `HLW8110_ConfigDefault`, `HLW8110_CalcK2`, `HLW8110_CorrectK`, `HLW8110_Deinit`.
- Register map split into `hlw8110_regs.h`; full Doxygen documentation.
- Host-side test suite with a simulated chip (`make -C tests test`).

### Changed
- Phase angle is computed as `ANGLE * 720 / UFREQ`, which is independent of the inaccurate internal oscillator.
- HAL header is configurable (`HLW8110_HAL_HEADER`, default `main.h`).
