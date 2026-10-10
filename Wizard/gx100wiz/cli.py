"""shifternoumi-setup: install, update and check ShifterNoumi, the open
firmware for the GX100 shifter.

    shifternoumi-setup                 interactive menu
    shifternoumi-setup install         first install through an ST-Link
    shifternoumi-setup install --check connect through the ST-Link, write nothing
    shifternoumi-setup update          update over the shifter's USB cable
    shifternoumi-setup check           live view of what the shifter sends
    shifternoumi-setup reset-calibration  back to the built-in calibration
    shifternoumi-setup info            what is connected, and what is installed
    shifternoumi-setup udev [--install]  Linux: USB permissions (once)
"""
import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

from . import __version__, device, image, layout, live
from .bootloader import BootloaderClient, BootloaderError, HidTransport
from . import swd
from .swd import OpenOcd, SwdError

WIRING = """
  ST-Link          Blue Pill SWD header (4 pins, end of the board)
  -------          ------------------------------------------------
  3.3V     ----->  3V3
  SWDIO    ----->  IO   (DIO)
  SWCLK    ----->  CLK  (DCLK)
  GND      ----->  GND

  !! Power the board from ONE source only. Keep the shifter's USB cable
  !! unplugged while the ST-Link is wired to the board, and remove the
  !! ST-Link wires (at least 3.3V) before plugging the USB cable back in.
"""

ONE_SOURCE = ("Never power the board from the USB cable and the ST-Link's 3.3V at the same "
              "time: two supplies on the same 3.3V line, or the board feeding an unplugged "
              "ST-Link backwards, can damage either of them.")

UDEV_RULES = """\
# GX100 shifter: USB access for the logged-in user (installed by shifternoumi-setup)
# Application (gamepad + serial port) and bootloader
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="0483", ATTRS{idProduct}=="572c", TAG+="uaccess"
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="0483", ATTRS{idProduct}=="572d", TAG+="uaccess"
SUBSYSTEM=="usb", ATTRS{idVendor}=="0483", ATTRS{idProduct}=="572c", TAG+="uaccess"
SUBSYSTEM=="usb", ATTRS{idVendor}=="0483", ATTRS{idProduct}=="572d", TAG+="uaccess"
SUBSYSTEM=="tty", ATTRS{idVendor}=="0483", ATTRS{idProduct}=="572c", TAG+="uaccess", ENV{ID_MM_DEVICE_IGNORE}="1"
# ST-Link V2, V2-1, V3 (first install)
SUBSYSTEM=="usb", ATTRS{idVendor}=="0483", ATTRS{idProduct}=="3748", TAG+="uaccess"
SUBSYSTEM=="usb", ATTRS{idVendor}=="0483", ATTRS{idProduct}=="374b", TAG+="uaccess"
SUBSYSTEM=="usb", ATTRS{idVendor}=="0483", ATTRS{idProduct}=="374e", TAG+="uaccess"
SUBSYSTEM=="usb", ATTRS{idVendor}=="0483", ATTRS{idProduct}=="374f", TAG+="uaccess"
SUBSYSTEM=="usb", ATTRS{idVendor}=="0483", ATTRS{idProduct}=="3753", TAG+="uaccess"
"""
UDEV_PATH = "/etc/udev/rules.d/60-gx100.rules"


class Abort(Exception):
    pass


def say(text=""):
    print(text, flush=True)


def ask(prompt: str) -> str:
    try:
        return input(prompt)
    except EOFError:
        return ""


def your_turn(text: str) -> None:
    """Something the user has to do now: stands out from the progress lines."""
    say("")
    say("   >>> YOUR TURN: " + text)


def waiting(what: str, ctrl_c: str = "stop") -> None:
    say(f"   ... waiting for {what} (Ctrl+C to {ctrl_c})")


