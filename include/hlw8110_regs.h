/**
 * @file    hlw8110_regs.h
 * @brief   Register map and bit definitions of the HLW8110 / HLW8112.
 *
 * Source: HLW8110/HLW8112 datasheet REV 1.01, chapter 9 (registers) and
 * chapter 12.2 (UART protocol).
 *
 * @note Registers 0x00..0x1F and 0x40 (IE) are write protected: the driver
 *       sends the "write enable" special command around every write to them.
 */
#ifndef HLW8110_REGS_H
#define HLW8110_REGS_H

/**
 * @defgroup hlw8110_regs Register map
 * @brief Register addresses (7 bit, as sent in the UART command byte).
 * @{
 */

/** @name Calibration / control registers (write protected) */
/** @{ */
#define HLW8110_REG_SYSCON        0x00U /**< System control: PGA gains, ADC enables (reset 0x0A04). */
#define HLW8110_REG_EMUCON        0x01U /**< Energy measure control (reset 0x0000). */
#define HLW8110_REG_HFCONST       0x02U /**< Pulse frequency constant (reset 0x1000). */
#define HLW8110_REG_PASTART       0x03U /**< Channel A start (no-load) power threshold (reset 0x0060). */
#define HLW8110_REG_PBSTART       0x04U /**< Channel B start power threshold (HLW8112). */
#define HLW8110_REG_PAGAIN        0x05U /**< Channel A active power gain calibration. */
#define HLW8110_REG_PBGAIN        0x06U /**< Channel B active power gain calibration. */
#define HLW8110_REG_PHASEA        0x07U /**< Channel A phase calibration (8 bit). */
#define HLW8110_REG_PHASEB        0x08U /**< Channel B phase calibration (8 bit). */
#define HLW8110_REG_PAOS          0x0AU /**< Channel A active power offset. */
#define HLW8110_REG_PBOS          0x0BU /**< Channel B active power offset. */
#define HLW8110_REG_RMSIAOS       0x0EU /**< Channel A current RMS offset. */
#define HLW8110_REG_RMSIBOS       0x0FU /**< Channel B current RMS offset. */
#define HLW8110_REG_IBGAIN        0x10U /**< Channel B current gain (HLW8112). */
#define HLW8110_REG_PSGAIN        0x11U /**< Apparent power gain calibration. */
#define HLW8110_REG_PSOS          0x12U /**< Apparent power offset calibration. */
#define HLW8110_REG_EMUCON2       0x13U /**< Energy measure control 2: update rate, waveform, ZX, PF (reset 0x0001). */
/** @} */

/** @name Measurement registers (read only) */
/** @{ */
#define HLW8110_REG_PFCNT_PA      0x20U /**< Fast pulse counter A. */
#define HLW8110_REG_PFCNT_PB      0x21U /**< Fast pulse counter B. */
#define HLW8110_REG_ANGLE         0x22U /**< Phase angle between U and I (16 bit). */
#define HLW8110_REG_UFREQ         0x23U /**< Line frequency period register (16 bit). */
#define HLW8110_REG_RMSIA         0x24U /**< Current A RMS (24 bit). */
#define HLW8110_REG_RMSIB         0x25U /**< Current B RMS (24 bit, HLW8112). */
#define HLW8110_REG_RMSU          0x26U /**< Voltage RMS (24 bit). */
#define HLW8110_REG_PF            0x27U /**< Power factor (24 bit signed). */
#define HLW8110_REG_ENERGY_PA     0x28U /**< Energy pulse count A. */
#define HLW8110_REG_ENERGY_PB     0x29U /**< Energy pulse count B. */
#define HLW8110_REG_POWER_PA      0x2CU /**< Active power A (32 bit signed). */
#define HLW8110_REG_POWER_PB      0x2DU /**< Active power B (32 bit signed, HLW8112). */
#define HLW8110_REG_POWER_S       0x2EU /**< Apparent power (32 bit signed). */
#define HLW8110_REG_EMUSTATUS     0x2FU /**< Status + checksum of the calibration registers. */
#define HLW8110_REG_PEAK_IA       0x30U /**< Peak current A. */
#define HLW8110_REG_PEAK_IB       0x31U /**< Peak current B. */
#define HLW8110_REG_PEAK_U        0x32U /**< Peak voltage. */
#define HLW8110_REG_INSTAN_IA     0x33U /**< Instantaneous current A. */
#define HLW8110_REG_INSTAN_IB     0x34U /**< Instantaneous current B. */
#define HLW8110_REG_INSTAN_U      0x35U /**< Instantaneous voltage. */
#define HLW8110_REG_WAVE_IA       0x36U /**< Waveform current A. */
#define HLW8110_REG_WAVE_IB       0x37U /**< Waveform current B. */
#define HLW8110_REG_WAVE_U        0x38U /**< Waveform voltage. */
#define HLW8110_REG_INSTANTAN_P   0x3CU /**< Instantaneous active power. */
#define HLW8110_REG_INSTANTAN_S   0x3DU /**< Instantaneous apparent power. */
/** @} */

