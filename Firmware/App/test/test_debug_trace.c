/**
 * test_debug_trace.c — format des lignes CDC (debug_trace.c compile avec
 * DEBUG_TRACE_HOST_TEST : la derniere ligne envoyee est capturee dans
 * debug_trace_host_buffer au lieu de partir sur le port CDC).
 *
 * Build : gcc -std=c99 -Wall -Wextra -DDEBUG_TRACE_HOST_TEST \
 *   -DCALIBRATION_FLASH_READ_OVERRIDE -I ../Inc -I hal_stub \
 *   test_debug_trace.c ../Src/debug_trace.c ../Src/gear_position.c \
 *   ../Src/calibration_store.c ../Src/mode_state_machine.c \
 *   ../Src/calibration_flash.c hal_stub/hal_flash_stub.c -lm -o test_debug_trace
 */
#include <stdio.h>
#include <string.h>

#include "calibration_store.h"
#include "debug_trace.h"
#include "hid_report.h"

extern char debug_trace_host_buffer[];

static int failures = 0;

#define CHECK(cond, msg)                                                                                             \
    do {                                                                                                             \
        if (!(cond)) {                                                                                               \
            printf("FAIL: %s (%s:%d)\n  line: %s\n", msg, __FILE__, __LINE__, debug_trace_host_buffer);           \
            failures++;                                                                                              \
        }                                                                                                            \
    } while (0)

static int count_fields(const char *line)
{
    int n = 1;
    for (; *line; line++) {
        n += (*line == ',');
    }
    return n;
}

static bool ends_with(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    return ls >= lx && strcmp(s + ls - lx, suffix) == 0;
}

int main(void)
{
    debug_trace_init();
    CHECK(ends_with(debug_trace_host_buffer, ",calib_target,fast_pos,buttons\r\n"), "entete : colonnes ajoutees");
    int header_fields = count_fields(debug_trace_host_buffer);

    gear_calibration_t cal;
    calibration_store_defaults(&cal);
    mode_state_machine_t msm;
    mode_state_machine_init(&msm, &cal);
    uint8_t report[HID_REPORT_SIZE] = {0};
    report[0] = 1u << HID_BUTTON_GEAR2;
    report[HID_DEBUG_BYTE_INDEX] = 1;

    debug_trace_step(1234, &cal.positions[GEAR_2], 0, &msm, report, GEAR_2);
    CHECK(strncmp(debug_trace_host_buffer, "1234,", 5) == 0, "ligne : t_ms en tete");
    CHECK(ends_with(debug_trace_host_buffer, ",gear2,2\r\n"), "ligne : fast_pos et buttons en fin");
    CHECK(count_fields(debug_trace_host_buffer) == header_fields, "ligne : autant de colonnes que l'entete");

    debug_trace_step(1334, &cal.positions[GEAR_NEUTRAL], 0, &msm, (const uint8_t[HID_REPORT_SIZE]){0}, GEAR_NONE);
    CHECK(ends_with(debug_trace_host_buffer, ",,0\r\n"), "ligne : fast_pos vide si aucune position");

    /* Ligne CAL : premiere envoyee tout de suite, puis periodique. */
    debug_trace_calibration_poll(500, &cal);
    CHECK(strncmp(debug_trace_host_buffer, "CAL,500,", 8) == 0, "CAL envoyee au premier appel");
    CHECK(count_fields(debug_trace_host_buffer) == 2 + GEAR_POSITION_COUNT * HALL_SENSOR_COUNT, "CAL : 54 valeurs");
    char expect[16];
    snprintf(expect, sizeof expect, ",%u\r\n", (unsigned)cal.positions[GEAR_SEQ_DOWN].v[HALL_SENSOR_COUNT - 1]);
    CHECK(ends_with(debug_trace_host_buffer, expect), "CAL : derniere valeur = seq_down u7");

    debug_trace_host_buffer[0] = '\0';
    debug_trace_calibration_poll(500 + DEBUG_TRACE_CAL_PERIOD_MS - 1, &cal);
    CHECK(debug_trace_host_buffer[0] == '\0', "CAL : rien avant la periode");

    cal.positions[GEAR_3].v[0] = 1111;
    debug_trace_calibration_poll(600, &cal);
    CHECK(strncmp(debug_trace_host_buffer, "CAL,600,", 8) == 0, "CAL : renvoyee aussitot apres un changement");

    debug_trace_host_buffer[0] = '\0';
    debug_trace_calibration_poll(600 + DEBUG_TRACE_CAL_PERIOD_MS, &cal);
    CHECK(debug_trace_host_buffer[0] != '\0', "CAL : renvoyee apres la periode");

    if (failures == 0) {
        printf("OK: tous les tests debug_trace ont reussi.\n");
    } else {
        printf("%d test(s) en echec.\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