WELCOME = """
   ************************************************************************
     Welcome in the future. This is how this shit should have shipped
     from factory. From Klakinoumi with <3 to the Hackers and DIY Sim
     Community. Enjoy !
   ************************************************************************"""


def confirm(prompt: str, assume_yes: bool) -> bool:
    if assume_yes:
        return True
    return ask(f"{prompt} [y/N] ").strip().lower() in ("y", "yes", "o", "oui")


def progress_bar(done: int, total: int) -> None:
    width = 30
    filled = width * done // total
    print(f"\r  Writing  [{'#' * filled}{'.' * (width - filled)}] {100 * done // total:3d}%",
          end="" if done < total else "\n", flush=True)


# ---- Firmware files

def firmware_dir(arg) -> Path:
    candidates = []
    if arg:
        candidates.append(Path(arg))
    if getattr(sys, "frozen", False):
        candidates.append(Path(getattr(sys, "_MEIPASS", "")) / "firmware")
        candidates.append(Path(sys.executable).parent / "firmware")
    repo = Path(__file__).resolve().parents[2]
    candidates.append(repo / "dist")
    for c in candidates:
        if (c / "manifest.json").is_file():
            return c
    raise Abort("Firmware files not found. Build them (see docs/building-and-flashing.md) "
                "or pass --firmware DIR (a folder with manifest.json).")


def firmware_version(fw: Path) -> str:
    return json.loads((fw / "manifest.json").read_text(encoding="utf-8")).get("version", "?")


# ---- install (ST-Link)

SEEN_ON_USB = {"community": "ShifterNoumi (seen on USB)",
               "stock": "the stock firmware (seen on USB)"}


def describe_chip(state, seen=None) -> list:
    """What the ST-Link sees, in words. seen: device.firmware_on_usb(),
    the only way to know what a read-protected chip holds."""
    chip = "STM32F103" if state.dev_id == 0x410 else f"chip ID 0x{state.dev_id:03X} (clone)"
    if state.read_protected:
        content = "read-protected, holding " + SEEN_ON_USB.get(
            seen, "unknown firmware (normally the stock one; plug the shifter in over USB first to identify it)")
    elif state.bootloader_write_protected:
        content = "ShifterNoumi with USB bootloader"
    else:
        content = "unprotected (ShifterNoumi without bootloader, or empty)"
    return [f"ST-Link:  {state.adapter}, board voltage {state.voltage:.2f} V",
            f"Chip:     {chip}, flash "
            + (f"{state.flash_kb} KB" if state.flash_kb is not None else "size hidden by the read protection"),
            f"Contents: {content}"]


def check_flash_size(state) -> None:
    """Unknown (read-protected chip): checked again after the erase."""
    if state.flash_kb is not None and state.flash_kb < 64:
        raise Abort(f"This chip reports {state.flash_kb} KB of flash, the firmware needs 64 KB.")


def connect(ocd: OpenOcd, halt: bool = True):
    """Reads the chip's state, retrying at low speed if the chip does not
    answer (long or poor wires)."""
    try:
        return ocd.chip_state(halt=halt)
    except SwdError as e:
        if e.kind not in (swd.NO_TARGET, swd.OTHER):
            raise
        say("   First attempt failed, retrying at low speed...")
        ocd.speed_khz = 100
        return ocd.chip_state(halt=halt)


