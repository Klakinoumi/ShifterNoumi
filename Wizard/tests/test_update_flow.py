"""The wizard's update code against the bootloader's real protocol code.

Bootloader/test/boot_proto_host runs Bootloader/src/boot_proto.c over a
simulated flash kept in a file, behind stdin/stdout. Build it first:
    make -C Bootloader test
"""
import os
import sys
import tempfile
import unittest
from pathlib import Path

from gx100wiz import image, layout
from gx100wiz.bootloader import BootloaderClient, BootloaderError, PipeTransport
from tests.test_image import fake_app

ROOT = Path(__file__).resolve().parents[2]
HOST = ROOT / "Bootloader" / "build" / "host" / ("boot_proto_host.exe" if os.name == "nt" else "boot_proto_host")
REAL_APP = ROOT / "Firmware" / "build" / "release-bootloader" / "gx100-app.bin"


class UnpluggedDevice:
    """hidapi device unplugged mid-update: fails in one of the ways seen."""
    def __init__(self, failure):
        self.failure = failure

    def write(self, data):
        if self.failure == "write-oserror":
            raise OSError("write error")
        return -1 if self.failure == "write-minus-one" else len(data)

    def read(self, size, timeout_ms):
        raise OSError("read error")  # seen on hardware, Oct 2026


class HidUnplugged(unittest.TestCase):
    def test_every_failure_is_a_bootloader_error(self):
        from gx100wiz.bootloader import HidTransport
        for failure in ("write-minus-one", "write-oserror", "read-oserror"):
            with self.subTest(failure=failure):
                t = HidTransport.__new__(HidTransport)  # no real device
                t._dev, t._timeout_ms = UnpluggedDevice(failure), 100
                with self.assertRaises(BootloaderError):
                    t.exchange(bytes(layout.REPORT_SIZE))


@unittest.skipUnless(HOST.exists(), f"{HOST.name} not built (make -C Bootloader test)")
class UpdateFlow(unittest.TestCase):
    def setUp(self):
        self._dir = tempfile.TemporaryDirectory()
        self.flash = Path(self._dir.name) / "flash.bin"
        self.transports = []

    def tearDown(self):
        for t in self.transports:
            try:
                t.close()
            except Exception:
                pass
        self._dir.cleanup()

    def connect(self):
        """A fresh 'power-up' of the device, same flash contents."""
        t = PipeTransport([str(HOST), str(self.flash)])
        self.transports.append(t)
        return BootloaderClient(t)

    def calibration_page(self):
        data = self.flash.read_bytes()
        off = layout.CALIBRATION_PAGE - layout.FLASH_BASE
        return data[off:off + layout.FLASH_PAGE_SIZE]

    def test_update_then_info(self):
        dev = self.connect()
        self.assertFalse(dev.info().app_valid)
        app = fake_app(25796)
        seen = []
        dev.update(app, progress=lambda done, total: seen.append(done))
        self.assertEqual(seen[-1], len(app))
        info = dev.info()
        self.assertTrue(info.app_valid)
        self.assertTrue(info.installed_matches(app))
        self.assertFalse(info.installed_matches(fake_app(4096)))
        self.assertEqual(self.calibration_page(), b"\xFF" * layout.FLASH_PAGE_SIZE)

    def test_power_cut_mid_update_then_retry(self):
        dev = self.connect()
        dev.update(fake_app(8000))

        app = fake_app(30000)
        writes = []

        def cut_power(done, total):
            writes.append(done)
            if len(writes) == 100:
                raise KeyboardInterrupt  # stands in for the cable being pulled

        with self.assertRaises(KeyboardInterrupt):
            dev.update(app, progress=cut_power)
        self.transports[-1].close()

        dev = self.connect()
        self.assertFalse(dev.info().app_valid, "half-written app must not look valid")
        dev.update(app)
        self.assertTrue(dev.info().installed_matches(app))

    def test_refuses_standalone_image_before_erasing(self):
        dev = self.connect()
        good = fake_app(4096)
        dev.update(good)
        standalone = bytearray(good)
        standalone[4:8] = (0x08000101).to_bytes(4, "little")
        with self.assertRaises(image.ImageError):
            dev.update(bytes(standalone))
        self.assertTrue(dev.info().installed_matches(good), "installed app must be untouched")

    def test_boot_command_ends_session(self):
        dev = self.connect()
        dev.update(fake_app(4096))
        dev.boot()
        with self.assertRaises(BootloaderError):
            dev.info()  # the device "reset"

    @unittest.skipUnless(REAL_APP.exists(), "Firmware/build/release-bootloader not built")
    def test_real_firmware_image(self):
        app = image.pad4(REAL_APP.read_bytes())
        dev = self.connect()
        dev.update(app)
        self.assertTrue(dev.info().installed_matches(app))


if __name__ == "__main__":
    unittest.main()
