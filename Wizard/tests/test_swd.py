"""The ST-Link side of the wizard (swd.py, install), against OpenOCD output.

tests/openocd/ holds real output of the ST-Link V2 clone on the reference
unit: stlink_v2_gx100_bootloader.txt (this firmware behind the bootloader)
and stlink_v2_read_protected.txt (read protection set again with
STM32CubeProgrammer, as on a stock shifter: the flash size register cannot
be read, OpenOCD fails there). The other failure outputs are written from
OpenOCD's own messages, not recorded.
"""
import argparse
import io
import json
import subprocess
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

from gx100wiz import cli, swd

REAL_OK = (Path(__file__).parent / "openocd" / "stlink_v2_gx100_bootloader.txt").read_text()

NO_ADAPTER = "Info : auto-selecting first available session transport \"hla_swd\".\nError: open failed\n"
NO_DRIVER = ("Error: libusb_open() failed with LIBUSB_ERROR_NOT_SUPPORTED\n"
             "Error: open failed\n")
BUSY = "Error: libusb_open() failed with LIBUSB_ERROR_ACCESS\nError: open failed\n"
WIRING_NO_POWER = ("Info : STLINK V2J48S7 (API v2) VID:PID 0483:3748\n"
                   "Info : Target voltage: 0.000000\n"
                   "Error: init mode failed (unable to connect to the target)\n")
WIRING_SWAPPED = ("Info : STLINK V2J48S7 (API v2) VID:PID 0483:3748\n"
                  "Info : Target voltage: 3.210512\n"
                  "Error: init mode failed (unable to connect to the target)\n")
PROTECTED = (Path(__file__).parent / "openocd" / "stlink_v2_read_protected.txt").read_text()


class FakeOpenOcd:
    """Replaces subprocess.run: answers each OpenOCD call with the next
    (returncode, output) and keeps the argument lists."""
    def __init__(self, *answers):
        self.answers = list(answers)
        self.calls = []

    def __call__(self, args, **kwargs):
        self.calls.append(args)
        code, out = self.answers.pop(0)
        return subprocess.CompletedProcess(args, code, out)


def openocd(*answers):
    fake = FakeOpenOcd(*answers)
    return fake, mock.patch.object(swd.subprocess, "run", fake)


class ChipStateTest(unittest.TestCase):
    def test_real_output(self):
        fake, patch = openocd((0, REAL_OK))
        with patch:
            state = swd.OpenOcd(exe="openocd").chip_state(halt=False)
        self.assertEqual(state.adapter, "ST-Link V2J48S7")
        self.assertAlmostEqual(state.voltage, 3.21, places=2)
        self.assertEqual(state.dev_id, 0x410)
        self.assertEqual(state.flash_kb, 64)
        self.assertFalse(state.read_protected)
        self.assertTrue(state.bootloader_write_protected)

    def test_protected_chip(self):
        """Real output: OpenOCD fails on the flash size, the rest is there."""
        fake, patch = openocd((1, PROTECTED))
        with patch:
            state = swd.OpenOcd(exe="openocd").chip_state()
        self.assertTrue(state.read_protected)
        self.assertIsNone(state.flash_kb)
        self.assertEqual(state.dev_id, 0x410)

    def test_unreadable_size_on_unprotected_chip_is_an_error(self):
        out = PROTECTED.replace("0x4002201c: 03fffffe", "0x4002201c: 03fffffc")
        fake, patch = openocd((1, out))
        with patch, self.assertRaises(swd.SwdError):
            swd.OpenOcd(exe="openocd").chip_state()

    def test_check_leaves_chip_running_and_opens_no_server(self):
        fake, patch = openocd((0, REAL_OK), (0, REAL_OK))
        with patch:
            swd.OpenOcd(exe="openocd").chip_state(halt=False)
            swd.OpenOcd(exe="openocd").chip_state(halt=True)
        check, install = fake.calls
        self.assertNotIn("reset halt", check)
        self.assertIn("reset halt", install)
        for args in fake.calls:
            for server in ("gdb port disabled", "tcl port disabled", "telnet port disabled"):
                self.assertIn(server, args)