def cmd_install(args) -> None:
    fw = firmware_dir(args.firmware)
    if args.hex:
        hex_path, with_bootloader = Path(args.hex), False
    elif args.standalone:
        hex_path, with_bootloader = fw / "gx100-standalone.hex", False
    else:
        hex_path, with_bootloader = fw / "gx100-full.hex", True
    if not hex_path.is_file():
        raise Abort(f"{hex_path} not found")

    if args.check:
        say("Checking the ST-Link connection (nothing is written)")
    else:
        say(f"First install of ShifterNoumi {firmware_version(fw)} ({hex_path.name})")
    # Identify the firmware while the shifter may still be on USB: once
    # read-protected, the chip cannot tell through the ST-Link.
    seen = device.firmware_on_usb()
    if seen:
        say(f"\n   The shifter is plugged in over USB, running {SEEN_ON_USB[seen][:-len(' (seen on USB)')]}.")
        say(f"   {ONE_SOURCE}")
        your_turn("unplug the shifter's USB cable before wiring the ST-Link.")
        waiting("the USB cable to be unplugged")
        if not device.wait_for(lambda: not device.firmware_on_usb(), 120):
            raise Abort("The shifter is still plugged in over USB. Unplug it, then run this again.")
        say("   OK, USB cable unplugged.")
    say("\n1. Connect the ST-Link to the shifter's Blue Pill:")
    say(WIRING)
    your_turn("wire the ST-Link to the Blue Pill as shown above and plug the ST-Link\n"
              "   into the PC (the shifter's USB cable stays unplugged).")
    ask("   Press Enter when done... ")
    if device.firmware_on_usb():
        raise Abort("The shifter is plugged in over USB. " + ONE_SOURCE + " Unplug the USB cable "
                    "and run this again.")

    ocd = OpenOcd()
    say("\n2. Connecting to the chip...")
    state = connect(ocd, halt=not args.check)
    for line in describe_chip(state, seen):
        say(f"   {line}")
    check_flash_size(state)
    if state.dev_id != 0x410:
        say(f"   Note: chip ID 0x{state.dev_id:03X} is not an STM32F103 C8 (0x410). A clone chip; continuing.")
    if args.check:
        if state.read_protected:
            plan = "ask you to type ERASE, erase the stock firmware, then write"
        else:
            plan = "write"
        say(f"\n   Connection OK. 'install' would {plan} ShifterNoumi {firmware_version(fw)}"
            + ("." if state.read_protected else "; your calibration would be kept."))
        return

    was_protected = state.read_protected
    if state.read_protected:
        if seen == "community":
            say("\n   The chip is read-protected, but it holds ShifterNoumi (seen on USB):")
            say("   nothing of the stock firmware is left. Removing the protection ERASES")
            say("   THE CHIP, including your calibration (you will calibrate again).")
        else:
            say("\n   The chip is read-protected: "
                + ("it holds the stock firmware (seen on USB)." if seen == "stock"
                   else "normally, it still has the stock firmware."))
            say("   Removing the protection ERASES WHATEVER IS ON THE CHIP, FOR GOOD.")
            say("   The stock firmware cannot be saved or restored: the shifter will only run")
            say("   this firmware.")
        your_turn("type ERASE (in capitals) to go on, anything else to stop.")
        if not args.yes and ask("   ERASE? ").strip() != "ERASE":
            raise Abort("Stopped, nothing was changed.")
        say("   Removing read protection...")
        try:
            state = ocd.remove_read_protection()
        except SwdError as e:
            raise Abort("The chip did not answer after the erase. Unplug the ST-Link from the PC "
                        "for a few seconds, plug it back and run install again: it carries on "
                        f"from where it stopped.\n({e})") from e
        if state.read_protected:
            raise Abort("The protection is still on. Unplug the ST-Link from the PC for a few "
                        "seconds (power cycle), then run install again.")
        say("   Done.")
        check_flash_size(state)  # readable now that the protection is off

    if state.bootloader_write_protected:
        say("   Unlocking the bootloader pages for rewriting...")
        ocd.set_bootloader_write_protection(False)

    if was_protected:
        # Removing the read protection does not erase the flash on every
        # chip (it did not on the reference unit, Oct 2026): erase it all,
        # so nothing of the stock firmware stays readable.
        say("   Erasing the whole chip (what is left of the stock firmware)...")
        ocd.mass_erase()

    say("\n3. Writing and verifying the firmware...")
    ocd.program(hex_path)
    if with_bootloader:
        say("   Protecting the bootloader against accidental overwrite...")
        ocd.set_bootloader_write_protection(True)
    if was_protected:
        say("   Firmware written and verified. The chip was erased: the shifter starts on the")
        say("   built-in calibration. Calibrate it once (docs/user-guide.md#calibration).")
    else:
        say("   Firmware written and verified. Your calibration (if any) was kept.")

    say("\n4. Swap the cables.")
    your_turn("remove the ST-Link wires from the shifter (at least the 3.3V wire:\n"
              "   unplugging the ST-Link from the PC is not enough), then plug the\n"
              "   shifter's USB cable into the PC.")
    waiting("the shifter on USB (up to 2 minutes)")
    if not device.wait_for(device.app_hids, 120):
        raise Abort("The shifter did not appear on USB within 2 minutes. Try another cable "
                    "(some are charge-only) or another USB port.")
    say("   OK, shifter detected.")
    live_check()
    say(WELCOME)


