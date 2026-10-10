# Configuration reference

Every tunable constant in the firmware, its value, and why it has that value.
All of them are compile-time `#define`s; there is no runtime configuration
apart from calibration.

Before changing a timing constant of the slow path, note that most were tuned
on real captures at the 100 ms cadence. Re-validate with the replay tools
([Testing and tools](testing-and-tools.md)) on the captures in
`captures/` (more in the development archive).

## Main loop — `Core/Src/main.c`

| Constant | Value | Meaning |
|---|---|---|
| `MAIN_LOOP_PERIOD_MS` | 100 | Slow-step period (state machine, debug trace, LED). Every slow-path constant below assumes it |
| `FAST_LOOP_PERIOD_MS` | 1 | Period of the main loop: each iteration waits until this many SysTicks have passed since it started. Before Sept. 2026 this was `HAL_Delay(1)`, which waits 1–2 ticks (≈2 ms period) |

## USB — `USB_DEVICE/Target/usbd_conf.h`, `usbd_desc.c`

| Constant | Value | Meaning |
|---|---|---|
| `HID_FS_BINTERVAL` | 1 | Host polling interval of the gamepad endpoint, ms (was 10) |
| `USBD_VID` / `USBD_PID_FS` | 0x0483 / 0x572C | Device identity |
| `USBD_PRODUCT_STRING_FS` | `"ShifterNoumi"` | Name shown by Windows and games |

## Classification — `App/Inc/gear_position.h`

| Constant | Value | Meaning |
|---|---|---|
| `GEAR_NEUTRAL_DIST_THRESHOLD` | 0.25 | Normalized distance to neutral below which a sample counts as neutral. Resting noise < 0.2, positions > 0.45 |

## Fast path — `App/Inc/fast_gear.h`

| Constant | Value | Meaning |
|---|---|---|
| `FAST_GEAR_ENGAGE_DIST` | 0.35 | Max distance to a point to engage it. In-gate p90 ≈ 0.15, p99 ≈ 0.30; closest pair of H points 0.83 |
| `FAST_GEAR_MIN_NEUTRAL_DIST` | 0.35 | Min distance from neutral to engage anything. Keeps resting noise away from seq + (0.46 from neutral) |
| `FAST_GEAR_MARGIN` | 0.08 | Required lead of the nearest point over the second nearest. Separates real 3rd (lead ≥ 0.11 observed) from readings halfway between 3rd and seq − (lead 0.00–0.05) |
| `FAST_GEAR_RELEASE_DIST` | 0.60 | Distance from the engaged point beyond which it starts releasing (hysteresis vs 0.35) |
| `FAST_GEAR_ENGAGE_MS` | 6 | Candidate must be stable this long before engaging (spike filter) |
| `FAST_GEAR_RELEASE_MS` | 15 | Must be out of the gate (or at neutral) this long before releasing |

## HID output — `App/Inc/hid_report.h`

| Constant | Value | Meaning |
|---|---|---|
| `FAST_PULSE_MS` | 100 | Duration of a sequential shift pulse. Well above a game's polling period, short enough for back-to-back shifts (~400 ms apart at the fastest observed) |
| `FAST_PULSE_GAP_MS` | 30 | Forced gap between two queued pulses of the same button, so the game sees two edges |
| `HID_PULSE_STEPS` | 2 | Slow-path pulse length in slow steps. Its pulses are now overridden by the fast path; kept for the slow-path unit tests |
| `HID_BUTTON_*` | 0–7, 10 | Bit indices: gears 1–6 → 0–5, shift up 6, shift down 7, reverse 10 (buttons are numbered from 1 in games) |
| `HID_DEBUG_BYTE_INDEX` | 14 | Report byte carrying the mode/calibration debug bits |

## Collar — `App/Inc/collar_switch.h`

| Constant | Value | Meaning |
|---|---|---|
| `COLLAR_DEBOUNCE_STEPS` | 2 | Consecutive identical reads needed to change state. Applied at both rates: 100–200 ms on the slow debouncer, 1–2 ms on the fast one |

## Movement tracker — `App/Inc/mode_state_machine.h`

