# Design history

Why the firmware looks the way it does: the key decisions, in order, and the
approaches that were tried and dropped. The full day-by-day journal (French,
`PLAN.md`) is in the development archive, a separate repository; this page is
the short version for reviewers.

## Goal and constraints

The stock shifter needs a vendor PC app to switch between H-pattern and
sequential. The goal: do everything from the lever, keep working with sim
profiles made for the stock device, and share the result with other owners.

Constraints that shaped everything:

- **Stock firmware is read-protected (RDP 1)** and cannot be dumped. The only
  option was a full rewrite, reproducing the stock HID interface and adding
  the new behaviour.
- **No sensor reports the mechanical lock** between H-pattern and
  sequential. The PC app only told the stock firmware which classification
  algorithm to use.
- **Minimal disassembly.** Taking the lever apart is painful, so every
  measurement session was batched and every algorithm was validated by
  replaying recorded data on a PC before flashing.

## Phase by phase

| Phase | Outcome |
|---|---|
| 0–1 | Photos, pinout "as found"; RDP 1 confirmed, no dump attempted |
| 2 | Full pinout mapped (six Hall sensors on PA0–PA5, collar on PB1) |
| 3 | Stock HID characterised: 16 buttons + 14 vendor bytes + 64-byte Output report; gears = buttons 1–6, reverse = 11. **Decision: reproduce this layout exactly** |
| 4 | A minimal diagnostic firmware streamed the raw sensors; every position's signature mapped. Showed that no single sensor identifies a gear and that sequential overlaps H-pattern sensors |
| 5 | Algorithms designed in Python on the captures, then ported to C with host tests |
| 6 | Flashed and debugged on real hardware with a composite HID + debug-port device; in-game investigation of missed shifts |

## Classification

- **Nearest neighbour on all six sensors**, not per-sensor thresholds. The
  Phase 4 map showed that each position is only distinct as a *combination*.
- **Per-sensor normalization** rather than one global scale, so that a
  sensor with a small swing that still discriminates (U6 in 2nd) is not
  drowned out by one with a large swing (U6 in 4th).
- **No interpolation** between calibrated points: classify against measured
  points only.

## Telling H-pattern from sequential

This took the most iterations.

1. **Initial idea: a deliberate repeated pattern** (5th → neutral ×3) to switch
   modes. Set aside once the sensor data was in.
2. **Duration alone separates them**: held gears last seconds, sequential taps
   100–300 ms (factor ×20). Amplitude alone fails, because seq − reads almost
   exactly like 3rd.
3. **Automatic mode inference** was tried next. Each calibrated position
   carries a mode; a movement of the other mode silently switches the mode and
   is not sent. It was abandoned because a slow, firm seq − could exceed the
   hold timeout and be classified as 3rd. That was a real conflict between the
   user's latency budget (< 250 ms) and the variability of human movements.
4. **A lateral "shake" gesture** was tried: its activity score could not be
   told apart from resting noise. Abandoned.
5. **Final design: explicit gestures** built on the collar, which gives a
   clean digital signal, combined with a distinctive lever position: hold 6th
   + two blips → H; two seq − taps + neutral + two blips → SEQ.

## Calibration

1. **On-the-fly recalibration**: holding the collar while a position is held
   would overwrite that point. Implemented and validated, then rejected by the
   user as too easy to trigger by accident and hard to reason about.
2. **Final design: a dedicated, guided calibration mode**, entered with a
   distinctive gesture (collar up, 1 → 5 → 6 → 2), nine points in a fixed order,
   2 s holds, LED feedback, no game input while active.
3. **Persistence** in the last flash page with slot rotation, so that the
   ~10 000-cycle erase endurance is never a concern.
4. **The LED shows which point is expected** (a breath, then *n* pulses). The
   earlier plain blink did not say which point was next, and field captures
   showed points recorded one step off.
5. **Saved in one go** after the ninth point. Points were first written to
   flash one by one, so a mistake was permanent; now unplugging mid-way
   cancels the session, and a calibration costs one flash slot instead of
   nine.
6. **Obviously wrong points are refused** (Oct. 2026): a lever left in
   neutral or on a position already recorded, and shift up / shift down
   swapped. A first version only caught a lever that had not moved; a field
   session with neutral taken back in 1st (157 from the first reading) led to
   the current thresholds, then a second session with five deliberate
   mistakes got five refusals. A calibration reset over USB
   (`shifternoumi-setup reset-calibration`) replaced erasing the page with
   STM32CubeProgrammer.