class ExplainTest(unittest.TestCase):
    def kind(self, output, windows=True):
        return swd.explain(output, windows=windows)

    def test_missing_driver_is_not_reported_as_missing_stlink(self):
        kind, text = self.kind(NO_DRIVER)
        self.assertEqual(kind, swd.NO_DRIVER)
        self.assertIn("STSW-LINK009", text)

    def test_no_stlink(self):
        self.assertEqual(self.kind(NO_ADAPTER)[0], swd.NO_ADAPTER)

    def test_access_denied(self):
        self.assertEqual(self.kind(BUSY, windows=True)[0], swd.BUSY)
        self.assertEqual(self.kind(BUSY, windows=False)[0], swd.NO_PERMISSION)

    def test_wiring(self):
        kind, text = self.kind(WIRING_NO_POWER)
        self.assertEqual(kind, swd.NO_TARGET)
        self.assertIn("3.3V", text)
        kind, text = self.kind(WIRING_SWAPPED)
        self.assertEqual(kind, swd.NO_TARGET)
        self.assertIn("SWDIO", text)
        self.assertNotIn("3.3V wire", text)

    def test_failure_carries_kind_and_output(self):
        fake, patch = openocd((1, NO_DRIVER))
        with patch, self.assertRaises(swd.SwdError) as e:
            swd.OpenOcd(exe="openocd").chip_state()
        self.assertEqual(e.exception.kind, swd.NO_DRIVER)
        self.assertIn("LIBUSB_ERROR_NOT_SUPPORTED", e.exception.output)


class ReadProtectionTest(unittest.TestCase):
    def test_unlock_error_ignored_when_chip_is_unprotected(self):
        fake, patch = openocd((1, "Error: something after the erase\n"), (0, REAL_OK))
        with patch:
            state = swd.OpenOcd(exe="openocd").remove_read_protection()
        self.assertFalse(state.read_protected)
        self.assertIn("stm32f1x unlock 0", fake.calls[0])

    def test_still_protected_is_reported(self):
        fake, patch = openocd((0, ""), (0, PROTECTED))
        with patch:
            self.assertTrue(swd.OpenOcd(exe="openocd").remove_read_protection().read_protected)

    def test_chip_silent_after_erase_raises(self):
        fake, patch = openocd((1, ""), (1, WIRING_SWAPPED))
        with patch, self.assertRaises(swd.SwdError):
            swd.OpenOcd(exe="openocd").remove_read_protection()


class FakeChip:
    """Stands for swd.OpenOcd in cli.cmd_install; records what is done."""
    def __init__(self, state, unlock_error=False, flash_after_unlock=64):
        self.state, self.unlock_error, self.done = state, unlock_error, []
        self.flash_after_unlock = flash_after_unlock
        self.speed_khz = 1000

    def __call__(self):
        return self

    def chip_state(self, halt=True):
        self.done.append(("state", halt))
        return self.state

    def remove_read_protection(self):
        self.done.append("unlock")
        if self.unlock_error:
            raise swd.SwdError("no answer", swd.NO_TARGET)
        self.state = swd.ChipState(**{**vars(self.state), "read_protected": False,
                                      "flash_kb": self.flash_after_unlock})
        return self.state

    def set_bootloader_write_protection(self, on):
        self.done.append(("wrp", on))

    def mass_erase(self):
        self.done.append("erase")

    def program(self, path):
        self.done.append("program")


def chip(read_protected=False):
    """A protected chip hides its flash size, like the real one."""
    return swd.ChipState("ST-Link V2J48S7", 3.21, 0x410, None if read_protected else 64,
                         read_protected, not read_protected)


