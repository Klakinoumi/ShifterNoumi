#!/usr/bin/env sh
# Builds and runs every host test of Firmware/App with gcc (Linux, macOS, or
# Git Bash/MinGW on Windows). Usage, from anywhere:
#   sh Firmware/App/test/run_tests.sh
# Exit code 0 only if every test builds and passes.
set -u
cd "$(dirname "$0")"
OUT="${TMPDIR:-/tmp}/gx100_tests"
mkdir -p "$OUT"

CFLAGS="-std=c99 -Wall -Wextra -DCALIBRATION_FLASH_READ_OVERRIDE -I ../Inc -I hal_stub"
CORE="../Src/gear_position.c ../Src/calibration_store.c ../Src/mode_state_machine.c ../Src/calibration_flash.c hal_stub/hal_flash_stub.c"
CAPTURES="../../../captures"
failed=0

run() { # name, sources, args...
    name=$1; srcs=$2; shift 2
    if ! gcc $CFLAGS $srcs -lm -o "$OUT/$name" 2>"$OUT/$name.build.log"; then
        echo "FAIL  $name (build, see $OUT/$name.build.log)"; failed=1; return
    fi
    if "$OUT/$name" "$@" >"$OUT/$name.log" 2>&1; then
        echo "ok    $name"
    else
        echo "FAIL  $name (see $OUT/$name.log)"; failed=1
    fi
}

run test_calibration_flash "test_calibration_flash.c ../Src/calibration_flash.c"
run test_calibration_mode  "test_calibration_mode.c ../Src/gear_position.c ../Src/calibration_store.c ../Src/mode_state_machine.c ../Src/calibration_flash.c" \
                           "$CAPTURES/test_Ben_cdc.csv" "$CAPTURES/test_H_pattern8_cdc.csv" "$CAPTURES/test_H_pattern9_cdc.csv" "$CAPTURES/calib_refus_cdc.csv:9:4"
run test_mode_flash       "test_mode_flash.c ../Src/mode_flash.c"
run test_collar_switch    "test_collar_switch.c ../Src/collar_switch.c hal_stub/hal_gpio_stub.c"
run test_hid_report        "test_hid_report.c ../Src/hid_report.c $CORE"
run test_status_led        "test_status_led.c ../Src/status_led.c hal_stub/hal_gpio_stub.c $CORE"
BASE_CFLAGS=$CFLAGS; CFLAGS="$CFLAGS -DDEBUG_TRACE_HOST_TEST"
run test_debug_trace       "test_debug_trace.c ../Src/debug_trace.c $CORE"
CFLAGS=$BASE_CFLAGS
run test_fast_gear         "test_fast_gear.c ../Src/fast_gear.c ../Src/hid_report.c $CORE" \
                           "$CAPTURES/sync_assetto2_cdc.csv" "$CAPTURES/test_Seq_cdc.csv"
# Slow-path replays of real captures, pinned to reference counts
# (moves emitted, mode switches / button changes). Moves and switches come
# from the development journal; if a deliberate change moves them, update
# them here and say why in the commit message.
replay() { # name, log, expected counts...
    tname=$1; log=$2; shift 2
    if "$OUT/$tname" "$CAPTURES/$log" "$@" >"$OUT/$tname.log" 2>&1; then
        echo "ok    $tname $log"
    else
        echo "FAIL  $tname $log (see $OUT/$tname.log)"; failed=1
    fi
}
run test_replay            "test_replay.c $CORE" "$CAPTURES/H_pattern_organique_log.txt" 25 0
run test_hid_replay        "test_hid_replay.c ../Src/hid_report.c $CORE" "$CAPTURES/sequentielle_organique_log.txt" 6
if [ -x "$OUT/test_replay" ] && [ -x "$OUT/test_hid_replay" ]; then
    replay test_replay     "sequentielle_organique_log.txt" 3 0
    replay test_replay     "séquence_bascule_séquentielle_collerette_log.txt" 6 1
    replay test_hid_replay "H_pattern_organique_log.txt" 36
    replay test_hid_replay "séquence_bascule_séquentielle_collerette_log.txt" 13
fi

exit $failed
