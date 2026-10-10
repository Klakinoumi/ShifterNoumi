# Installation

How to put this firmware on your GX100, from the box to your first race. It
takes about 20 minutes, and you do it only once: later updates use the USB
cable alone (see [Updating](#updating)).

> **Point of no return.** The GX100's stock firmware is locked. Installing
> this one **erases it for good**: nobody can back it up or put it back.
> The shifter will then only run this firmware.

> The installer has only been tested on a PC so far, never end to end on
> a shifter. Until it has, keep STM32CubeProgrammer at hand
> ([Building and flashing](building-and-flashing.md)) in case a step fails.

## What you need

| Item | Notes |
|---|---|
| ST-Link V2 programmer | A cheap clone (USB stick, ~5 €) works. Only needed for this first install |
| 4 female-to-female jumper wires | Usually sold with the ST-Link |
| A small screwdriver | To open the shifter's base |
| A USB-C **data** cable | Some cables only charge. If the PC never sees the shifter, try another one |
| A PC with Windows 10/11 or Linux | macOS is not supported |

## 1. Download the installer

From the project's [Releases page](https://github.com/Klakinoumi/ShifterNoumi/releases),
download the latest **shifternoumi-setup** for your system:

- Windows: `shifternoumi-setup-…-windows.exe`
- Linux: `shifternoumi-setup-…-linux`. Make it executable (`chmod +x`), then
  run `./shifternoumi-setup-…-linux udev --install` once. It asks for your password
  and gives your user access to the shifter and the ST-Link.

The installer is a single file with the firmware built in. You don't need to
install anything else on Linux.

> **No release has been published yet.** Until there is one, the installer
> is only available as a test build: on the project's
> [Actions page](https://github.com/Klakinoumi/ShifterNoumi/actions), open
> the latest green *build* run and download `shifternoumi-setup-Windows` or
> `shifternoumi-setup-Linux` under *Artifacts* (a zip with the executable inside;
> you must be signed in to GitHub).

## 2. Windows only: the ST-Link driver

Windows doesn't include an ST-Link driver. Install
[ST's ST-Link driver (STSW-LINK009)](https://www.st.com/en/development-tools/stsw-link009.html),
then plug the ST-Link in. If you skip this step, the installer stops with
*"Windows has no driver for it"*.

The shifter itself needs no driver.

## 3. Wire the ST-Link to the shifter

1. **Unplug the shifter's USB cable.** During the install, the ST-Link
   powers the board.

   > **Power the board from one source only.** Never have the shifter's
   > USB cable plugged in while the ST-Link's 3.3V wire is on the board:
   > two supplies would drive the same 3.3V line, and an ST-Link unplugged
   > from the PC but still wired would be fed backwards by the board.
   > Either can be damaged. Unplugging the ST-Link from the PC is not
   > enough: remove its wires from the board. The installer refuses to
   > start while the shifter is plugged in over USB.
2. Open the base and find the **Blue Pill**: the small blue board with the
   USB-C socket, plugged onto the black main board.
   <!-- TODO: photo of the opened base, screws to remove -->
3. At the end of the Blue Pill, opposite the USB-C socket, there are 4 pins
   marked `3V3`, `IO` (or `DIO`), `CLK` (or `DCLK`) and `GND`. Connect them to
   the ST-Link pins that carry the same names:

   | ST-Link | Blue Pill |
   |---|---|
   | 3.3V | 3V3 |
   | SWDIO | IO / DIO |
   | SWCLK | CLK / DCLK |
   | GND | GND |

   Don't use the ST-Link's 5V pin. The pin order printed on ST-Link clones
   differs from one model to another, so go by the labels, not the
   positions.
   <!-- TODO: photo of the 4 wires on the SWD header -->

## 4. Run the installer

Plug the ST-Link into the PC, then start shifternoumi-setup (double-click on
Windows). To check the wiring first, choose **5 Check the ST-Link
connection**: it shows the ST-Link, the board voltage and what the chip
contains, and writes nothing. Then choose **1 First install**.

If the shifter is still plugged in over USB when you start, the installer
first says which firmware it runs (stock or community: the chip itself
cannot tell once read-protected), then asks you to unplug it before wiring
the ST-Link.

The installer then:

1. **Shows the wiring** and waits for Enter.
2. **Connects to the chip**, and reads the board voltage (about 3.3 V) and
   the flash size (64 KB). If the first attempt fails, it retries at a lower
   speed, which is normal with long wires.
3. **Asks you to type `ERASE`.** This is the point of no return described
   at the top. Type it in capitals and press Enter. The installer removes
   the protection and erases the whole chip, which takes a few seconds.
4. **Writes the firmware and checks it.** It also locks the small USB
   updater (the bootloader) so that no later update can damage it.
5. **Asks you to swap cables.** Remove the ST-Link wires from the Blue
   Pill (at least the 3.3V wire), close the base, and only then plug the
   shifter's USB cable into the PC.
6. **Live check.** Move the lever through 1, 2, 3, 4, 5, 6, then lift the
   collar and engage reverse (in sequential mode: push up, then down). Each
   position appears as you reach it:

   ```
     H-pattern   now: 3         seen: 1 2 3 . . . .
   ```

   When every position has been seen, the installer says *All positions
   detected* and you are done.

If a position never shows up, don't worry yet: the firmware starts with a
default calibration measured on another shifter, and the next step fixes
that.

## 5. Calibrate

Your shifter's magnet and sensors differ slightly from the reference unit,
so calibrate once. It takes about two minutes, all on the lever, with no PC
tool. Follow [User guide → Calibration](user-guide.md#calibration). The LED
shows you which position it expects next.

Then run shifternoumi-setup again, choose **3 Check the shifter**, and make sure
all seven positions are detected.

## 6. Map the buttons in your game

The shifter appears as a gamepad called **ShifterNoumi**:

| Button | Function |
|---|---|
| 1–6 | Gears 1–6 |
| 11 | Reverse |
| 7 / 8 | Sequential shift up / down |

Buttons 1–6 and 11 are the same as the stock firmware's, so existing
profiles keep working. For sequential driving, lock the lever and switch
modes with the
[gesture](user-guide.md#switching-between-h-pattern-and-sequential). The
shifter remembers the last mode you used.

## Updating

Download the new shifternoumi-setup, plug the shifter in, and choose **2 Update
over USB**. You don't need the ST-Link or to open the base, and your
calibration and mode are kept. The shifter restarts into update mode (fast
blinking LED), the new firmware is written and checked, and the live check
runs again.

If an update is interrupted (cable pulled, PC crash), the shifter stays in
update mode. Run the update again. If the shifter doesn't respond at all,
force update mode by hand: **lift the collar, plug the USB cable in,
release the collar and lift it again within 3 seconds**. The LED blinks
fast. Then run the update.

## Troubleshooting

The installer stops with a message saying what went wrong. The most common
ones:

| Message | What to do |
|---|---|
| *No ST-Link found* | Plug the ST-Link in, or try another USB port |
| *The ST-Link is in use by another program* | Close STM32CubeProgrammer or STM32CubeIDE |
| *Windows has no driver for it* | Install the driver ([step 2](#2-windows-only-the-st-link-driver)) |
| *No permission to use the ST-Link* (Linux) | Run `shifternoumi-setup udev --install`, then unplug and replug the ST-Link |
| *The ST-Link reads 0.0 V on the board* | The 3.3V wire is missing or on the wrong pin |
| *The ST-Link cannot talk to the chip* | SWDIO and SWCLK are probably swapped. Also check GND |
| *The protection is still on* | Unplug the ST-Link from the PC for a few seconds, then run the install again |
| *The shifter did not appear on USB* | Use another USB cable (the one you have may only charge) or another port |
| *The shifter … has no USB bootloader* | It was installed without one. Run the first install once more with the ST-Link |

If the ST-Link still can't connect, the chip may need to be held in reset.
STM32CubeProgrammer can do this, and the installer can't yet: follow
[Building and flashing → Remove the read protection](building-and-flashing.md#1-remove-the-read-protection-one-time)
with *Mode: Under reset*, then run the installer again.

For anything else, see the [User guide troubleshooting](user-guide.md#troubleshooting)
or open an issue on the project page.
