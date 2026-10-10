# User guide

How to drive with the GX100 on this firmware: what the game receives, how to
switch modes, how to calibrate, and what the LED means. Everything is done on
the lever — no PC software is needed.

## Contents

- [What your PC sees](#what-your-pc-sees)
- [Button mapping](#button-mapping)
- [H-pattern mode](#h-pattern-mode)
- [Reverse](#reverse)
- [Sequential mode](#sequential-mode)
- [Switching between H-pattern and sequential](#switching-between-h-pattern-and-sequential)
- [Calibration](#calibration)
- [LED codes](#led-codes)
- [Updating the firmware](#updating-the-firmware)
- [Troubleshooting](#troubleshooting)

## What your PC sees

Once flashed, the shifter enumerates as a composite USB device called
**ShifterNoumi** (VID `0x0483`, PID `0x572C`) with two functions:

- a **gamepad** with 16 buttons (plus raw sensor values in vendor bytes that
  games ignore) — this is what you map in your sim;
- a **virtual COM port** that streams diagnostics. You can ignore it; it is
  only for troubleshooting (see [Testing and tools](testing-and-tools.md)).

No driver is needed on Windows 10/11.

## Button mapping

| Button | Meaning | Behaviour |
|---|---|---|
| 1–6 | Gears 1–6 (H-pattern) | Held while the lever is in the gate |
| 11 | Reverse (H-pattern) | Held while the lever is in the 1st gate, if the collar was lifted on entry |
| 7 | Shift up (sequential) | One 100 ms pulse per push |
| 8 | Shift down (sequential) | One 100 ms pulse per push |

Buttons 1–6 and 11 match the stock firmware, so existing sim profiles keep
working. Buttons 7 and 8 are new.

## H-pattern mode

This is the mode the shifter starts in the first time. After that, it
starts in whichever mode was active when it was unplugged: each mode switch
is saved.

The gear button turns on a few milliseconds after the lever settles in a
gate, and turns off about 15 ms after the lever leaves it. Neutral sends no
button. You can shift directly from one gate to another (e.g. 6 → 5); the
button switches without passing through "no button".

## Reverse

Lift the collar **before** moving the lever into the 1st-gear position. The
firmware checks the collar at the moment the gate is engaged:

- collar lifted → button **11** (reverse) for as long as the lever stays in
  that gate, even if you let go of the collar;
- collar down → button **1** (1st gear). Lifting the collar afterwards does
  not change it — go back to neutral and re-enter.

## Sequential mode

Lock the lever mechanically for sequential use, then
[switch the firmware to sequential](#switching-between-h-pattern-and-sequential).

Each push to the stop sends **one** 100 ms pulse: button 7 (up) or 8 (down).
The pulse fires as soon as the lever reaches the stop, not when you release
it. Holding the lever at the stop does not repeat the shift; let it come back
to centre before the next shift. Two quick pushes always produce two separate
pulses.

## Switching between H-pattern and sequential

The mechanical lock changes the lever travel, but nothing tells the firmware
which way the lever is locked, so you tell it with a gesture. Both gestures use
**collar blips**: lift the collar and release it, each lift lasting
roughly 0.1–0.7 s, the two blips no more than 1.2 s apart.

### To sequential

1. Lock the lever mechanically in sequential.
2. Make **two quick downshifts** (push to *shift −* and back, twice). Keep them
   short — a push that stays at the stop longer than about 0.3 s counts as
   "held" and is ignored by the gesture.
3. With the lever back at centre, **blip the collar twice**.
4. The LED double-flashes: you are in sequential mode.

Each step must follow the previous one within 5 s.

### To H-pattern

1. Unlock the lever back to H-pattern.
2. Put it in **6th** and keep it there.
3. **Blip the collar twice** while holding 6th.
4. The LED double-flashes: you are in H-pattern mode. Return to neutral before
   engaging a gear (the 6th you are holding is not sent after the switch).

If the target mode is already active, the gesture does nothing (and the LED
does not flash).

> While switching **to** H-pattern, the game sees 6th gear, because the
> firmware is still in H-pattern mode while you hold it. Do it with the car
> stopped or the clutch in.

## Calibration

Calibration teaches the firmware where each position is on *your* unit.
The firmware ships with defaults measured on the reference unit; calibrate
if gears are not recognised reliably, or after any mechanical work. It takes
about two minutes and is saved to flash, so it survives unplugging.

### Enter calibration mode

With the lever unlocked (H-pattern travel):

1. Lift the collar and **keep it lifted** for the whole sequence.
2. Move the lever through **1st → 5th → 6th → 2nd**, pausing about half a
   second in each.
3. On reaching 2nd, the LED starts *breathing* (a slow fade in and out):
   you are in calibration mode. After each breath it pulses the point it
   expects next (see below).

Releasing the collar at any point restarts the sequence. If the firmware is in
H-pattern mode, the game sees reverse (1st with the collar up), 5th and 6th
during this gesture; the final 2nd is never sent.

While calibration mode is active, **no buttons are sent** to the game.

### Record the nine points

Points are recorded in this fixed order:

| # | Position | Lever lock |
|---|---|---|
| 1 | 1st | H-pattern |
| 2 | 2nd | H-pattern |
| 3 | 3rd | H-pattern |
| 4 | 4th | H-pattern |
| 5 | 5th | H-pattern |
| 6 | 6th | H-pattern |
| 7 | Shift up (held at the stop) | Sequential |
| 8 | Shift down (held at the stop) | Sequential |
| 9 | Neutral | Either |

The LED tells you which point is next: one slow breath, then as many short
pulses as the number in the table (1 for 1st … 7 for shift up, 8 for shift
down), and a single long pulse for neutral. Then it pauses and repeats.

For each point:

1. Check the pulse count, put the lever in that position and keep it still.
2. Lift the collar and **hold it lifted for at least 2 seconds**. After 2 s
   the LED changes to a *double blip* pattern: the point is ready.
3. Release the collar. The point is recorded (average of the readings taken
   after the 2 s mark) and the LED flashes 3 times quickly.

Releasing before the double-blip cancels the attempt; just try again. After
point 9 (neutral), the LED flashes 3 times, the new calibration is saved,
calibration mode ends by itself and the LED goes off.

The firmware refuses a point that is obviously wrong: the lever still in
neutral (for points 1 to 8), on a position already recorded in this
session (you forgot to move the lever, or went back to a gear already
done), or shift up and shift down swapped. The LED then flickers fast for one second instead of
flashing 3 times, and shows the same point again. Move the lever to the
right position and record it again. It cannot tell two different gears
apart, so check the pulse count before each point.

> Nothing is saved until the ninth point: the new calibration is written to
> flash in one go at the end. If you make a mistake, **unplug the shifter**
> before finishing; the previous calibration is kept. Once all nine points
> are done, a wrong point can only be fixed by calibrating again — see
> [Troubleshooting](#troubleshooting).

## LED codes

The Blue Pill's on-board LED is off during normal driving.

| Pattern | Meaning |
|---|---|
| Off | Normal operation |
| Slow breath, then *n* pulses | Calibration mode, waiting for point *n* (1–6 = gears, 7 = shift up, 8 = shift down; one long pulse = neutral) |
| Double blip, repeating | Collar held long enough — release to record this point |
| 3 fast flashes | Point recorded, moving to the next one (after point 9: calibration saved) |
| Fast flicker for 1 s | Point refused (lever in neutral, or on a position already recorded): same point again |
| Double flash, then off | Mode switch applied (H-pattern ↔ sequential) |
| Fast blink, 5 Hz | Update mode (USB bootloader): waiting for `shifternoumi-setup`. Unplug and replug to leave it |
| On for up to 3 s at plug-in | You plugged in with the collar lifted: the bootloader is watching for the [recovery gesture](#updating-the-firmware) |

## Updating the firmware

Once the firmware has been installed with `shifternoumi-setup install` ([Installation](installation.md)) (which puts
a USB bootloader on the shifter), updates need only the USB cable: run
`shifternoumi-setup update`. Your calibration is kept. On Linux, run
`shifternoumi-setup udev --install` once first.

If an update is interrupted, the shifter stays in update mode (fast blinking
LED): run the update again. If the firmware ever misbehaves so badly that the
update cannot reach it, force update mode by hand: **lift the collar, plug
the USB cable in, release the collar and lift it again within 3 seconds.**

## Troubleshooting

**A gear is not recognised, or the wrong gear lights up.** Recalibrate. If one
position is still unreliable, capture a session with the debug port (see
[Testing and tools](testing-and-tools.md#capture_sessionpy)) and open an
issue with the files.

**Sequential shifts are ignored, or the lever gives gears 3/4 instead.** The
firmware is probably in H-pattern mode (it starts in the last mode used, H-pattern
the very first time). Do the [to-sequential gesture](#to-sequential).

**The mode-switch gesture does not take.** Keep each collar blip short (under
0.7 s) and the two blips close (under 1.2 s apart). For the to-H-pattern
gesture the lever must be sitting in 6th during both blips. For the
to-sequential gesture the two downshifts must be quick taps.

**I saved a calibration point on the wrong position.** If calibration is
still running, unplug the shifter: nothing is saved before the ninth point.
Otherwise run the whole calibration again. If calibration mode cannot be entered any more because
gears are no longer recognised, run `shifternoumi-setup reset-calibration` (menu
option 6): the shifter erases its calibration and restarts on the built-in
defaults, then calibrate again.

**Shifts register late, or not at all, and you end up breaking each shift
down.** This was a firmware bug, not your driving: firmware before the fast
path sent the gear 300–500 ms after the lever reached the gate, and dropped
some quick shifts entirely (direct 6→5, fast 4→3→4). The fast path fixes it
(13 ms median in Assetto Corsa, see [Design history](design-history.md)).
If you still see it with current firmware, record a session with
`capture_session.py` and open an issue with the files.

**The device is not detected.** Check the USB cable carries data, then check
Device Manager for a *GX100* entry. If nothing appears after flashing, see
[Building and flashing](building-and-flashing.md#troubleshooting).