# ---- update (USB)

def enter_bootloader(timeout_s: float = 60) -> bytes:
    """Returns the bootloader's HID path, rebooting the app into it if needed."""
    if device.bootloaders():
        return device.open_bootloader_path()
    if not device.app_serial_ports():
        your_turn("plug the shifter's USB cable into the PC.")
        waiting("the shifter on USB")
        if not device.wait_for(device.app_serial_ports, timeout_s):
            raise Abort("No shifter found. Check the cable (some are charge-only).")
    ports = device.app_serial_ports()
    if len(ports) > 1:
        say(f"   Several shifters found ({', '.join(ports)}); using {ports[0]}. Unplug the others to be sure.")
    say(f"   Restarting the shifter in update mode ({ports[0]})...")
    device.request_bootloader(ports[0])
    found = device.wait_for(device.bootloaders, 10)
    if not found:
        if device.app_hids():
            raise Abort("The shifter restarted but has no USB bootloader: it was flashed without "
                        "one. Run 'install' once with an ST-Link.")
        raise Abort("The shifter did not come back in update mode. Unplug and replug it, then retry."
                    + device.permission_hint())
    return found[0]["path"]


def cmd_update(args) -> None:
    fw = firmware_dir(args.firmware)
    app_path = Path(args.file) if args.file else fw / "gx100-app.bin"
    app = image.pad4(app_path.read_bytes())
    image.check_app(app)
    say(f"Update to ShifterNoumi {firmware_version(fw) if not args.file else app_path.name}")

    path = enter_bootloader()
    try:
        client = BootloaderClient(HidTransport(path))
    except OSError as e:
        raise Abort(f"Cannot open the bootloader: {e}{device.permission_hint()}") from e
    info = client.info()
    say(f"   Update mode (bootloader {info.version}, chip {info.uid}).")
    if info.installed_matches(app) and not args.force:
        say("   This firmware is already installed. Nothing to do (use --force to rewrite it).")
    else:
        try:
            client.update(app, progress=progress_bar)
        except BootloaderError as e:
            raise Abort(f"\nThe update was interrupted ({e}).\nNothing is lost: the shifter stays in "
                        "update mode (fast blinking LED) with your calibration intact. Plug it "
                        "back in if needed and run update again.") from e
        say("   Written and verified.")
    client.boot()
    if not device.wait_for(device.app_hids, 15):
        raise Abort("The shifter did not restart. Unplug and replug it; if it stays in update "
                    "mode (fast blinking LED), run update again.")
    say("   Shifter restarted with the new firmware. Your calibration was kept.")
    if not args.no_check:
        live_check()
    say(WELCOME)


# ---- check / info / udev

def live_check() -> None:
    say("\nLive check: what the PC receives from the shifter.")

    def on_mode(mode):
        # Only the current mode's instruction (the report says which).
        your_turn(live.INSTRUCTIONS[mode])
        waiting("every position", ctrl_c="skip")

    checklist = live.run(out=print, on_mode=on_mode)
    say("All positions detected." if not checklist.missing else
        f"Not seen: {' '.join(checklist.missing)}. If a position is never detected, calibrate "
        "(docs/user-guide.md#calibration).")


