"""First install through an ST-Link, driven by OpenOCD.

OpenOCD is looked up in this order: $GX100_OPENOCD, the copy bundled with
the wizard (openocd/bin/ next to it), then PATH. Its own scripts directory
is found relative to the binary.
"""
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

FLASH_OBR = 0x4002201C    # bit 1: read protection active
FLASH_WRPR = 0x40022020   # bit n = 0: pages 4n..4n+3 write-protected
FLASH_SIZE_REG = 0x1FFFF7E0
DBGMCU_IDCODE = 0xE0042000

# OpenOCD opens gdb/tcl/telnet servers by default; on Windows the first one
# pops up a firewall prompt in the middle of the install.
NO_SERVERS = ["gdb port disabled", "tcl port disabled", "telnet port disabled"]

# What went wrong, for callers that word it differently (info vs install)
NO_ADAPTER = "no-adapter"
NO_DRIVER = "no-driver"
BUSY = "busy"
NO_PERMISSION = "no-permission"
NO_TARGET = "no-target"
OTHER = "other"


class SwdError(Exception):
    def __init__(self, message: str, kind: str = OTHER, output: str = ""):
        super().__init__(message)
        self.kind = kind
        self.output = output


def _bundle_dir() -> Path:
    if getattr(sys, "frozen", False):
        return Path(getattr(sys, "_MEIPASS", Path(sys.executable).parent))
    return Path(__file__).resolve().parents[1] / "vendor"


def find_openocd() -> str:
    exe = "openocd.exe" if os.name == "nt" else "openocd"
    candidates = [os.environ.get("GX100_OPENOCD"), str(_bundle_dir() / "openocd" / "bin" / exe),
                  shutil.which("openocd")]
    for c in candidates:
        if c and Path(c).is_file():
            return c
    raise SwdError("OpenOCD not found (set GX100_OPENOCD, or install openocd)")


@dataclass
class ChipState:
    adapter: str
    voltage: float
    dev_id: int
    flash_kb: object  # int, or None when read protection hides it
    read_protected: bool
    bootloader_write_protected: bool


def adapter_name(output: str) -> str:
    """'ST-Link V2J48S7' from OpenOCD's 'STLINK V2J48S7 (API v2) VID:PID ...' line."""
    m = re.search(r"STLINK (\S+)", output)
    return f"ST-Link {m.group(1)}" if m else "ST-Link"


def _voltage(output: str):
    m = re.search(r"Target voltage: ([\d.]+)", output)
    return float(m.group(1)) if m else None


def explain(output: str, windows: bool = os.name == "nt"):
    """Turns OpenOCD's failure output into (kind, advice). The libusb errors
    come first: OpenOCD follows them with 'open failed' too."""
    if re.search(r"LIBUSB_ERROR_NOT_SUPPORTED", output):
        return NO_DRIVER, ("The ST-Link is plugged in but Windows has no driver for it. Install ST's "
                           "ST-Link driver (STSW-LINK009), unplug and replug the ST-Link.")
    if re.search(r"LIBUSB_ERROR_(ACCESS|BUSY)|could not claim", output, re.I):
        if windows:
            return BUSY, ("The ST-Link is in use by another program. Close STM32CubeProgrammer, "
                          "STM32CubeIDE or any other debugger, then retry.")
        return NO_PERMISSION, ("No permission to use the ST-Link. Run 'shifternoumi-setup udev --install' "
                               "once, then unplug and replug it.")
    if re.search(r"open failed|No ST-?LINK|LIBUSB_ERROR_(NOT_FOUND|NO_DEVICE)", output, re.I):
        return NO_ADAPTER, "No ST-Link found. Check it is plugged into the PC."
    if re.search(r"unable to connect to the target|init mode failed|Error connecting DP", output, re.I):
        v = _voltage(output)
        if v is not None and v < 2.5:
            return NO_TARGET, (f"The ST-Link reads {v:.1f} V on the board: the 3.3V wire is probably "
                               "not connected. (Some ST-Link clones always read 0 V: if 3.3V is "
                               "wired, check SWDIO, SWCLK and GND.)")
        return NO_TARGET, ("The ST-Link cannot talk to the chip. Check SWDIO and SWCLK (often "
                           "swapped) and GND.")
    tail = "\n".join(output.strip().splitlines()[-6:])
    return OTHER, f"OpenOCD failed:\n{tail}"


