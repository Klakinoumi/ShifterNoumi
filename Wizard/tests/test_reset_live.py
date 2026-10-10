"""reset-calibration (cli/device) and the live check's checklist (live.py),
with the serial port and USB enumeration replaced by fakes."""
import argparse
import io
import unittest
from contextlib import redirect_stdout
from unittest import mock

from gx100wiz import cli, device, layout, live


def report(buttons=0, sequential=False):
    r = bytearray(16)
    r[0], r[1] = buttons & 0xFF, buttons >> 8
    r[14] = 1 if sequential else 0
    return bytes(r)


class ChecklistTest(unittest.TestCase):
    def test_h_pattern(self):
        c = live.Checklist()
        for bit in (0, 1, 2, 3, 4, 5):
            c.update(report(1 << bit))
        self.assertEqual(c.missing, ["R"])
        c.update(report(1 << 10))
        self.assertEqual(c.missing, [])

    def test_sequential_needs_only_shifts(self):
        c = live.Checklist()
        c.update(report(1 << 6, sequential=True))
        self.assertEqual(c.missing, ["shift-"])
        c.update(report(1 << 7, sequential=True))
        self.assertEqual(c.missing, [])
        self.assertIn("sequential", c.line([]))

    def test_mode_switch_restarts_the_list(self):
        c = live.Checklist()
        c.update(report(1 << 0))
        c.update(report(0, sequential=True))
        self.assertEqual(c.missing, ["shift+", "shift-"])


class FakeHid:
    """Stands for the hidapi module: one shifter sending scripted reports."""
    def __init__(self, reports):
        self.reports = list(reports)

    def enumerate(self, vid, pid):
        return [{"path": b"fake"}]

    def device(self):
        reports = self.reports

        class Dev:
            def open_path(self, path):
                pass

            def read(self, size, timeout):
                if not reports:
                    raise KeyboardInterrupt  # the user's Ctrl+C
                return list(reports.pop(0))

            def close(self):
                pass
        return Dev()


class LiveRunTest(unittest.TestCase):
    def run_live(self, reports):
        events = []
        out = io.StringIO()

        def say(text="", end="\n", flush=False):
            out.write(text + end)

        with mock.patch.dict("sys.modules", {"hid": FakeHid(reports)}):
            live.run(out=say, on_mode=lambda m: (events.append(m), out.write(f"[{m}]\n")))
        return events, out.getvalue()

    def test_instruction_only_for_the_current_mode(self):
        events, text = self.run_live([report(0), report(1 << 0), report(1 << 1)])
        self.assertEqual(events, [live.H_MODE], "told once, for H-pattern only")
        self.assertLess(text.index("[H-pattern]"), text.index("H-pattern   now"))

    def test_switch_during_the_check_gives_the_new_instruction(self):
        events, _ = self.run_live([report(0), report(1 << 0), report(0, sequential=True),
                                   report(1 << 6, sequential=True)])
        self.assertEqual(events, [live.H_MODE, live.SEQ_MODE])

    def test_stops_when_every_position_of_the_mode_is_seen(self):
        events, _ = self.run_live([report(1 << 6, sequential=True), report(1 << 7, sequential=True),
                                   report(1 << 0)])  # never read: already complete
        self.assertEqual(events, [live.SEQ_MODE])

    def test_cli_says_only_the_current_mode(self):
        out = io.StringIO()
        with mock.patch.dict("sys.modules", {"hid": FakeHid([report(0), report(1 << 0)])}), \
                redirect_stdout(out):
            cli.live_check()
        self.assertIn("H-pattern mode", out.getvalue())
        self.assertNotIn("sequential mode", out.getvalue())
        self.assertEqual(out.getvalue().count("Ctrl+C"), 1)


class CalLineTest(unittest.TestCase):
    def test_parse(self):
        line = "CAL,7658665," + ",".join(str(v) for v in layout.DEFAULT_CALIBRATION) + "\r\n"
        self.assertEqual(device.parse_cal_line(line), layout.DEFAULT_CALIBRATION)
        self.assertIsNone(device.parse_cal_line("7659525,2979,3138,2931,2962,2790,2916,0,IDLE,SEQ\r\n"))
        self.assertIsNone(device.parse_cal_line("CAL,1,2,3\r\n"))


class ResetCalibrationTest(unittest.TestCase):
    """The shifter disappears after the command and comes back."""
    def run_reset(self, calibration_after, answer="y", yes=False):
        ports = iter([["COM10"], ["COM10"], [], ["COM10"], ["COM10"]])
        sent = []
        out = io.StringIO()
        with mock.patch.object(device, "app_serial_ports", lambda: next(ports, ["COM10"])), \
                mock.patch.object(device, "send_command", lambda port, cmd: sent.append((port, cmd))), \
                mock.patch.object(device, "read_calibration", lambda port: calibration_after), \
                mock.patch.object(device.time, "sleep", lambda s: None), \
                mock.patch.object(cli, "ask", lambda p: answer), redirect_stdout(out):
            cli.cmd_reset_calibration(argparse.Namespace(yes=yes))
        return sent, out.getvalue()

    def test_reset_to_defaults(self):
        sent, out = self.run_reset(list(layout.DEFAULT_CALIBRATION))
        self.assertEqual(sent, [("COM10", layout.CDC_RESET_CALIBRATION)])
        self.assertIn("back on the default calibration", out)

    def test_refused_sends_nothing(self):
        with self.assertRaises(cli.Abort):
            sent, _ = self.run_reset(None, answer="n")

    def test_old_firmware_keeps_its_calibration(self):
        other = list(layout.DEFAULT_CALIBRATION)
        other[0] += 30
        with self.assertRaises(cli.Abort) as e:
            self.run_reset(other, yes=True)
        self.assertIn("update it first", str(e.exception))


if __name__ == "__main__":
    unittest.main()
