# Testing and tools

The application logic compiles on a PC, so most behaviour can be checked
against real recorded sessions without touching the shifter. This page
covers the host tests, the replay tools, and the capture scripts that
produce the data.

## Host tests

Requirements: `gcc` (Linux/macOS, or MinGW / Git Bash on Windows). No
hardware and no ST tools needed.

```sh
sh Firmware/App/test/run_tests.sh
```

```
ok    test_calibration_flash
ok    test_calibration_mode
ok    test_mode_flash
ok    test_collar_switch
ok    test_hid_report
ok    test_status_led
ok    test_debug_trace
ok    test_fast_gear
ok    test_replay
ok    test_hid_replay
ok    test_replay sequentielle_organique_log.txt
...
```

| Test | What it checks |
|---|---|
| `test_calibration_flash` | Slot rotation, CRC rejection, highest-sequence-wins, erase on wrap, a save interrupted by a power cut. Flash is simulated in RAM with real semantics (programming can only clear bits, erase sets 0xFF) |
| `test_calibration_mode` | Calibration mode end to end through the state machine: a session interrupted after 5 points changes nothing (RAM or flash); a complete one applies all nine points and writes exactly one flash slot, which reloads identically |
| `test_collar_switch` | Debounce (isolated bounce rejected, edges confirmed after 2 reads), and the GPIO read through a stub |
| `test_hid_report` | Slow-path report building: gear latch until neutral, reverse latch, shift pulses, debug byte, raw axes |
| `test_mode_flash` | Saved mode: blank page, no rewrite when unchanged, interrupted record, erase after 512 records, foreign page content |
| `test_status_led` | LED patterns: breath then *n* pulses for the expected calibration point, saved/ready/mode-switch flashes, software PWM duty |
| `test_debug_trace` | CDC line format: header and line column counts, `fast_pos`/`buttons` columns, `CAL` line content and timing |
| `test_fast_gear` | Fast path: engage/release timing, spike rejection, seq ± in SEQ mode only, no phantom shift + from a noisy neutral, seq − never engaging 3rd in H mode, 6th not sent after a mode switch, one pulse per push, two quick pushes → two pulses, calibration-entry mask, reference points following a recalibration. Also replays any `*_cdc.csv` given as arguments |
| `test_replay` | Replays a Phase-4 log (`T … HALL … COLLAR …` format) through the state machine, prints emitted movements and mode switches, and checks their counts |
| `test_hid_replay` | Same, through the slow-path HID report; counts changes of the button and debug bytes |

How host builds work: `calibration_store.c` depends on
`calibration_flash.c`, which calls the HAL flash API. Tests compile with
`-DCALIBRATION_FLASH_READ_OVERRIDE` and link `hal_stub/hal_flash_stub.c`,
which replaces the HAL with a RAM-backed flash; `hal_stub/main.h` stands in
for the firmware's `main.h`. Each test's exact build line is in its header
comment.

The replay tests are pinned to reference counts in `run_tests.sh` (for
example 25 movements and 0 switches on `H_pattern_organique_log.txt`, the
figures recorded during development). A change that moves one of them fails
the run; if the change is deliberate, update the count and explain it in the
commit message.