/** @name Interrupt and system registers */
/** @{ */
#define HLW8110_REG_IE            0x40U /**< Interrupt enable (write protected). */
#define HLW8110_REG_IF            0x41U /**< Interrupt flags (clear on read). */
#define HLW8110_REG_RIF           0x42U /**< Reset interrupt flags. */
#define HLW8110_REG_SYSSTATUS     0x43U /**< System status: bit0 RST (clear on read), bit4 WREN. */
#define HLW8110_REG_RDATA         0x44U /**< Last data read. */
#define HLW8110_REG_WDATA         0x45U /**< Last data written. */
/** @} */

/** @name Factory conversion coefficients (read only, 0xFFFF = chip not calibrated) */
/** @{ */
#define HLW8110_REG_CHECKSUM      0x6FU /**< Checksum of the coefficient registers. */
#define HLW8110_REG_RMSIA_C       0x70U /**< Current A RMS coefficient. */
#define HLW8110_REG_RMSIB_C       0x71U /**< Current B RMS coefficient. */
#define HLW8110_REG_RMSU_C        0x72U /**< Voltage RMS coefficient. */
#define HLW8110_REG_POWERPA_C     0x73U /**< Active power A coefficient. */
#define HLW8110_REG_POWERPB_C     0x74U /**< Active power B coefficient. */
#define HLW8110_REG_POWERS_C      0x75U /**< Apparent power coefficient. */
#define HLW8110_REG_ENERGYA_C     0x76U /**< Energy A coefficient. */
#define HLW8110_REG_ENERGYB_C     0x77U /**< Energy B coefficient. */
/** @} */
/** @} */ /* hlw8110_regs */

/**
 * @defgroup hlw8110_bits Register bit fields
 * @brief Bit masks of the registers that the driver configures.
 * @{
 */

/** @name SYSCON (0x00, reset 0x0A04)
 *  PGA field: `1XX` = x16, `011` = x8, `010` = x4, `001` = x2, `000` = x1. */
/** @{ */
#define HLW8110_SYSCON_PGAIA_MASK (7UL << 0)  /**< PGA of current channel A. */
#define HLW8110_SYSCON_PGAU_MASK  (7UL << 3)  /**< PGA of the voltage channel. */
#define HLW8110_SYSCON_PGAIB_MASK (7UL << 6)  /**< PGA of current channel B. */
#define HLW8110_SYSCON_ADC1ON     (1UL << 9)  /**< Enable current channel A ADC. */
#define HLW8110_SYSCON_ADC2ON     (1UL << 10) /**< Enable current channel B ADC (HLW8112 only). */
#define HLW8110_SYSCON_ADC3ON     (1UL << 11) /**< Enable voltage channel ADC. */
/** @} */

/** @name EMUCON2 (0x13, reset 0x0001) */
/** @{ */
#define HLW8110_EMUCON2_CHS_IB      (1UL << 7) /**< 1 = measure IB current (HLW8112). */
#define HLW8110_EMUCON2_PFACTOR_EN  (1UL << 6) /**< Enable power factor / apparent power output. */
#define HLW8110_EMUCON2_WAVE_EN     (1UL << 5) /**< Enable waveform / instantaneous data (needed for ZX). */
#define HLW8110_EMUCON2_ZX_EN       (1UL << 2) /**< Enable zero-crossing, phase angle and frequency. */
#define HLW8110_EMUCON2_DUPSEL_MASK (3UL << 8) /**< Data update rate: 0=3.4 1=6.8 2=13.65 3=27.3 Hz. */
/** @} */
/** @} */ /* hlw8110_bits */

/**
 * @defgroup hlw8110_proto UART protocol constants
 * @brief Bytes of the HLW8110 UART protocol (datasheet 12.2).
 *
 * Read:  host sends `0xA5, reg`; chip answers `data(MSB first)..., checksum`.
 * Write: host sends `0xA5, 0x80|reg, data..., checksum`.
 * Checksum = bitwise NOT of the 8-bit sum of all preceding bytes of the frame.
 * @{
 */
#define HLW8110_UART_HEADER       0xA5U /**< First byte of every frame. */
#define HLW8110_CMD_SPECIAL       0xEAU /**< Special command prefix. */
#define HLW8110_CMD_WRITE_ENABLE  0xE5U /**< Unlock write-protected registers. */
#define HLW8110_CMD_WRITE_DISABLE 0xDCU /**< Lock write-protected registers. */
#define HLW8110_CMD_SELECT_A      0x5AU /**< Use current channel A for PF / apparent power / angle. */
#define HLW8110_CMD_SELECT_B      0xA5U /**< Use current channel B (HLW8112). */
#define HLW8110_CMD_RESET         0x96U /**< Reset the chip. */
/** @} */

#endif /* HLW8110_REGS_H */
