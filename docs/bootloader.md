# USB bootloader and shifternoumi-setup

How the firmware gets onto a shifter, and how it is updated afterwards
without opening the case.

> **Status (Oct 2026): validated on hardware** (one unit, one ST-Link V2
> clone, Windows). See [Hardware validation](#hardware-validation).

- A shifter needs an **ST-Link once**. The first install writes a small USB
  bootloader together with the firmware.
- Every **later update goes over the shifter's own USB cable**, with no
  driver to install on Windows (the bootloader is a HID device).
- Both are done with one program, **`shifternoumi-setup`** (Windows and Linux).

## Flash map

The contract lives in `Firmware/App/Inc/gx100_boot.h`, shared by the
bootloader, the app and the wizard (`Wizard/gx100wiz/layout.py` mirrors it;
a test fails if they differ).

| Address | Size | Content | Written by |
|---|---|---|---|
| `0x08000000` | 4 KB (pages 0–3) | Bootloader | ST-Link only; write-protected after install |
| `0x08001000` | up to 59,384 B | Application | Bootloader (USB update) or ST-Link |
| `0x0800F7F8` | 8 B | App trailer: length, CRC | Written **last** by an update |
| `0x0800F800` | 1 KB | Settings: the saved mode | The app only. Never erased by an update or an install |
| `0x0800FC00` | 1 KB | Calibration | The app only. Never erased by an update or an install |

The bootloader starts the app only if the trailer describes a complete image
whose CRC matches (STM32 CRC unit: CRC-32/MPEG-2 over 32-bit words), the
stack pointer is in RAM and the reset vector is inside the image. An update
first erases the app's first page and the trailer page, so from that moment
until the final COMMIT the app is invalid: **a cable pulled mid-update leaves
the shifter in update mode, never half-flashed.** Running the update again
recovers it.

The release build is 27.1 KB and the debug build 53.3 KB (end of Sept 2026);
both fit.

## Entering the bootloader

At every reset the bootloader runs first, at 8 MHz, touching nothing it does
not restore. It stays in **update mode** (LED blinking fast, 5 Hz) if:

1. **The app asked for it.** `shifternoumi-setup` sends
   `!GX100:ENTER_BOOTLOADER` on the shifter's serial port. The app
   (`boot_link.c`) writes `0x424C` to backup register `BKP_DR1` and resets.
   Backup registers survive a reset but not a power cycle, so **unplugging
   always returns to the app**.
2. **No valid app is installed** (blank, interrupted update, wrong image).
3. **Manual recovery: lift the collar, plug the USB cable in, release the
   collar and lift it again within 3 s.** The LED lights while the
   bootloader watches the collar. This is for an app that crashes before it
   can answer the wizard.

   Merely holding the collar lifted at power-up does *not* enter update
   mode: a disconnected collar wire also reads "lifted" (PB1 has a pull-up),
   and it must not trap the shifter in the bootloader.

Otherwise it jumps to the app. Boot delay in normal use: the CRC check, about
10 ms.

**USB re-enumeration.** The Blue Pill's D+ pull-up is hard-wired, so a reset
without unplugging is invisible to the PC. Both the bootloader and the app
(`boot_link_usb_reenumerate()`, called before the USB stack starts) hold D+
low for 20 ms at start-up, which the PC sees as unplug + replug.

## Protocol

USB identity: VID `0x0483`, PID `0x572D` (the app is `0x572C`), product
"ShifterNoumi Bootloader", serial = chip unique ID. One vendor HID interface with
64-byte interrupt IN and OUT reports. One response per request.

```
request:   [0] cmd  [1] seq  [2..3] 0   [4..] arguments
response:  [0] cmd  [1] seq  [2] status [3] 0  [4..] payload
```

| Cmd | Name | Arguments | Effect |
|---|---|---|---|
| `0x01` | INFO | — | Magic `GXBL`, protocol and bootloader version, app valid flag, layout, trailer length/CRC, chip UID, flash size |
| `0x02` | ERASE | — | Erases the app's first page and the trailer page; opens an update session |
| `0x03` | WRITE | offset (u32), length (u8, ≤48, ×4), data | Erases each page the first time it is touched, programs, reads back |
| `0x04` | COMMIT | length, CRC | Checks the CRC over the written image, writes the trailer, checks the vector table |
| `0x05` | BOOT | — | Answers, then resets into the app |

The bootloader refuses any write outside `[0x08001000, 0x0800F7F8)`: it
cannot overwrite itself, the saved mode or the calibration. Status codes and exact offsets:
`gx100_boot.h`. A 27 KB update is about 570 WRITE round trips, a few
seconds.

## shifternoumi-setup

```
shifternoumi-setup                 menu
shifternoumi-setup install         first install through an ST-Link
shifternoumi-setup install --check connect through the ST-Link, show the chip, write nothing
shifternoumi-setup update          update over USB
shifternoumi-setup check           live view: gears the PC receives, as you move the lever
shifternoumi-setup reset-calibration  back to the built-in calibration (over USB)
shifternoumi-setup info            what is connected (ST-Link, chip, shifter) and installed
shifternoumi-setup udev --install  Linux, once: USB permissions for the shifter and the ST-Link
```

**install** (ST-Link, via OpenOCD): shows the wiring, connects (retrying at
100 kHz), reads the chip state, and if the stock firmware's read protection is
on, asks you to type `ERASE` before removing it. Then it lifts the
bootloader's write protection if set, erases the whole flash if the
protection was on (removing it did not erase the flash on the reference
unit with OpenOCD, Oct 2026), writes `gx100-full.hex`
(bootloader + app + trailer, erasing only the pages it covers), verifies,
write-protects the bootloader and waits for the shifter on USB for the live
check. `install --standalone` writes the app alone at `0x08000000`, with no
bootloader.

**update**: finds the shifter, asks it to reboot into the bootloader, writes
`gx100-app.bin`, restarts it and runs the live check. If the same firmware is
already installed it only restarts (`--force` rewrites). It refuses a
standalone-built image before erasing anything.

The Windows and Linux executables are single files with the firmware and
[xPack OpenOCD](https://github.com/xpack-dev-tools/openocd-xpack) inside.
OpenOCD is started with `set CPUTAPID 0` so clone chips are accepted, and
with its gdb/tcl/telnet servers off (no Windows firewall prompt). `info` and
`install --check` connect without resetting or halting the chip, so a
running shifter keeps running. If OpenOCD reports an error during the
read-protection removal, the chip's state is read again before giving up:
the erase often went through anyway.

**Windows driver.** The shifter and its bootloader need no driver. The
ST-Link does: ST's driver (STSW-LINK009) or WinUSB installed with Zadig. The
wizard detects the missing driver and says so; installing it automatically is
[not done yet](known-issues.md).

**Linux.** `shifternoumi-setup udev --install` writes `/etc/udev/rules.d/60-gx100.rules`
(access for the logged-in user to the shifter, its bootloader and ST-Links,
and tells ModemManager to leave the shifter's serial port alone).

## Building and testing

```sh
make -C Bootloader                  # build/gx100-bootloader.bin, must fit in 4 KB (3.4 KB today)
make -C Bootloader test              # protocol host tests + the simulator used below
make -C Firmware                    # app for the bootloader: build/release-bootloader/
make -C Firmware LAYOUT=standalone  # app at 0x08000000, no bootloader (ST-Link only)
cd Wizard && python -m unittest discover -s tests -t .
```

The wizard tests run the wizard's real update code against the bootloader's
real protocol code: `Bootloader/test/boot_proto_host` wraps `boot_proto.c`
around a simulated flash kept in a file, behind stdin/stdout. They cover a
full update, a power cut half-way then a retry, an image built for the wrong
layout, and the actual release firmware.

Release files (CI does this on every push; by hand from `Wizard/`):

```sh
python -m gx100wiz.image package --boot ../Bootloader/build/gx100-bootloader.bin \
    --app ../Firmware/build/release-bootloader/gx100-app.bin \
    --standalone ../Firmware/build/release-standalone/gx100-app.bin \
    --version v0.1.0 --out ../dist
python build_exe.py --firmware ../dist --version v0.1.0 --out ../dist-exe
```

| File | Use |
|---|---|
| `gx100-full.hex` | First install: bootloader + app + trailer |
| `gx100-app.bin` | USB updates |
| `gx100-bootloader.bin` | The bootloader alone |
| `gx100-standalone.hex` | App without bootloader, at `0x08000000` |
| `manifest.json` | Version, app length and CRC |
| `shifternoumi-setup-<version>-windows.exe`, `-linux` | The wizard, with all of the above inside |

## Hardware validation

Run on the reference unit in October 2026, with the CI builds, one change at
a time (the ST-Link can recover any step):

- **Bootloader + app** (`gx100-full.hex` through the ST-Link): starts
  normally.
- **Update over USB**: the app reboots into the bootloader, the new app is
  written and verified, the shifter restarts; calibration and saved mode
  kept, value for value.
- **Interrupted update**: USB cable pulled at 79 %, and a real power cut at
  79 % (ST-Link unplugged): the bootloader keeps control (app invalid), and
  the next `update` recovers.
- **Collar gesture**: enters update mode with a valid app; a collar merely
  held at plug-in starts the app normally.
- **`install` through the ST-Link**: unlocks the bootloader pages, writes,
  verifies, protects them again; calibration kept.
- **From a read-protected chip** (protection set again with
  STM32CubeProgrammer): the chip is identified, `ERASE` is required (a
  lowercase `erase` changes nothing), the protection is removed, the whole
  flash erased, and the shifter starts on the built-in calibration in
  H-pattern. This run found two installer bugs, both fixed: the flash size
  cannot be read while the chip is protected, and removing the protection
  with OpenOCD did not erase the flash on this unit.

Not covered: a PC without ST's tools installed (only ST's driver), Linux,
other ST-Links, other chips.