class InstallTest(unittest.TestCase):
    def setUp(self):
        self._dir = tempfile.TemporaryDirectory()
        fw = Path(self._dir.name)
        (fw / "manifest.json").write_text(json.dumps({"version": "test"}))
        (fw / "gx100-full.hex").write_text(":00000001FF\n")
        self.fw = str(fw)

    def tearDown(self):
        self._dir.cleanup()

    def install(self, fake, answer="", check=False, on_usb=False, unplugged_in_time=True, stock=False):
        """on_usb: the shifter is also plugged in over USB at the start,
        running this firmware, or the stock one if stock=True."""
        args = argparse.Namespace(firmware=self.fw, hex=None, standalone=False, yes=False, check=check)
        out = io.StringIO()
        usb = {"present": on_usb}

        def wait_for(find, *a):
            if usb["present"] and unplugged_in_time and "program" not in fake.done:
                usb["present"] = False  # the user unplugs it while we wait
            elif "program" in fake.done:
                usb["present"] = True   # cables swapped after writing
            return find()

        with mock.patch.object(cli, "OpenOcd", fake), mock.patch.object(cli, "ask", lambda p: answer), \
                mock.patch.object(cli.device, "app_hids",
                                  lambda: [{}] if usb["present"] and (not stock or "program" in fake.done)
                                  else []), \
                mock.patch.object(cli.device, "stock_hids",
                                  lambda: [{}] if usb["present"] and stock and "program" not in fake.done
                                  else []), \
                mock.patch.object(cli.device, "bootloaders", lambda: []), \
                mock.patch.object(cli.device, "wait_for", wait_for), \
                mock.patch.object(cli, "live_check", lambda: None), redirect_stdout(out):
            cli.cmd_install(args)
        return out.getvalue()

    def test_shifter_on_usb_must_be_unplugged_first(self):
        fake = FakeChip(chip())
        with self.assertRaises(cli.Abort):
            self.install(fake, on_usb=True, unplugged_in_time=False)
        self.assertEqual(fake.done, [], "nothing done with the board on two supplies")
        fake = FakeChip(chip())
        out = self.install(fake, on_usb=True)
        self.assertIn("unplug the shifter's USB cable", out)
        self.assertIn("program", fake.done)
        self.assertIn("Welcome in the future", out)

    def test_stock_shifter_on_usb_must_be_unplugged_too(self):
        fake = FakeChip(chip(read_protected=True))
        with self.assertRaises(cli.Abort):
            self.install(fake, on_usb=True, unplugged_in_time=False, stock=True)
        self.assertEqual(fake.done, [])

    def test_firmware_identified_on_usb(self):
        """A protected chip cannot be read: what USB showed is reported."""
        out = self.install(FakeChip(chip(read_protected=True)), answer="ERASE", on_usb=True)
        self.assertIn("running ShifterNoumi", out)
        self.assertIn("holding ShifterNoumi (seen on USB)", out)
        self.assertIn("nothing of the stock firmware is left", out)
        out = self.install(FakeChip(chip(read_protected=True)), answer="ERASE", on_usb=True, stock=True)
        self.assertIn("holding the stock firmware (seen on USB)", out)
        self.assertIn("ERASES WHATEVER IS ON THE CHIP", out)
        out = self.install(FakeChip(chip(read_protected=True)), answer="ERASE")
        self.assertIn("unknown firmware", out)

    def test_wiring_and_swap_warn_about_one_supply(self):
        out = self.install(FakeChip(chip()))
        self.assertIn("ONE source only", out)
        self.assertIn("at least the 3.3V wire", out)

    def test_first_install_says_as_is(self):
        out = self.install(FakeChip(chip()))
        self.assertIn('"AS IS"', out)
        self.assertIn("declines all responsibility", out)
        self.assertNotIn('"AS IS"', self.install(FakeChip(chip()), check=True))

    def test_check_writes_nothing(self):
        for protected in (False, True):
            fake = FakeChip(chip(read_protected=protected))
            out = self.install(fake, check=True)
            self.assertEqual(fake.done, [("state", False)])
            self.assertIn("ST-Link V2J48S7", out)
            self.assertIn("ERASE" if protected else "calibration would be kept", out)

    def test_protected_chip_needs_erase_typed(self):
        fake = FakeChip(chip(read_protected=True))
        with self.assertRaises(cli.Abort):
            self.install(fake, answer="erase")
        self.assertNotIn("unlock", fake.done)
        self.assertNotIn("program", fake.done)

    def test_protected_chip_install(self):
        fake = FakeChip(chip(read_protected=True))
        self.install(fake, answer="ERASE")
        self.assertEqual(fake.done[1:], ["unlock", "erase", "program", ("wrp", True)])

    def test_protected_chip_erased_after_unlocking_the_bootloader_pages(self):
        """The reference unit: still write-protected after the unlock."""
        fake = FakeChip(chip(read_protected=True))
        fake.state = swd.ChipState(**{**vars(fake.state), "bootloader_write_protected": True})
        out = self.install(fake, answer="ERASE")
        self.assertEqual(fake.done[1:], ["unlock", ("wrp", False), "erase", "program", ("wrp", True)])
        self.assertIn("Calibrate it once", out)

    def test_unprotected_chip_is_not_erased(self):
        fake = FakeChip(chip())
        out = self.install(fake)
        self.assertNotIn("erase", fake.done)
        self.assertIn("calibration (if any) was kept", out)

    def test_size_checked_after_erase(self):
        fake = FakeChip(chip(read_protected=True), flash_after_unlock=32)
        with self.assertRaises(cli.Abort) as e:
            self.install(fake, answer="ERASE")
        self.assertIn("32 KB", str(e.exception))
        self.assertNotIn("program", fake.done)

    def test_chip_silent_after_erase_says_run_again(self):
        fake = FakeChip(chip(read_protected=True), unlock_error=True)
        with self.assertRaises(cli.Abort) as e:
            self.install(fake, answer="ERASE")
        self.assertIn("run install again", str(e.exception))
        self.assertNotIn("program", fake.done)


if __name__ == "__main__":
    unittest.main()
