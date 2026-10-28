# Hardware and calibration guide

This page explains **what K1 and K2 are, how to find them on your board, and how to verify and correct
voltage, frequency and current readings**.

> ⚠️ **Mains safety.** The HLW8110 ground is normally the mains neutral. Measure resistors only with the
> board fully disconnected from the mains. Run live tests only through an **isolated USB link** or an
> **isolation transformer** – never connect such a board directly to a PC.

## 1. How the chip measures

The chip has no ammeter or voltmeter inside. It measures **small voltages** and the driver turns them into amperes and volts:

```
 mains L ──► [load] ──► SHUNT (≈ 1 mΩ) ──► mains N        current → millivolts (V = I·R)  → IAP / IAN
 mains L ──► 5 × 200 kΩ ──┬──► VP pin                        voltage divider                 → VP
                          └── 1.65 kΩ ──► GND
```

- **Shunt:** a very small, precise resistor in series with the load. The current creates a tiny voltage across it
  (10 A through 1 mΩ = 10 mV). The chip amplifies it (PGA ×16), digitises it and reports a raw number. Power lost in the shunt is `I²·R`
  (10 A → 0.1 W), which is why it must be so small.
- **Divider:** reduces the mains voltage (≈ 325 V peak) to a few hundred millivolts for the VP pin.

The chip's factory coefficients assume a reference front end (1 mΩ shunt, 1 kΩ / 1 MΩ divider). The driver corrects for **your** front end with two factors.

## 2. K1 and K2

| Factor | Definition | Reference value |
|---|---|---|
| **K1** | shunt resistance in **milliohm** | 1 mΩ → `1.0` |
| **K2** | `1000 · Rsense / (Rtop + Rsense)` | 1 kΩ over 1 MΩ → `1.0` |

Readings are **inversely proportional** to K (a K that is too large shows too little):

| Quantity | Datasheet formula (chapter 10) | Driver output |
|---|---|---|
| Current | `Irms[mA] = RmsIA · RmsIAC / (K1 · 2²³)` | A |
| Voltage | `Urms[10 mV] = RmsU · RmsUC / (K2 · 2²²)` | V |
| Power | `P[W] = Power · PowerC / (K1 · K2 · 2³¹)` | W / VA |

## 3. Finding the values on your board

### 3.1 SMD marking codes

| Marking | Rule | Value |
|---|---|---|
| `R001` | `R` is the decimal point: 0.001 Ω | **1 mΩ** (`R002` = 2 mΩ, `R0005` = 0.5 mΩ) |
| `2003` | 4 digits: `200` × 10³ | **200 kΩ** |
| `1651` | 4 digits: `165` × 10¹ | **1.65 kΩ** |
| `101` | 3 digits: `10` × 10¹ | 100 Ω (the input filter resistors) |

Where to look: the **shunt** sits in the load current path and its two ends go (through two 100 Ω resistors) to pins IAP and IAN.
The **divider** is the resistor chain from the mains to pin VP; the single resistor from VP to GND is `Rsense`.

### 3.2 Worked example (this project's reference board)

| Part | Marking | Value |
|---|---|---|
| Shunt | `R001` | 1 mΩ → **K1 = 1.0** |
| Divider top | 5 × `2003` | 5 × 200 kΩ = 1 MΩ |
| Divider bottom | `1651` | 1.65 kΩ |

```c
cfg.k1_current = 1.0f;
cfg.k2_voltage = HLW8110_CalcK2(1000000.0f, 1650.0f);   /* = 1000·1650/(1000000+1650) = 1.6473 */
```

## 4. Verify and correct the voltage (no load needed)

1. Read the mains voltage with a multimeter **at the same moment** as the driver log line (mains voltage moves by ±1 V within seconds).
2. If they differ by more than your meter's accuracy:
   ```c
   cfg.k2_voltage = HLW8110_CorrectK(cfg.k2_voltage, displayed_V, actual_V);
   ```
3. A residual error below ~1 % is normal: multimeters are typically ±1 % on AC voltage and the chip's factory coefficients are good to ~±1 %.

If the displayed voltage is far off (e.g. 190 V instead of 230 V) the K2 value is wrong – recheck the resistor values.

## 5. Verify and correct the line frequency

The HLW8110 uses an **internal oscillator** (3.507 – 3.65 MHz, ±2 %) and the frequency is computed as `f = clki / (8 · UFREQ)`. Without correction it can read ~1 Hz off.

1. Compare with a reference (a multimeter with a Hz function, a mains-frequency web service, …). Note that the real mains frequency is **not** exactly 50.00 Hz – it wanders by about ±0.1 Hz.
2. Correct the clock value:
   ```c
   cfg.clki_hz = 3579545.0f * actual_Hz / displayed_Hz;   /* e.g. 3579545 · 50.00 / 49.32 */
   ```
   Keep the result inside 3.507 – 3.65 MHz. The value is valid for **this chip only** (and drifts slightly with temperature).

`clki_hz` changes only the frequency reading. The phase angle is computed as `ANGLE · 720 / UFREQ`, which does not depend on the clock.

## 6. Verify and correct the current (needs a load)

With no load the current register shows only offset/noise (a few mA) and **cannot** be used for calibration.

1. Connect a **resistive** load of at least some tens of watts (incandescent lamp, heater) through an isolation transformer.
2. Measure the real current with a clamp meter or a calibrated socket meter.
3. Correct K1:
   ```c
   cfg.k1_current = HLW8110_CorrectK(cfg.k1_current, displayed_A, actual_A);
   ```
4. Sanity check: with a purely resistive load the power factor should be ≈ 0.98 – 1.0, the phase angle ≈ 0° and `active_power_w ≈ voltage_v · current_a`.

Power (W, VA) is derived from K1 · K2 and follows automatically.

## 7. Behaviour without load (not a bug)

| Observation | Reason |
|---|---|
| Current ≈ 2 mA, power ≈ ±0.05 W | offset / noise of the shunt input; can be reduced with the offset registers (`RMSIAOS`, `PAOS`) |
| Power factor exactly 1.000 | below the start power (`PASTART`) the chip locks PF to 1.0 (datasheet 9.1.5) |
| Phase angle jumping randomly | no current signal to measure the angle against |

## 8. Further calibration (not implemented)

The chip also offers gain, phase and offset registers for meter-grade accuracy (datasheet chapter 11: `PAGain`, `PhaseA`, `PAOS`, `RMSIAOS` …),
normally adjusted against a reference energy meter. The driver exposes `HLW8110_WriteReg()` and the register map in `hlw8110_regs.h` if you need them.
