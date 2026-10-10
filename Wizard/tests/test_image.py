import struct
import tempfile
import unittest
from pathlib import Path

from gx100wiz import image, layout


def fake_app(length=4096):
    body = bytes((i * 7) & 0xFF for i in range(length))
    return struct.pack("<II", 0x20005000, layout.APP_BASE + 0x101) + body[8:]


class Crc(unittest.TestCase):
    def test_catalogue_value(self):
        # CRC-32/MPEG-2("123456789") = 0x0376E6E7; our CRC takes LE words, so
        # feed the bytes pre-swapped in groups of 4 (padded to 12 bytes).
        def mpeg2(data):
            crc = 0xFFFFFFFF
            for b in data:
                crc ^= b << 24
                for _ in range(8):
                    crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if crc & 0x80000000 else (crc << 1) & 0xFFFFFFFF
            return crc
        self.assertEqual(mpeg2(b"123456789"), 0x0376E6E7)
        data = b"12345678ABCD"
        swapped = b"".join(data[i:i + 4][::-1] for i in range(0, 12, 4))
        self.assertEqual(image.stm32_crc(swapped), mpeg2(data))


class AppChecks(unittest.TestCase):
    def test_accepts_bootloader_layout(self):
        image.check_app(fake_app())

    def test_rejects_standalone_build(self):
        app = struct.pack("<II", 0x20005000, 0x08000101) + bytes(4088)
        with self.assertRaisesRegex(image.ImageError, "standalone"):
            image.check_app(app)

    def test_rejects_non_firmware(self):
        with self.assertRaisesRegex(image.ImageError, "RAM"):
            image.check_app(b"PK\x03\x04" + bytes(60))

    def test_rejects_oversize(self):
        with self.assertRaisesRegex(image.ImageError, "holds"):
            image.check_app(fake_app(layout.APP_MAX_SIZE + 4))


class HexAndPackage(unittest.TestCase):
    def test_hex_roundtrip_across_64k_boundary(self):
        segs = [image.Segment(0x0800FFF0, bytes(range(40))), image.Segment(0x08000000, b"\x01\x02\x03\x04")]
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "x.hex"
            image.write_hex(segs, path)
            back = image.read_hex(path)
        self.assertEqual([(s.address, s.data) for s in back],
                         sorted((s.address, s.data) for s in segs))

    def test_package_full_hex_layout(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            (d / "boot.bin").write_bytes(b"\xB0" * 3408)
            (d / "app.bin").write_bytes(fake_app(25798)[:25798])  # not x4: gets padded
            (d / "sa.bin").write_bytes(b"\x5A" * 100)
            m = image.package(d / "boot.bin", d / "app.bin", d / "sa.bin", "v9", d / "out")
            segs = image.read_hex(d / "out" / "gx100-full.hex")
            app = (d / "out" / "gx100-app.bin").read_bytes()
        self.assertEqual(len(app) % 4, 0)
        self.assertEqual(m["app"]["length"], len(app))
        self.assertEqual([s.address for s in segs], [layout.FLASH_BASE, layout.APP_BASE, layout.APP_TRAILER_ADDR])
        self.assertEqual(segs[1].data, app)
        length, crc = struct.unpack("<II", segs[2].data)
        self.assertEqual((length, crc), (len(app), image.stm32_crc(app)))
        self.assertLess(segs[2].address + 8, layout.CALIBRATION_PAGE + 1)


if __name__ == "__main__":
    unittest.main()