The mode (H / SEQ) is also remembered across plug-ins, in a page of its own
so that erasing it can never touch the calibration.

## Movement tracker fixes found on real data

- Classify the **mean of the episode**, not a single sample (3rd vs seq −
  noise).
- Exclude the still-near-neutral `PENDING` samples from `HELD`, but not from
  `SEQ`: brief taps need every sample.
- **Continuous re-evaluation while held** (`HELD_CORRECTED`): the first
  confirmation can fire before the lever finishes travelling, and direct
  gate-to-gate moves never pass through neutral.

## Debugging on hardware

- SWO trace was planned but cannot easily be read by a standalone script.
  **Replaced by a composite USB device**: the gamepad plus a CDC port
  streaming a CSV line of internal state every step. The F1 USB library only
  supports one class per device, hence the custom `usbd_composite` class.
- Field bugs fixed during Phase 6 (all regression-tested on the captures):
  - the CDC line buffer was on the stack while the USB interrupt was still
    reading it;
  - the reverse button flickered while the collar moved in 1st (now latched on
    entry);
  - `HELD_CORRECTED` was ignored in three detectors;
  - stale blips blocked new pairs;
  - gesture windows were not renewed;
  - the gesture timeout was raised from 2.5 s to 5 s and the maximum blip
    length from 0.5 s to 0.7 s after measuring real gestures.
- An **LED** was added so that calibration and mode switches can be followed
  without a PC.

## The missed-shift investigation (Sept 2026)

Symptom: in Assetto Corsa, H-pattern shifts sometimes had to be "broken down"
to register.

1. **HID vs game telemetry** (`capture_session.py`): every HID button reached
   the game, but game acceptance latency was bimodal (5–20 ms or
   0.3–1.8 s).
2. **Hypothesis: inert vendor axes.** The stock firmware filled the 14
   vendor bytes with live sensor values while this one sent zeros. They were
   populated; no measurable effect.
3. **A wrong turn: blaming the clutch.** Pedal telemetry showed AC
   accepting gears during the clutch *release*. This was read as the cause:
   the firmware was declared blameless and the driver was advised to release
   the clutch more smoothly. That diagnosis was wrong, and the driver was
   right all along ("the input arrives after the movement is over").
4. **The firmware was late, and sometimes silent.** Measured on the same
   captures, the lever had been in the gate for **300–500 ms** before the
   button was sent: 100 ms sampling, plus 2 confirmation samples, plus the
   250 ms hold timeout, all needed only to tell a held gear from a
   sequential tap. Gears therefore arrived while the clutch was already
   coming up, which is what the telemetry had shown. Worse, 15 shifts in the
   captures were never sent at all (direct 6→5, quick 4→3→4). The button was
   correct and stable, which is why the firmware had been called blameless;
   nobody had measured when it arrived.
5. **Fix: the fast path** (`fast_gear`). Since the mode is explicit, the
   firmware does not need duration to classify: it engages a position as soon
   as the lever is in it, every 1 ms, while the slow path keeps handling
   gestures and calibration. Extended to sequential (pulse at the stop rather
   than on release) and hardened with a margin rule, a wait for neutral after
   a mode switch, and a calibration-entry mask, all found by replaying the
   captures. Replay result: 323 ms earlier (median), 15 dropped shifts
   recovered. Validated in game in Oct 2026: 13 ms median from the lever
   reaching the gate to the gear in Assetto Corsa, no shift missed.

## Distribution (Sept.–Oct. 2026)

Goal: other owners should not need an IDE. A 4 KB USB HID bootloader (written
from scratch) sits in front of the app so that updates need only the
shifter's cable; the first install still needs an ST-Link, because the F103's
ROM bootloader has no USB. `shifternoumi-setup` (Windows and Linux) wraps both,
and GitHub Actions builds the release files.

Everything was then validated on hardware, one step at a time
([USB bootloader → Hardware validation](bootloader.md#hardware-validation)).
The read-protected case found two installer bugs: the flash size cannot be
read while the chip is protected, and OpenOCD's protection removal did not
erase the flash, so the installer now erases it itself. The installer also
learned to name the firmware it sees on USB, to refuse to run with the board
powered by both USB and the ST-Link, and to say plainly when it waits for
the user.

In October 2026 the firmware was named **ShifterNoumi** and released under
the Apache License 2.0.
