# USB interface

What the firmware exposes over USB: the device identity, the gamepad report,
and the diagnostic serial stream. Useful for mapping in games, writing tools,
or changing the descriptors.

## Device

| Field | Value | Where |
|---|---|---|
| VID | `0x0483` (STMicroelectronics) | `USB_DEVICE/App/usbd_desc.c` |
| PID | `0x572C` | same |
| Manufacturer / Product | `GX100 Project` / `GX100` | same |
| Device class | `0xEF / 0x02 / 0x01` (Miscellaneous, Interface Association) | same — required for the CDC pair to bind as one COM port |
| Speed | Full speed (12 Mbit/s) | |
| Serial number | derived from the MCU unique ID (ST default) | `usbd_desc.c` |

The VID belongs to ST and the PID was picked for this project. See
[Known issues](known-issues.md#usb-identity) before distributing binaries.

## Composite layout

The ST USB device library shipped for the F1 supports a single class per
device, and CubeMX offers no HID + CDC composite for the F1. So
`USB_DEVICE/App/usbd_composite.c` implements one class that dispatches to HID
or CDC logic by interface/endpoint. Both halves are functional copies of ST's
`usbd_hid.c` / `usbd_cdc.c`.

| Interface | Class | Endpoints | Purpose |
|---|---|---|---|
| IF0 | HID (0x03), no boot protocol | EP1 IN `0x81`, interrupt, 16 B, `bInterval = 1` ms | Gamepad |
| IF1 | CDC Communication (ACM) | EP3 IN `0x83`, interrupt, 8 B | CDC notifications (unused) |
| IF2 | CDC Data | EP2 IN `0x82` / OUT `0x02`, bulk, 64 B | Debug stream (IN). Host writes are ignored, except a packet starting with `!GX100:ENTER_BOOTLOADER` (reboots into the [USB bootloader](bootloader.md)) or `!GX100:RESET_CALIBRATION` (erases the calibration page and restarts on the built-in defaults) |

IF1 and IF2 are grouped by an Interface Association Descriptor. The
configuration descriptor is 100 bytes; the size breakdown is in
`usbd_composite.h`.

**Packet memory (PMA, 512 B)** allocation in `USB_DEVICE/Target/usbd_conf.c`:
EP0 OUT `0x18`, EP0 IN `0x58`, HID IN `0x100`, CDC IN `0x110`, CDC OUT
`0x150`, CDC CMD `0x190`.

The class state (HID + CDC) is a single static block sized by
`USBD_COMPOSITE_HANDLE_SIZE` (128 B) in `USBD_static_malloc`.

## HID report descriptor

Identical in structure to the stock GX100 firmware (so sim profiles made for
the stock device keep working):

```
Usage Page (Generic Desktop), Usage (Gamepad), Collection (Application)
  Usage Page (Button), Usage Min 1, Max 16, 1 bit × 16      → Input  (2 bytes)
  Usage Page (Undefined), Usage (Pointer), 8 bit × 14        → Input  (14 bytes)
  Usage (Vendor 0x20), 8 bit × 64                            → Output (64 bytes)
End Collection
```

The descriptor is 50 bytes. The copy served to the host is in
`usbd_composite.c`, and a duplicate sits in the ST middleware `usbd_hid.c`;
change both together.

The Output report is declared for compatibility only. There is no OUT
endpoint, and `SET_REPORT` is not handled.

## Input report (16 bytes)

| Byte | Bits | Content |
|---|---|---|
| 0 | 0–5 | Buttons 1–6: gears 1–6 (H-pattern), held while engaged |
| 0 | 6 | Button 7: shift up (sequential), 100 ms pulse |
| 0 | 7 | Button 8: shift down (sequential), 100 ms pulse |
| 1 | 2 | Button 11: reverse (1st with collar lifted on entry) |
| 1 | others | Unused (0) |
| 2–13 | | Raw Hall values U2..U7, `uint16` little-endian each (0–4095) |
| 14 | 0 | Debug: current mode (0 = H-pattern, 1 = sequential) |
| 14 | 1 | Debug: calibration mode active |
| 15 | | Unused (0) |

At most one of buttons 1–6/11 is set at a time. The raw values in bytes 2–13
mirror the stock firmware, which also exposed live sensor values there. Games
ignore them.

A report is submitted on every main-loop iteration (1 ms). If the previous
one has not been collected yet, that iteration's report is skipped and the
next one carries the current state.

## Debug stream (CDC)

Any serial terminal or `pyserial` can open the COM port; line settings are
ignored. The firmware sends a header line at startup, then **one CSV line
per slow step (100 ms)**, terminated by `\r\n`. A line is dropped if the
previous transfer has not finished, so gaps are possible.

| Column | Meaning |
|---|---|
| `t_ms` | Firmware time (`HAL_GetTick`, ms since boot) |
| `u2`…`u7` | Raw Hall values of this step |
| `collar` | Slow-debounced collar state (0/1) |
| `tracker_state` | `IDLE`, `PENDING`, `RISING`, `HELD` |
| `mode` | `H` or `SEQ` |
| `report_debug` | Byte 14 of the HID report |
| `emit_pos` | Position confirmed by the **slow** tracker this step (`gear1`…`seq_down`), empty if none |
| `emit_kind` | `SEQ`, `HELD`, `HELD_CORRECTED`, empty if none |
| `switch_target` | `H`/`SEQ` if a mode switch was applied this step |
| `gear6_active`, `gear6_blips` | To-H gesture detector state |
| `seqdown_window`, `seqdown_count`, `seqdown_blips` | To-SEQ gesture detector state |
| `calib_active`, `calib_step`, `calib_target` | Calibration mode state |
| `fast_pos` | Position engaged by the **fast** path at this step (`gear1`…`seq_down`), empty if none |
| `buttons` | Bytes 0–1 of the HID report sent at this step, as a decimal `uint16` (bit 0 = button 1) |

The line is written after the fast step of the same iteration, so `buttons`
is what the game received. Firmware older than Sept. 2026 sends lines
without the last two columns; `capture_session.py` accepts both.

In between, the firmware sends the active calibration on a line of its own,
every 2 s and right after a new calibration is applied (at the end of
calibration mode, once all nine points are recorded):

```
CAL,<t_ms>,<neutral u2…u7>,<gear1 u2…u7>,…,<seq_up u2…u7>,<seq_down u2…u7>
```

That is 54 raw values, in `gear_position_t` order. `capture_session.py`
writes each distinct calibration to `<session>_cal.csv`, and
`replay_fast_path` uses it (see [Testing and tools](testing-and-tools.md)).

Because the debug port is part of the same device, no special build is
needed to diagnose a unit in the field.

## Regenerating descriptors

CubeMX knows nothing about the composite class. See
[Building and flashing → Regenerating with CubeMX](building-and-flashing.md#regenerating-with-cubemx)
for the list of files it would overwrite.
