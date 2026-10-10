# Hardware

What is inside the GX100, how it is wired, and what the sensors see. All of
this was reverse-engineered from one unit; see the
[schematic](../hardware/kicad/PCB.kicad_sch) and the [photos](../hardware/photos/).

## The board

- **MCU module**: STM32F103C8T6 "Blue Pill" (72 MHz Cortex-M3, 64 KB flash,
  20 KB RAM, no FPU) with an 8 MHz crystal, USB-C, reset button and BOOT0
  jumper. It plugs into a socket on a custom black carrier PCB, so it can be
  lifted out (to reach its pins more easily when flashing, for example).
- **Sensors**: six linear Hall-effect sensors marked `49E` (SM353LT / AH49E
  family, analog output around VCC/2 at rest), referenced **U2–U7**, arranged
  around a cut-out. A magnet fixed under the lever moves above them.
- **Collar switch**: the ring you lift to engage reverse. It is a plain switch
  on screw terminal **J1**.
- **J2**: a 6-pin screw terminal that is not used on the tested unit (3 signal
  pins + 3 grounds).
- **Status LED**: the Blue Pill's on-board LED (PC13, active low) — used by this
  firmware for calibration and mode-switch feedback.

## Pinout

| Function | STM32 pin | Peripheral / mode |
|---|---|---|
| U2 Hall output | PA0 | ADC1_IN0 |
| U3 Hall output | PA1 | ADC1_IN1 |
| U4 Hall output | PA2 | ADC1_IN2 |
| U5 Hall output | PA3 | ADC1_IN3 |
| U6 Hall output | PA4 | ADC1_IN4 |
| U7 Hall output | PA5 | ADC1_IN5 |
| Collar (J1) | PB1 | GPIO input, **internal pull-up required** |
| J2 pin 1 (unused) | PB0 | — |
| J2 pin 2 (unused) | PA7 | (ADC1_IN7 capable) |
| J2 pin 3 (unused) | PA6 | (ADC1_IN6 capable) |
| J2 pins 4–6 | GND | |
| Status LED | PC13 | GPIO output, low = on |
| USB | PA11/PA12 | USB FS device |
| SWD | PA13/PA14 | ST-Link |

The collar line floats without the internal pull-up: with the pull-up it reads
`1` when lifted and `0` at rest.

## Clocks

HSE 8 MHz → PLL ×9 → **72 MHz** SYSCLK. USB = PLL / 1.5 = 48 MHz. ADC =
PCLK2 / 6 = 12 MHz. With 28.5-cycle sampling, one scan of the six channels takes
about 20 µs, so the DMA buffer is refreshed far faster than the firmware reads
it.

## Read-out protection on the stock firmware

The stock firmware is protected at **RDP level 1**: the flash cannot be read
through SWD. Dropping to level 0 is needed before flashing anything. With
STM32CubeProgrammer (the first time, on the stock firmware) it erased the
whole flash; with OpenOCD's `stm32f1x unlock` on the same unit (Oct. 2026) it
did **not** (calibration and saved mode survived), so
`shifternoumi-setup install` erases the whole flash itself afterwards. Either
way the stock firmware is gone for good and cannot be backed up. The
procedure is in [Building and flashing](building-and-flashing.md#1-remove-the-read-protection-one-time).

## The stock USB device (for reference)

Characterised before replacing it (scripts in the development archive):

- VID `0x04B0`, PID `0x5750`, product string `GX100`.
- One HID gamepad interface: 16 buttons (2 bytes) + 14 vendor bytes (the raw
  Hall values), plus a 64-byte vendor Output report the PC app uses to
  configure the device.
- Buttons 1–6 = gears 1–6, button 11 = reverse. One bit per position, held
  while the lever stays in the gate.

This firmware keeps the same report layout so existing sim profiles keep
working, and adds buttons 7/8 for sequential. See
[USB interface](usb-interface.md).

## What each position looks like to the sensors

Raw ADC values (12 bit, 0–4095; rest ≈ 2800–3100 on every sensor). These are
the default calibration shipped in the firmware
(`Firmware/App/Src/calibration_store.c`), measured on the reference unit:

| Position | U2 | U3 | U4 | U5 | U6 | U7 | Dominant sensor(s) |
|---|---|---|---|---|---|---|---|
| Neutral | 2977 | 3093 | 2917 | 2979 | 2772 | 2902 | — |
| 1st | **3792** | **3771** | 2946 | 3017 | 2826 | 2990 | U2 + U3 |
| 2nd | 3012 | 3109 | 2980 | **3771** | 3375 | 2938 | U5 (+U6) |
| 3rd | 3183 | **4095** | 3146 | 3024 | 2800 | 2949 | U3 (saturated) |
| 4th | 3020 | 3078 | 2973 | 3188 | **4046** | 3067 | U6 |
| 5th | 3008 | **3747** | **3727** | 3045 | 2823 | 2942 | U3 + U4 |
| 6th | 3062 | 3103 | 2942 | 3008 | 3538 | **3664** | U7 (+U6) |
| Seq + | 2970 | 3016 | 2923 | 3082 | **3321** | 2940 | U6 (short travel) |
| Seq − | 3037 | **3942** | 2998 | 2991 | 2789 | 2924 | U3 |

Two things in this table drive the whole design:

1. **No single sensor identifies a gear.** U3 is involved in 1st, 3rd and 5th.
   Classification has to use all six values together (see
   [Algorithms](algorithms.md#1-classifying-a-sample)).
2. **Sequential shares sensors with H-pattern.** Locking the lever for
   sequential shortens its travel: *seq +* is a weaker version of 4th, and
   *seq −* is almost the same reading as 3rd (they are only 0.30 apart after
   normalization). The firmware cannot tell the two modes apart from the
   sensors alone, so the **mode is set explicitly** with a gesture
   ([User guide](user-guide.md#switching-between-h-pattern-and-sequential)).
   There is no switch that detects the mechanical lock.

Reverse is the 1st-gear position with the collar lifted — the sensors see
1st, the collar input tells them apart.