def cmd_reset_calibration(args) -> None:
    say("Reset the calibration to the built-in defaults")
    if not device.app_serial_ports():
        your_turn("plug the shifter's USB cable into the PC.")
        waiting("the shifter on USB")
        if not device.wait_for(device.app_serial_ports, 60):
            raise Abort("No shifter found. Check the cable (some are charge-only)."
                        + device.permission_hint())
    port = device.app_serial_ports()[0]
    say("   Your calibration will be replaced by the defaults (measured on another")
    say("   shifter). Use this when gears are so badly recognised that you cannot enter")
    say("   calibration mode any more; calibrate again afterwards.")
    if not confirm("   Reset the calibration?", args.yes):
        raise Abort("Stopped, nothing was changed.")
    device.send_command(port, layout.CDC_RESET_CALIBRATION)
    # The shifter erases the page and restarts: wait for it to leave, then
    # come back.
    device.wait_for(lambda: not device.app_serial_ports(), 5)
    if not device.wait_for(device.app_serial_ports, 15):
        raise Abort("The shifter did not restart. Unplug and replug it, then run "
                    "reset-calibration again.")
    values = device.read_calibration(device.app_serial_ports()[0])
    if values is None:
        say("   Shifter restarted, but its debug port sent no calibration to check.")
    elif values == layout.DEFAULT_CALIBRATION:
        say("   Done: the shifter is back on the default calibration. Now calibrate it")
        say("   (docs/user-guide.md#calibration).")
    else:
        raise Abort("The shifter restarted but still has its old calibration: the reset did "
                    "not take. Its firmware may be older than this command; update it first.")


def cmd_check(args) -> None:
    if not device.wait_for(device.app_hids, 30):
        raise Abort("No shifter found on USB.")
    live_check()


def cmd_info(args) -> None:
    say(f"shifternoumi-setup {__version__}")
    try:
        fw = firmware_dir(args.firmware)
        # Bundled in the executable: its temporary unpack folder means nothing
        # to the user (and shows their user name in screenshots).
        where = "built in" if getattr(sys, "frozen", False) and not args.firmware else str(fw)
        say(f"Firmware files: {where} (version {firmware_version(fw)})")
    except Abort as e:
        say(str(e))
    stlink_on_board = False
    try:
        state = OpenOcd().chip_state(halt=False)
        for line in describe_chip(state, device.firmware_on_usb()):
            say(line)
        stlink_on_board = state.voltage > 2.5
    except SwdError as e:
        if e.kind == swd.NO_ADAPTER:
            say("ST-Link:  not connected (only needed for the first install)")
        else:
            say(f"ST-Link:  {e}")
    apps, boots, stock = device.app_hids(), device.bootloaders(), device.stock_hids()
    if not apps and not boots and not stock:
        say("No shifter connected.")
    for d in stock:
        say("Shifter with the stock firmware: run 'install' (ST-Link) to replace it.")
    for d in apps:
        say(f"Shifter (running): {d.get('product_string')} serial {d.get('serial_number')}; "
            f"ports: {', '.join(device.app_serial_ports()) or 'none'}")
    for d in boots:
        try:
            client = BootloaderClient(HidTransport(d["path"]))
            i = client.info()
            app = (f"app valid ({i.app_length} bytes, crc 0x{i.app_crc:08X})" if i.app_valid
                   else "app MISSING/INVALID (interrupted update or none installed: run update)")
            say(f"Shifter in update mode: bootloader {i.version}, chip {i.uid}, {app}")
        except (OSError, BootloaderError) as e:
            say(f"Shifter in update mode, not readable: {e}{device.permission_hint()}")
    if stlink_on_board and (apps or boots or stock):
        say("\nWARNING: the shifter is plugged in over USB while the ST-Link is wired to it. "
            + ONE_SOURCE + " Remove the ST-Link wires (at least 3.3V) or unplug the USB cable.")


