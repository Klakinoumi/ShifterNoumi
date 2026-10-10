"""Live check: shows what the shifter sends to the PC while the user moves
the lever. Report layout: docs/usb-interface.md#input-report-16-bytes."""
import time

from . import layout

BUTTONS = {0: "1", 1: "2", 2: "3", 3: "4", 4: "5", 5: "6", 6: "shift+", 7: "shift-", 10: "R"}
H_PATTERN = ["1", "2", "3", "4", "5", "6", "R"]
SEQUENTIAL = ["shift+", "shift-"]
H_MODE, SEQ_MODE = "H-pattern", "sequential"


def decode(report: bytes):
    """-> (list of active positions, mode text)."""
    bits = report[0] | (report[1] << 8)
    active = [name for bit, name in BUTTONS.items() if bits & (1 << bit)]
    mode = SEQ_MODE if len(report) > 14 and report[14] & 1 else H_MODE
    return active, mode


def expected(mode: str) -> list:
    return SEQUENTIAL if mode == SEQ_MODE else H_PATTERN


# What to do with the lever, for the mode the shifter is in.
INSTRUCTIONS = {
    H_MODE: "the shifter is in H-pattern mode: move the lever through\n"
            "   1 2 3 4 5 6, then reverse (collar lifted).",
    SEQ_MODE: "the shifter is in sequential mode: push the lever up (shift +),\n"
              "   then down (shift -).",
}


class Checklist:
    """What has been seen, against the positions of the current mode. A
    mode switch during the check starts the list again."""
    def __init__(self):
        self.mode = None
        self.seen = set()
        self.mode_changed = False  # this report started a mode (first one, or a switch)

    def update(self, report: bytes):
        active, mode = decode(report)
        self.mode_changed = mode != self.mode
        if self.mode_changed:
            self.mode, self.seen = mode, set()
        self.seen.update(active)
        return active

    @property
    def missing(self) -> list:
        return [p for p in expected(self.mode or H_MODE) if p not in self.seen]

    def line(self, active) -> str:
        marks = " ".join(p if p in self.seen else "." for p in expected(self.mode))
        now = ", ".join(active) if active else "neutral"
        return f"\r  {self.mode:10}  now: {now:8}  seen: {marks}   "


def run(out=print, stop_when_all_seen=True, timeout_s: float = 300, on_mode=None) -> Checklist:
    """Prints the live position until every position of the current mode
    has been seen, Ctrl+C, or timeout. on_mode(mode) is called when the
    first report arrives and on every mode switch, before the status line,
    to say what to do with the lever."""
    import hid
    devices = hid.enumerate(layout.USB_VID, layout.USB_PID_APP)
    if not devices:
        raise RuntimeError("shifter not found on USB")
    dev = hid.device()
    dev.open_path(devices[0]["path"])
    checklist = Checklist()
    status_shown = False
    deadline = time.monotonic() + timeout_s
    try:
        while time.monotonic() < deadline:
            report = bytes(dev.read(64, 100))
            if len(report) < 2:
                continue
            active = checklist.update(report)
            if checklist.mode_changed and on_mode is not None:
                if status_shown:
                    out("")  # end the status line before the new instruction
                on_mode(checklist.mode)
            out(checklist.line(active), end="", flush=True)
            status_shown = True
            if stop_when_all_seen and not checklist.missing:
                break
    except KeyboardInterrupt:
        pass
    finally:
        dev.close()
        out("")
    return checklist