| Constant | Value | Meaning |
|---|---|---|
| `CONFIRM_SAMPLES` | 2 | Consecutive non-neutral samples before a movement starts rising |
| `HOLD_TIMEOUT_MS` | 250 | Rising this long without returning to neutral → `HELD`; returning earlier → `SEQ` |
| `HELD_WINDOW` | 5 | Sliding window (samples) re-classified while held |
| `MOVEMENT_ACCUM_MAX` | 64 | Max samples averaged per movement (≈6 s at 100 ms; extra samples are ignored) |

## Mode-switch gestures — `App/Inc/mode_state_machine.h`

| Constant | Value | Meaning |
|---|---|---|
| `GESTURE_TIMEOUT_MS` | 5000 | Window renewed at each gesture step. Raised from 2500 after field tests: reaching the first blip took ~2.3 s |
| `BLIP_MIN_MS` / `BLIP_MAX_MS` | 50 / 700 | Accepted collar blip duration. Max raised from 500: real blips measured 510–550 ms |
| `INTER_BLIP_MAX_MS` | 1200 | Max gap between the two blips of a pair |
| `SEQDOWN_MIN_COUNT` | 2 | seq − taps required before the to-SEQ blips |
| `GESTURE_BLIP_MAX` | 8 | Blip history size per detector |

## Calibration — `App/Inc/mode_state_machine.h`, `App/Inc/calibration_flash.h`

| Constant | Value | Meaning |
|---|---|---|
| `CALIBRATION_HOLD_MS` | 2000 | Collar hold before a point starts accumulating. Far from `BLIP_MAX_MS`, so a blip can never save a point |
| `CALIBRATION_SEQUENCE` | 1st…6th, seq +, seq −, neutral | Order of the nine points |
| `CALIBRATION_MIN_GAP_L1` | 400 | A point closer than this (L1, ADC steps) to a point already recorded in the session is refused. Distinct points (except 3rd / seq −) are ≥ ~700 apart; a lever put back in a recorded gate lands up to ~160 away |
| `CALIBRATION_MIN_GAP_CLOSE_L1` | 150 | Same, between 3rd and seq −, which are really 347–516 apart |
| `CALIBRATION_MIN_NEUTRAL_L1` | 300 | A point (other than neutral) closer than this to the current neutral is refused. Neutral held again: 63–95; nearest real point: seq +, ~810 |
| `CALIBRATION_FLASH_PAGE_ADDR` | `0x0800FC00` | Last 1 KB flash page |
| `CALIBRATION_FLASH_SLOT_SIZE` / `_COUNT` | 128 B / 8 | Wear-levelling slots per page |
| `MODE_FLASH_PAGE_ADDR` | `0x0800F800` | Settings page, just below the calibration page (`GX100_SETTINGS_PAGE`) |
| `MODE_FLASH_RECORD_H` / `_SEQ` | `0x4848` / `0x5353` | One 16-bit record appended per mode switch; the page is erased when its 512 records are used |

The built-in default calibration (used until the first calibration is saved)
is `DEFAULT_CALIBRATION` in `App/Src/calibration_store.c`; its values are
listed in [Hardware](hardware.md#what-each-position-looks-like-to-the-sensors).

## Status LED — `App/Src/status_led.c`

| Constant | Value | Meaning |
|---|---|---|
| `STATUS_LED_BREATH_MS` | 2000 | One breath of the calibration "waiting" pattern (~0.5 Hz) |
| `STATUS_LED_GAP_MS` / `_REST_MS` | 500 / 1000 | Dark time after the breath / after the pulses |
| `STATUS_LED_PULSE_ON_MS` / `_OFF_MS` | 250 / 250 | Pulses showing the expected point |
| `STATUS_LED_NEUTRAL_ON_MS` | 1000 | Single long pulse for neutral |
| `STATUS_LED_FLASH_MS` / `_COUNT` | 80 / 3 | Point-saved flash |
| `STATUS_LED_SWITCH_MS` / `_COUNT` | 100 / 2 | Mode-switched flash |
| `STATUS_LED_REJECT_MS` / `_COUNT` | 50 / 10 | Point-refused flicker (1 s) |
| `STATUS_LED_HOLD_BLIP_MS` / `_GAP_MS` / `_REST_MS` | 100 / 150 / 500 | "Ready to save" double-blip pattern |
| `STATUS_LED_MIN_LEVEL` | 16 | Brightness (0–255) below which the software PWM switches the LED off rather than flicker |

Patterns start on a 100 ms slow step, then run with 1 ms timing.
