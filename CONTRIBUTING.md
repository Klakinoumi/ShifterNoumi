# Contributing

Thanks for helping. This project runs on real hardware that is tedious to
take apart, so the workflow favours **recording once, replaying many times**.

## Ground rules

- **Behaviour changes come with evidence.** A change to classification,
  timing or gestures should be backed by a capture (`capture_session.py`)
  replayed before and after (`replay_fast_path`, `plot_fast_path.py`).
  Commit new captures to `captures/` with a descriptive name.
- **Keep `App/` HAL-free.** Only `hall_sensors`, `collar_switch`,
  `status_led`, `boot_link`, `calibration_flash` and `mode_flash` (writes) and `debug_trace` may touch the
  hardware. New logic goes in a pure module with a host test.
- **Don't retune slow-path constants casually.** Every slow-path timing was
  tuned on real 100 ms captures. The history of each value is in
  [Configuration reference](docs/configuration-reference.md) and
  [Design history](docs/design-history.md).
- **Mark edits to generated files** with a `GX100` comment, and add the file to
  the regeneration table in
  [Building and flashing](docs/building-and-flashing.md#regenerating-with-cubemx).

## Before opening a pull request

```sh
sh Firmware/App/test/run_tests.sh      # all host tests must pass
make -C Bootloader test                # bootloader protocol
(cd Wizard && python -m unittest discover -s tests -t .)   # installer
```

Then build the Debug configuration in STM32CubeIDE; it must compile without
new warnings. If you changed anything the driver notices (buttons, gestures,
timings, LED), say whether you tested it on hardware, and with which game.

## Code conventions

- C99, 4-space indentation, `snake_case`, module prefix on public symbols
  (`fast_gear_step`, `hid_report_build`).
- Constants are `#define`s in the module's header, each with a comment
  saying where its value comes from.
- Times are `uint32_t` milliseconds from `HAL_GetTick()`, compared as
  differences (`t_ms - start >= X`) so they survive the counter wrap.
- No dynamic allocation.
- Existing comments are in French. New comments may be in English or French;
  documentation under `docs/` is in English.

## Reporting a problem

Please attach:

1. what you did and what you expected;
2. a `capture_session.py` recording of it (the `_hid.csv`, `_cdc.csv` and
   `.png` files);
3. whether the unit has been recalibrated since flashing.