class OpenOcd:
    def __init__(self, exe: str = None, speed_khz: int = 1000):
        self.exe = exe or find_openocd()
        self.speed_khz = speed_khz

    def run(self, commands, timeout_s: float = 60, halt: bool = True) -> str:
        """halt=False leaves the chip running (read-only checks)."""
        args = [self.exe]
        for c in NO_SERVERS:
            args += ["-c", c]
        args += ["-f", "interface/stlink.cfg",
                 "-c", "set CPUTAPID 0",          # accept clone chips (CS32/APM32 IDCODEs)
                 "-f", "target/stm32f1x.cfg",
                 "-c", f"adapter speed {self.speed_khz}",
                 "-c", "init"]
        if halt:
            args += ["-c", "reset halt"]
        for c in commands:
            args += ["-c", c]
        args += ["-c", "shutdown"]
        try:
            proc = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  timeout=timeout_s, text=True, errors="replace")
        except subprocess.TimeoutExpired as e:
            raise SwdError("OpenOCD did not finish in time") from e
        if proc.returncode != 0:
            kind, message = explain(proc.stdout)
            raise SwdError(message, kind, proc.stdout)
        return proc.stdout

    @staticmethod
    def _word(output: str, address: int) -> int:
        m = re.search(rf"0x{address:08x}:\s+([0-9a-f]+)", output, re.I)
        if not m:
            raise SwdError(f"could not read 0x{address:08X}", OTHER, output)
        return int(m.group(1), 16)

    def chip_state(self, halt: bool = True) -> ChipState:
        # The flash size register is read last: on a read-protected chip
        # (stock firmware) it cannot be read, OpenOCD stops there with an
        # error, and the three registers before it are all that is needed.
        try:
            out = self.run([f"mdw 0x{DBGMCU_IDCODE:08x}", f"mdw 0x{FLASH_OBR:08x}",
                            f"mdw 0x{FLASH_WRPR:08x}", f"mdh 0x{FLASH_SIZE_REG:08x}"], halt=halt)
        except SwdError as e:
            hidden = re.search(rf"Failed to read memory at 0x{FLASH_SIZE_REG:08x}", e.output, re.I)
            if not hidden:
                raise
            out = e.output
        obr = self._word(out, FLASH_OBR)
        size = re.search(rf"0x{FLASH_SIZE_REG:08x}:\s+([0-9a-f]+)", out, re.I)
        if size is None and not obr & 0x2:
            raise SwdError(f"could not read 0x{FLASH_SIZE_REG:08X}", OTHER, out)
        return ChipState(
            adapter=adapter_name(out),
            voltage=_voltage(out) or 0.0,
            dev_id=self._word(out, DBGMCU_IDCODE) & 0xFFF,
            flash_kb=int(size.group(1), 16) & 0xFFFF if size else None,
            read_protected=bool(obr & 0x2),
            bootloader_write_protected=not (self._word(out, FLASH_WRPR) & 0x1),
        )

    def remove_read_protection(self) -> ChipState:
        """Removes read protection level 1 and returns the state read
        afterwards. OpenOCD can report an error while it went through, so its
        result is ignored: the state decides. On the reference unit this did
        NOT erase the flash (calibration and mode survived, Oct 2026): call
        mass_erase() afterwards."""
        try:
            self.run(["stm32f1x unlock 0", "reset halt"], timeout_s=120)
        except SwdError:
            pass
        return self.chip_state()

    def mass_erase(self) -> None:
        """Erases the whole main flash (bootloader write protection must be
        off). Used after removing the read protection, which does not erase
        the flash on every chip."""
        # Page by page, the same erase write_image uses on every install.
        self.run(["flash erase_sector 0 0 last", "reset halt"], timeout_s=120)

    def set_bootloader_write_protection(self, on: bool) -> None:
        """Protection block 0 = pages 0-3 = the bootloader (OpenOCD counts
        protection blocks here, not pages). Takes effect after the reset;
        leaves the chip running."""
        self.run([f"flash protect 0 0 0 {'on' if on else 'off'}", "reset run"])

    def program(self, hex_path: Path) -> None:
        """Erases only the pages the image covers (calibration page kept),
        writes, verifies, then starts the chip."""
        p = "{" + str(Path(hex_path).resolve()).replace("\\", "/") + "}"
        self.run([f"flash write_image erase {p} 0 ihex", f"verify_image {p} 0 ihex", "reset run"],
                 timeout_s=180)
