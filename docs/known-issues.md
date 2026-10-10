# Known issues and limitations

What is known to be missing, fragile or wrong. Grouped by who it affects.
Each item points to the code involved.

## For drivers

### Installer validated on one setup only
`shifternoumi-setup` and the bootloader were validated on hardware in Oct
2026 ([details](bootloader.md#hardware-validation)), but with one ST-Link V2
clone, on a Windows PC that already had ST's tools installed. A clean PC with
only ST's driver, Linux, and other ST-Links are untested. The Windows executable
is not signed, so SmartScreen warns about it.
Also missing: shifternoumi-setup does not install the ST-Link driver on Windows
(it tells you to), and it cannot connect under reset (no NRST wire), so a
chip whose firmware disables the SWD pins still needs STM32CubeProgrammer's
*Under reset* mode.

### 3rd gear engages a little later than the others
Measured in Assetto Corsa (Oct 2026): from the lever entering the gate to
the gear in game, 13 ms median, 56 ms for 90 % of shifts, no shift missed.
The slowest (up to ~100 ms) are all 3rd: in H-pattern the firmware waits
until the lever is clearly closer to 3rd than to the seq − position next to
it, so that the switch-to-sequential gesture never sends 3rd to the game
(`FAST_GEAR_MARGIN`).

### Calibration points are only partly checked
The firmware refuses a point taken in neutral, on a position already
recorded in the session, or with shift up and shift down swapped (Oct
2026). It cannot tell whether the lever is in the *requested* gear when
that gear is not recorded yet (4th recorded at the 3rd step is accepted),
and it does not check that the neutral point is taken at rest: a lever held
somewhere unusual (between gates) at the last step is saved as neutral. Unplugging before the ninth point discards the session;
after that, the fix is recalibrating, or `shifternoumi-setup reset-calibration`
if gears are too broken to enter calibration mode.
(`calibration_mode_rejects`, `calibration_store_commit`)

### Side effects of gestures
The calibration-entry gesture (collar up, 1 → 5 → 6 → 2) sends reverse, 5th
and 6th to the game in H-pattern mode; only the final 2nd is suppressed (the
firmware only knows it is the gesture once 2nd is reached). Switching to
H-pattern while holding 6th does *not* send 6th: after a mode switch nothing
is engaged until the lever has been back to neutral (`fast_gear`).

### Validated on one unit, one game
Default calibration and every threshold come from a single shifter. Other
production batches may need a calibration (expected) or different
thresholds (unknown). In-game testing was done in Assetto Corsa only.

## For builders

### CubeMX regeneration breaks the USB device
`usb_device.c` registers the composite class outside a `USER CODE` block, and
several other generated files carry hand edits. See
[Building and flashing → Regenerating with CubeMX](building-and-flashing.md#regenerating-with-cubemx).

## USB identity

The device uses ST's vendor ID `0x0483` with a product ID chosen for this
project (`0x572C`, `usbd_desc.c`). That is fine for personal use, but a VID/PID
pair that is not allocated to the project should not be distributed. Options:
request a free PID from [pid.codes](https://pid.codes) (for open-source
hardware/firmware) or ST's own programme.

## For contributors

### Old captures replay with the default calibration
Captures recorded before the `CAL` line existed have no `_cal.csv`, so
`replay_fast_path` uses the default calibration; those taken after a
recalibration (`test_H_pattern4/6/7`, `test_H_pattern`, `test1`,
`test_Ben`) replay unreliably. New captures carry their calibration.

### Dead or duplicated code
- Slow-path gear and shift bits built by `hid_report_build` are now always
  overwritten by the fast path outside calibration (`HID_PULSE_STEPS` is only
  exercised by unit tests).
- The HID report descriptor exists twice (`usbd_composite.c` and the
  middleware `usbd_hid.c`).
- The 64-byte HID Output report is declared, but there is no OUT endpoint
  and `SET_REPORT` is not handled.
- The `.ioc` still routes PB3 to SWO trace (unused leftover).

### Code comments are in French
The code is commented in French (comments may refer to `PLAN.md`, the
development journal kept in the archive repository); this documentation is
in English.

## Before publishing

ShifterNoumi is under the Apache License 2.0 (Oct 2026). The repository is
still private; before making it public:

- Ask [pid.codes](https://pid.codes) for a USB PID (they require an
  open-source license, which is now the case) instead of `0x572C`/`0x572D`
  under ST's vendor ID.
- ST's USB device library (`Firmware/Middlewares/`) is under ST's SLA0044
  license (use on ST microcontrollers only), not Apache 2.0: say so where
  the license is announced (done in the README) so nobody assumes the whole
  tree is Apache.

The shifternoumi-setup executables bundle OpenOCD (GPL-2.0-or-later), unmodified
(see [THIRD-PARTY-NOTICES](../Wizard/THIRD-PARTY-NOTICES.md)). Before
publishing them, make each release meet the GPL's source requirement, for
example by attaching the xPack OpenOCD source archive to the release.

## Debug build size

The debug build of the app is 53.8 KB for 59.4 KB available behind the
bootloader (Oct 2026). Release builds (`-Os`, 27.5 KB) are what ships; if
the debug build stops fitting, build it with `-Og` or drop the slow-path
leftovers listed under *Dead or duplicated code*.
