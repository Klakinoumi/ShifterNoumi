"""Finding the shifter on USB: the application (HID gamepad + serial port)
and the bootloader (vendor HID), and switching from one to the other."""
import sys
import time

from . import layout


class DeviceError(Exception):
    pass


def _hid():
    import hid
    return hid


# The stock firmware's USB identity (docs/hardware.md#the-stock-usb-device-for-reference)
STOCK_VID, STOCK_PID = 0x04B0, 0x5750


def stock_hids():
    return _hid().enumerate(STOCK_VID, STOCK_PID)


def firmware_on_usb():
    """What the shifter plugged in over USB runs: 'community' (this
    firmware or its bootloader), 'stock', or None (not plugged in)."""
    if app_hids() or bootloaders():
        return "community"
    if stock_hids():
        return "stock"
    return None


def bootloaders():
    return _hid().enumerate(layout.USB_VID, layout.USB_PID_BOOT)


def app_hids():
    return _hid().enumerate(layout.USB_VID, layout.USB_PID_APP)


def app_serial_ports():
    from serial.tools import list_ports
    return sorted(p.device for p in list_ports.comports()
                  if p.vid == layout.USB_VID and p.pid == layout.USB_PID_APP)


def wait_for(find, timeout_s: float, poll_s: float = 0.25):
    """Calls find() until it returns something truthy or the time is up."""
    deadline = time.monotonic() + timeout_s
    while True:
        found = find()
        if found or time.monotonic() >= deadline:
            return found
        time.sleep(poll_s)


def send_command(port: str, command: str) -> None:
    """Sends one command line to the running app (it then resets)."""
    import serial
    try:
        with serial.Serial(port, 115200, timeout=1, write_timeout=2) as s:
            s.write((command + "\n").encode("ascii"))
            s.flush()
    except serial.SerialException as e:
        # The device may reset before the port is closed cleanly: harmless.
        if "PermissionError" in str(e) or "Permission denied" in str(e):
            raise DeviceError(f"cannot open {port}: {e}{permission_hint()}") from e


def request_bootloader(port: str) -> None:
    """Asks the running app to reboot into the bootloader."""
    send_command(port, layout.CDC_ENTER_BOOTLOADER)


def parse_cal_line(line: str):
    """'CAL,t_ms,<54 values>' -> the 54 values, or None."""
    fields = line.strip().split(",")
    if len(fields) != 56 or fields[0] != "CAL":
        return None
    try:
        return [int(v) for v in fields[2:]]
    except ValueError:
        return None


def read_calibration(port: str, timeout_s: float = 5):
    """The calibration the app is using, from the CAL line of its debug
    port (sent every 2 s), or None if none arrived in time."""
    import serial
    deadline = time.monotonic() + timeout_s
    try:
        with serial.Serial(port, 115200, timeout=0.5) as s:
            while time.monotonic() < deadline:
                values = parse_cal_line(s.readline().decode("ascii", "replace"))
                if values:
                    return values
    except serial.SerialException as e:
        raise DeviceError(f"cannot read {port}: {e}{permission_hint()}") from e
    return None


def open_bootloader_path(timeout_s: float = 0) -> bytes:
    found = wait_for(bootloaders, timeout_s) if timeout_s else bootloaders()
    if not found:
        raise DeviceError("no GX100 bootloader found")
    return found[0]["path"]


def permission_hint() -> str:
    if sys.platform.startswith("linux"):
        return ("\nOn Linux, USB access needs the GX100 udev rules: run "
                "'shifternoumi-setup udev --install' once, then unplug and replug.")
    return ""
