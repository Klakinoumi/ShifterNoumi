"""layout.py must match Firmware/App/Inc/gx100_boot.h."""
import re
import unittest
from pathlib import Path

from gx100wiz import layout

HEADER = Path(__file__).resolve().parents[2] / "Firmware" / "App" / "Inc" / "gx100_boot.h"


def header_defines():
    """#define NAME value -> {NAME: evaluated value}, resolving earlier names."""
    values = {}
    for line in HEADER.read_text(encoding="utf-8").splitlines():
        m = re.match(r"#define\s+GX100_(\w+)\s+(.+?)\s*(/\*.*)?$", line)
        if not m or m.group(1) == "BOOT_H":
            continue
        name, expr = m.group(1), m.group(2)
        if expr.startswith('"'):
            values[name] = expr.strip('"')
            continue
        expr = re.sub(r"\b(0x[0-9A-Fa-f]+|\d+)U?L?\b", r"\1", expr)
        expr = re.sub(r"GX100_(\w+)", lambda g: str(values[g.group(1)]), expr)
        values[name] = eval(expr, {})  # constants from our own header only
    return values


class LayoutMatchesHeader(unittest.TestCase):
    def test_every_header_value_is_mirrored(self):
        defines = header_defines()
        self.assertGreater(len(defines), 30)
        skip = {"BOOT_VERSION_MAJOR", "BOOT_VERSION_MINOR",
                "TRAILER_LENGTH_OFFSET", "TRAILER_CRC_OFFSET"}
        for name, value in defines.items():
            if name in skip:
                continue
            with self.subTest(name=name):
                self.assertTrue(hasattr(layout, name), f"layout.{name} missing")
                self.assertEqual(getattr(layout, name), value)


STORE = Path(__file__).resolve().parents[2] / "Firmware" / "App" / "Src" / "calibration_store.c"


class DefaultCalibrationMatchesFirmware(unittest.TestCase):
    def test_same_values_and_order(self):
        order = ["NEUTRAL", "1", "2", "3", "4", "5", "6", "SEQ_UP", "SEQ_DOWN"]
        text = STORE.read_text(encoding="utf-8")
        values = []
        for name in order:
            m = re.search(rf"\[GEAR_{name}\]\s*=\s*\{{\{{([^}}]*)\}}\}}", text)
            self.assertIsNotNone(m, f"GEAR_{name} not found in calibration_store.c")
            values += [int(v) for v in m.group(1).split(",")]
        self.assertEqual(layout.DEFAULT_CALIBRATION, values)


if __name__ == "__main__":
    unittest.main()