The bootloader and the installer have their own host tests:
`make -C Bootloader test` and `python -m unittest discover -s tests -t .` from
`Wizard/` (see [USB bootloader](bootloader.md#building-and-testing)).

## Replaying captures through the current firmware

### `replay_fast_path` — before/after on a real session

`Firmware/App/test/replay_fast_path.c` feeds a `*_cdc.csv` capture through
the same two-rate structure as `main.c`: the slow step runs on each captured
sample (100 ms), and the fast step runs every 1 ms on Hall values linearly
interpolated between samples. For every millisecond it writes:

```
t_host_s,t_ms,is_sample,mode,fast_pos,slow_buttons,new_buttons
```

`slow_buttons` is what the firmware sent before the fast path existed;
`new_buttons` is what it sends now. The initial mode is taken from the
capture's first line.

If a `<name>_cal.csv` sits next to `<name>_cdc.csv` (recorded by
`capture_session.py` since Sept. 2026), the replay starts with its first
calibration and applies later ones at their host time. Otherwise it uses the
default calibration.

Limits:
- interpolation between 100 ms samples is an approximation of what the board
  sees;
- older captures have no `_cal.csv`, so they replay with the **default**
  calibration; those recorded after a recalibration (`test_H_pattern4/6/7`,
  `test_H_pattern`, `test1`, `test_Ben`) replay unreliably. Before trusting
  such a capture, compare its replayed
  `slow_buttons` with the recorded `*_hid.csv`: they should match almost
  everywhere (99.7–99.9 % on `sync_assetto2`/`3`).

### `plot_fast_path.py` — the before/after figure

```sh
.venv/Scripts/python.exe tools/plot_fast_path.py
```

Compiles `replay_fast_path` with gcc, replays the captures listed in
`STATS_CAPTURES` and `EXAMPLES`, prints aggregate statistics and writes
`captures/fast_path_before_after.png`. Per-millisecond replays go to
`captures/fast_path_replay/` (≈47 MB, git-ignored, regenerated on every
run). To add an example, append a line to `EXAMPLES` (capture name,
approximate `t_ms` of the lever arrival, window, title, subtitle).

## Capturing a session

Set up the Python environment once (from the repository root):

```sh
python -m venv .venv
.venv/Scripts/pip install -r requirements.txt     # Windows
.venv/bin/pip install -r requirements.txt         # Linux/macOS
```

### `capture_session.py`

The main field tool. With the shifter plugged in (production firmware):

```sh
.venv/Scripts/python.exe tools/capture_session.py COM7 my_session
```

It records, all on one host clock, into `captures/`:

| File | Content |
|---|---|
| `my_session_hid.csv` | Every change of the HID report as the PC receives it (buttons + raw axes) |
| `my_session_cdc.csv` | Every debug line from the firmware, with a host timestamp prepended |
| `my_session_cal.csv` | The unit's active calibration, one row per distinct calibration seen (firmware from Sept. 2026) |
| `my_session_ac.csv` | Gear as seen by **Assetto Corsa** (shared memory), if the game is running |
| `my_session_pedals.csv` | AC throttle/brake/clutch, continuously |
| `my_session.png` | Combined plot: Hall values, collar, HID buttons, AC gear, pedals |

Ctrl+C stops the capture and draws the plot. The HID device is found by
VID/PID. The COM port is the *USB Serial Device* in Device Manager. AC
telemetry is optional: the script retries every loop, so the game can be
started after the capture.

This is how the "missed shift" investigation was done: firmware state, what
the PC received, and what the game accepted, side by side.

### `hall_diag_stream.py`

For the **diagnostic firmware** (`Diag/Gx100_DiagFirm`) only. It streams the
`T … HALL … COLLAR …` lines to a log and plots the six sensors and the collar:

```sh
.venv/Scripts/python.exe tools/hall_diag_stream.py COM9 my_log.txt
```

## Captures in this repository (`captures/`)

Only the captures the host tests replay are here:

| Captures | Content |
|---|---|
| `H_pattern_organique_log.txt`, `sequentielle_organique_log.txt` | Sensor mapping sessions (diagnostic firmware format) |
| `séquence_bascule_séquentielle_collerette_log.txt` | Mode-switch gesture recording |
| `sync_assetto2_cdc.csv`, `test_Seq_cdc.csv` | Production firmware sessions (fast path tests) |
| `test_Ben_cdc.csv`, `test_H_pattern8_cdc.csv`, `test_H_pattern9_cdc.csv` | Complete calibration sessions (all points accepted) |
| `calib_refus_cdc.csv` (+ `_cal.csv`) | Calibration session with mistakes (four must be refused) |

Every other capture recorded during development (sensor mapping, field tests,
in-game Assetto Corsa sessions with game gear and pedals, the stock device's
HID characterisation), the stock-device scripts and the Python prototypes the
C code was ported from are kept in the development archive (a separate repository: day-by-day journal `PLAN.md`, Python prototypes, every capture recorded during development).
`plot_fast_path.py` needs those captures to redraw its figure.