def cmd_udev(args) -> None:
    if not args.install:
        print(UDEV_RULES, end="")
        return
    if not sys.platform.startswith("linux"):
        raise Abort("udev rules are for Linux only.")
    say(f"Installing {UDEV_PATH} (needs your password)...")
    subprocess.run(["sudo", "tee", UDEV_PATH], input=UDEV_RULES, text=True,
                   stdout=subprocess.DEVNULL, check=True)
    subprocess.run(["sudo", "udevadm", "control", "--reload-rules"], check=True)
    subprocess.run(["sudo", "udevadm", "trigger"], check=True)
    say("Done. Unplug and replug the shifter / ST-Link.")


# ---- entry point

def menu(args) -> None:
    say(f"ShifterNoumi setup {__version__}  (open firmware for the GX100 shifter)\n")
    say("  1  First install (ST-Link, erases the stock firmware)")
    say("  2  Update over USB")
    say("  3  Check the shifter (live)")
    say("  4  Information")
    say("  5  Check the ST-Link connection (writes nothing)")
    say("  6  Reset the calibration to defaults")
    choice = ask("\nChoice: ").strip()
    commands = {"1": cmd_install, "2": cmd_update, "3": cmd_check, "4": cmd_info, "5": cmd_install,
                "6": cmd_reset_calibration}
    if choice not in commands:
        raise Abort("No choice made.")
    args.check = choice == "5"
    say()
    commands[choice](args)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(prog="shifternoumi-setup", description="ShifterNoumi installer for the GX100 shifter")
    parser.add_argument("--firmware", help="folder with the firmware files (manifest.json)")
    parser.add_argument("--version", action="version", version=__version__)
    sub = parser.add_subparsers(dest="cmd")
    p = sub.add_parser("install", help="first install through an ST-Link")
    p.add_argument("--standalone", action="store_true", help="firmware without the USB bootloader")
    p.add_argument("--hex", help="write this .hex instead (expert)")
    p.add_argument("--yes", action="store_true", help="do not ask for confirmation")
    p.add_argument("--check", action="store_true",
                   help="connect and show the chip's state, write nothing")
    p = sub.add_parser("update", help="update over USB")
    p.add_argument("--file", help="app .bin to write instead of the bundled one")
    p.add_argument("--force", action="store_true", help="rewrite even if already installed")
    p.add_argument("--no-check", action="store_true", help="skip the live check afterwards")
    sub.add_parser("check", help="live view of the lever")
    p = sub.add_parser("reset-calibration", help="go back to the built-in calibration")
    p.add_argument("--yes", action="store_true", help="do not ask for confirmation")
    sub.add_parser("info", help="what is connected and installed")
    p = sub.add_parser("udev", help="Linux USB permissions")
    p.add_argument("--install", action="store_true")
    args = parser.parse_args(argv)
    for name, default in (("standalone", False), ("hex", None), ("yes", False), ("check", False), ("file", None),
                          ("force", False), ("no_check", False)):
        if not hasattr(args, name):
            setattr(args, name, default)

    handlers = {"install": cmd_install, "update": cmd_update, "check": cmd_check,
                "reset-calibration": cmd_reset_calibration,
                "info": cmd_info, "udev": cmd_udev, None: menu}
    code = 0
    try:
        handlers[args.cmd](args)
    except (Abort, SwdError, BootloaderError, device.DeviceError, image.ImageError) as e:
        say(f"\n{e}")
        code = 1
    except KeyboardInterrupt:
        say("\nStopped.")
        code = 130
    if args.cmd is None and getattr(sys, "frozen", False) and os.name == "nt":
        ask("\nPress Enter to close.")  # double-clicked exe: keep the window open
    return code
