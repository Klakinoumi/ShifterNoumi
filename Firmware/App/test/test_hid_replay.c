/**
 * test_hid_replay.c — rejoue un log de capture a travers
 * mode_state_machine + hid_report, affiche chaque changement des boutons
 * (octets 0-1) ou de l'octet debug du rapport HID. Les octets axes (Hall
 * brut) changent a chaque echantillon et sont ignores ici.
 *
 * Build (voir test_replay.c pour la note sur calibration_flash) :
 *   gcc -std=c99 -Wall -Wextra -DCALIBRATION_FLASH_READ_OVERRIDE \
 *     -I ../Inc -I hal_stub test_hid_replay.c ../Src/gear_position.c \
 *     ../Src/calibration_store.c ../Src/mode_state_machine.c \
 *     ../Src/hid_report.c ../Src/calibration_flash.c \
 *     hal_stub/hal_flash_stub.c -lm -o test_hid_replay
 * Usage : test_hid_replay <log.txt> [changements_attendus]
 * Avec le decompte attendu, echoue (code 1) s'il differe.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "calibration_store.h"
#include "hid_report.h"
#include "mode_state_machine.h"

static int parse_line(const char *line, uint32_t *t_ms, hall_sample_t *hall, int *collar)
{
    unsigned long t;
    unsigned int h[HALL_SENSOR_COUNT];
    int c;
    const char *p = strstr(line, "T ");
    while (p != NULL) {
        int n = sscanf(p, "T %lu HALL %u %u %u %u %u %u COLLAR %d", &t, &h[0], &h[1], &h[2], &h[3], &h[4], &h[5], &c);
        if (n == 8) {
            *t_ms = (uint32_t)t;
            for (int i = 0; i < HALL_SENSOR_COUNT; i++) {
                hall->v[i] = (uint16_t)h[i];
            }
            *collar = c;
            return 1;
        }
        p = strstr(p + 1, "T ");
    }
    return 0;
}

static char *decode_utf16le_to_ascii(const unsigned char *raw, long size, long *out_len)
{
    long start = (size >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) ? 2 : 0;
    long n = (size - start) / 2;
    char *out = malloc((size_t)n + 1);
    if (!out) {
        return NULL;
    }
    for (long i = 0; i < n; i++) {
        out[i] = (char)raw[start + i * 2];
    }
    out[n] = '\0';
    *out_len = n;
    return out;
}

static void print_buttons(const uint8_t *report)
{
    uint16_t buttons = (uint16_t)(report[0] | (report[1] << 8));
    int first = 1;
    for (int i = 0; i < 16; i++) {
        if (buttons & (1u << i)) {
            printf("%s%d", first ? "" : ",", i + 1);
            first = 0;
        }
    }
    if (first) {
        printf("-");
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <log.txt>\n", argv[0]);
        return 1;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror("fopen");
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *raw = malloc((size_t)fsize);
    if (fsize > 0 && raw != NULL) {
        size_t r = fread(raw, 1, (size_t)fsize, f);
        (void)r;
    }
    fclose(f);

    char *text = NULL;
    if (fsize >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        long len;
        text = decode_utf16le_to_ascii(raw, fsize, &len);
        free(raw);
    } else {
        text = malloc((size_t)fsize + 1);
        if (text != NULL && fsize > 0) {
            memcpy(text, raw, (size_t)fsize);
        }
        if (text != NULL) {
            text[fsize] = '\0';
        }
        free(raw);
    }
    if (!text) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    gear_calibration_t calibration;
    calibration_store_defaults(&calibration);

    mode_state_machine_t msm;
    mode_state_machine_init(&msm, &calibration);

    hid_report_state_t rep;
    hid_report_init(&rep);

    uint8_t prev_buttons[2] = {0, 0};
    uint8_t prev_debug = 0;

    int samples = 0;
    int changes = 0;

    char *line = strtok(text, "\r\n");
    while (line != NULL) {
        uint32_t t_ms;
        hall_sample_t hall;
        int collar;
        if (parse_line(line, &t_ms, &hall, &collar)) {
            samples++;
            mode_state_machine_step(&msm, t_ms, &hall, collar);
            hid_report_build(&rep, &msm, collar, &hall);

            if (memcmp(rep.report, prev_buttons, sizeof(prev_buttons)) != 0 ||
                rep.report[HID_DEBUG_BYTE_INDEX] != prev_debug) {
                printf("t=%10u buttons=", t_ms);
                print_buttons(rep.report);
                printf(" debug=0x%02X\n", rep.report[HID_DEBUG_BYTE_INDEX]);
                memcpy(prev_buttons, rep.report, sizeof(prev_buttons));
                prev_debug = rep.report[HID_DEBUG_BYTE_INDEX];
                changes++;
            }
        }
        line = strtok(NULL, "\r\n");
    }

    free(text);
    printf("\n%d echantillons, %d changement(s) de rapport HID\n", samples, changes);

    if (samples == 0) {
        printf("FAIL: aucun echantillon lu\n");
        return 1;
    }
    if (argc >= 3 && changes != atoi(argv[2])) {
        printf("FAIL: attendu %s changement(s)\n", argv[2]);
        return 1;
    }
    return 0;
}
