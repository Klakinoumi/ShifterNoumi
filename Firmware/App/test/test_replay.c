/**
 * test_replay.c — rejoue un log de capture (meme format que
 * PCB/mode_state_machine.py) a travers le portage C de mode_state_machine,
 * et affiche les memes evenements que `python mode_state_machine.py <log>`
 * (mouvements emis, bascules, recalibrations) pour comparaison directe.
 *
 * Build (hors cible, gcc/mingw) — depuis l'introduction de la persistance
 * flash (calibration_store.c depend maintenant de calibration_flash.c,
 * qui depend du HAL) : lie aussi calibration_flash.c et le stub
 * hal_stub/hal_flash_stub.c, compile avec -DCALIBRATION_FLASH_READ_OVERRIDE
 * (ce test ne verifie pas la persistance flash elle-meme, juste la logique
 * mode_state_machine sur les valeurs par defaut — voir
 * test_calibration_flash.c pour les tests de persistance) :
 *   gcc -std=c99 -Wall -Wextra -DCALIBRATION_FLASH_READ_OVERRIDE \
 *       -I ../Inc -I hal_stub test_replay.c ../Src/gear_position.c \
 *       ../Src/calibration_store.c ../Src/mode_state_machine.c \
 *       ../Src/calibration_flash.c hal_stub/hal_flash_stub.c -lm -o test_replay
 *
 * Usage : test_replay <chemin_log.txt> [mouvements_attendus bascules_attendues]
 * Avec les deux decomptes attendus, echoue (code 1) s'ils different — c'est
 * ainsi que run_tests.sh l'utilise, avec les valeurs de reference de PLAN.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gear_position.h"
#include "calibration_store.h"
#include "mode_state_machine.h"

static int parse_line(const char *line, uint32_t *t_ms, hall_sample_t *hall, int *collar)
{
    unsigned long t;
    unsigned int h[HALL_SENSOR_COUNT];
    int c;
    /* Format : "T <ms> HALL <u2> <u3> <u4> <u5> <u6> <u7> COLLAR <0|1>",
     * precede d'un prefixe variable ("[HH:MM:SS] " ou rien) — cherche le
     * motif n'importe ou dans la ligne, comme re.search() cote Python
     * (mode_state_machine.py LINE_RE), plutot que d'exiger qu'il soit en
     * debut de ligne. */
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

static const char *kind_name(movement_kind_t kind)
{
    switch (kind) {
    case MOVEMENT_KIND_SEQ:
        return "SEQ";
    case MOVEMENT_KIND_HELD:
        return "HELD";
    case MOVEMENT_KIND_HELD_CORRECTED:
        return "HELD_CORRECTED";
    default:
        return "NONE";
    }
}

/* Les logs de capture existants sont parfois en UTF-16LE avec BOM (artefact
 * du script de capture PowerShell/Python, cf PCB/mode_state_machine.py
 * parse_log()) — le firmware reel ne lira jamais ce format (ADC/USB direct),
 * ce decodage n'existe que pour rejouer ces logs de test. Convertit en
 * ASCII/UTF-8 approx en supprimant l'octet haut de chaque code unit (les
 * logs ne contiennent que des caracteres ASCII, un accent eventuel dans un
 * nom de fichier n'apparait pas dans le contenu). */
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
        size_t read = fread(raw, 1, (size_t)fsize, f);
        (void)read;
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

    int emitted_count = 0;
    int switch_count = 0;
    int samples = 0;

    char *line = strtok(text, "\r\n");
    while (line != NULL) {
        uint32_t t_ms;
        hall_sample_t hall;
        int collar;
        if (parse_line(line, &t_ms, &hall, &collar)) {
            samples++;

            mode_state_machine_step(&msm, t_ms, &hall, collar);

            if (msm.last_emitted.pos != GEAR_NONE) {
                printf("EMIT   t=%10u pos=%-10s kind=%s\n", t_ms, gear_position_name(msm.last_emitted.pos),
                       kind_name(msm.last_emitted.kind));
                emitted_count++;
            }
            if (msm.last_switch_applied) {
                printf("SWITCH t=%10u -> %s\n", t_ms, msm.last_switch_target == GESTURE_TARGET_H ? "H" : "SEQ");
                switch_count++;
            }
        }
        line = strtok(NULL, "\r\n");
    }

    free(text);

    printf("\n%d echantillons, %d mouvement(s) emis, %d bascule(s)\n", samples, emitted_count, switch_count);

    if (samples == 0) {
        printf("FAIL: aucun echantillon lu\n");
        return 1;
    }
    if (argc >= 4) {
        int want_emitted = atoi(argv[2]);
        int want_switches = atoi(argv[3]);
        if (emitted_count != want_emitted || switch_count != want_switches) {
            printf("FAIL: attendu %d mouvement(s), %d bascule(s)\n", want_emitted, want_switches);
            return 1;
        }
    }
    return 0;
}
