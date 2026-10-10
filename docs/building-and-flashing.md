# Building and flashing

From a fresh clone to a flashed shifter. Takes about 30 minutes the first
time.

> **Point of no return.** The stock GX100 firmware is read-protected (RDP
> level 1). Removing the protection, which you must do to flash anything,
> **mass-erases the chip**. The stock firmware cannot be backed up or restored
> afterwards.

## What you need

| Item | Notes |
|---|---|
| ST-Link V2 (original or clone) | SWD programmer |
| 4 jumper wires | 3.3 V, SWDIO, SWCLK, GND to the Blue Pill's 4-pin SWD header |
| [STM32CubeIDE](https://www.st.com/en/development-tools/stm32cubeide.html) | Build + flash. Tested with 2.2.0 |
| [STM32CubeProgrammer](https://www.st.com/en/development-tools/stm32cubeprog.html) | One-time read-protection removal |
| [STM32CubeMX](https://www.st.com/en/development-tools/stm32cubemx.html) (optional) | Only to change the peripheral configuration (`.ioc`). Since CubeIDE 2.0 the `.ioc` editor is a separate tool. Project generated with CubeMX 6.18.1, firmware package STM32Cube FW_F1 V1.8.7 |

## 1. Remove the read protection (one time)

1. Wire the ST-Link to the SWD header: `3V3`, `SWDIO`, `SWCLK`, `GND`, with
   the shifter's USB cable unplugged: the ST-Link powers the board. Never
   have both connected at once (two supplies on the same 3.3V line); remove
   the ST-Link wires before plugging USB back in.
2. Open STM32CubeProgrammer, select *ST-LINK*, click *Connect*. If it fails,
   set *Mode* to *Under reset* and retry, holding the Blue Pill's reset button
   while clicking *Connect*.
3. Go to *Option bytes (OB)* → *Read Out Protection* → set **RDP = AA
   (Level 0)** → *Apply*. The chip mass-erases itself. This is expected.
4. Disconnect.

Skipping this step makes the first flash from CubeIDE fail with *failed to
erase memory*.

## 2. Build

1. In STM32CubeIDE: *File → Import → General → Existing Projects into
   Workspace*, select the `Firmware/` folder (project `Gx100_Firmware`).
2. Select the **Debug** build configuration and build (*Project → Build*,
   hammer icon). The output is `Firmware/Debug/Gx100_Firmware.elf`.

If the build fails with `usbd_cdc.h: No such file or directory`, the CDC
include path is missing from the configuration you are building (both Debug
and Release have it since Sept. 2026; a CubeMX regeneration may drop it).
Add it under *Project → Properties → C/C++ Build → Settings → MCU GCC
Compiler → Include paths*:

```
../Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Inc
```

### Command-line build (no IDE)

`Firmware/Makefile` builds the same sources with the same flags, into
`Firmware/build/` (CubeIDE's `Debug/` is left alone):

```
cd Firmware
make                                  # release (-Os), for the USB bootloader
                                      #   -> build/release-bootloader/gx100-app.{elf,bin,hex}
make LAYOUT=standalone                # linked at 0x08000000, like CubeIDE: flash with an ST-Link
make CONFIG=debug LAYOUT=standalone   # CubeIDE's Debug flags
```

It needs `arm-none-eabi-gcc` and `make` on `PATH`. The ones bundled with
CubeIDE work, under `STM32CubeIDE/plugins/*gnu-tools-for-stm32*/tools/bin`
and `*externaltools.make*/tools/bin`. Before the bootloader work,
`CONFIG=debug LAYOUT=standalone` was checked byte-identical to CubeIDE's Debug
build. The release build is half the size (27 KB vs 53 KB).

`LAYOUT=bootloader` output only runs behind the USB bootloader; see
[USB bootloader and shifternoumi-setup](bootloader.md) for the bootloader itself,
the release files and the installer.

GitHub Actions (`.github/workflows/build.yml`) runs every host test, builds
the bootloader and the firmware, packages the release files and builds
shifternoumi-setup for Windows and Linux on every push. Pushing a tag `vX.Y.Z`
creates a draft Release with all of it attached.

## 3. Flash

With the ST-Link connected: *Run → Run* (or the launch configuration
`Gx100_Firmware.launch`). CubeIDE builds, downloads and resets the board.

Then plug the shifter's USB-C into the PC. Device Manager should show:

- *Human Interface Devices* → a HID game controller, and *GX100* in
  *Set up USB game controllers* (`joy.cpl`);
- *Ports (COM & LPT)* → a *USB Serial Device (COMx)* — the debug stream.

## 4. First run

The firmware starts in H-pattern mode with the built-in default calibration
(later, it starts in the last mode used).
Check gears in `joy.cpl` (buttons 1–6, 11 with the collar), then
[calibrate](user-guide.md#calibration) on the lever.

Flashing a new firmware version does **not** erase your calibration or the
saved mode; they live in the last two flash pages, which the program does not
use. To go back to the default calibration, run `shifternoumi-setup
reset-calibration`, or erase the last page with STM32CubeProgrammer
(*Erasing & programming* → erase sector at `0x0800FC00`); erasing the one at
`0x0800F800` forgets the saved mode.

## Regenerating with CubeMX

CubeMX regenerates `Core/`, `USB_DEVICE/` and `Middlewares/` from
`Gx100_Firmware.ioc`. The `.ioc` still describes a **plain HID** device, so
regeneration undoes the composite HID + CDC device. Everything under `App/`
is safe.

These files carry hand edits (marked `GX100` in comments) that must be
re-applied after any regeneration:

| File | Edit | Inside a `USER CODE` block? |
|---|---|---|
| `USB_DEVICE/App/usb_device.c` | Registers `USBD_Composite` instead of `USBD_HID` | **No — silently lost** |
| `USB_DEVICE/App/usbd_desc.c` | PID, strings, device class `0xEF/0x02/0x01` | No |
| `USB_DEVICE/App/usb_device.h` | `extern hUsbDeviceFS` | Yes |
| `USB_DEVICE/Target/usbd_conf.c` | PMA layout for the CDC endpoints; `USBD_static_malloc` sized for the composite state | No |
| `USB_DEVICE/Target/usbd_conf.h` | `HID_FS_BINTERVAL` = 1 | No |
| `Middlewares/.../HID/Inc/usbd_hid.h`, `Src/usbd_hid.c` | 16-byte endpoint, custom report descriptor | No |
| `Middlewares/.../CDC/Inc/usbd_cdc.h` | Endpoints moved to EP2/EP3 | No |
| `Core/Src/gpio.c` | LED off at init | Yes |
| `Core/Src/main.c` | Whole application loop | Yes |

`USB_DEVICE/App/usbd_composite.[ch]` and `usbd_cdc_if.[ch]` are not generated
by CubeMX and are never touched.

Safest workflow: regenerate, then compare with git (`git diff`) and restore
the hunks marked `GX100`.

The `.ioc` also still routes PB3 to SWO trace, a leftover from an abandoned
debug approach. It is harmless (PB3 is not used).

## The diagnostic firmware

`Diag/Gx100_DiagFirm/` is a separate, minimal CubeIDE project used to map the
sensors before the real firmware existed. It exposes only a CDC port that
prints `T <ms> HALL <u2> … <u7> COLLAR <0|1>` every 100 ms (read by
`tools/hall_diag_stream.py`). Use it to inspect a unit that behaves
very differently from the reference one. Build and flash it the same way.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `failed to erase memory` on first flash | Read protection still on — step 1 |
| CubeProgrammer cannot connect | Use *Under reset* mode, check wiring, try a lower SWD frequency |
| `usbd_cdc.h: No such file or directory` | Missing include path — see [Build](#2-build) |
| Device shows up but no COM port | Wrong device class: the `usbd_desc.c` edit was lost by a regeneration |
| Device appears as a plain gamepad with no COM port and buttons behave oddly | `usb_device.c` registers `USBD_HID` again after a regeneration |
| Nothing enumerates | Charge-only USB cable, or a crash in `Error_Handler` at boot (debug with the ST-Link) |
