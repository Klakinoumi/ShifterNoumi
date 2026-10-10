/**
 * replay_fast_path.c — rejoue une capture CDC (*_cdc.csv) a travers la
 * chaine complete de main.c (pas lent mode_state_machine + hid_report_build,
 * puis pas rapide fast_gear + hid_report_apply_fast_gear) et ecrit, pour
 * chaque echantillon, les boutons HID que le firmware corrige aurait
 * envoyes. Sert aux graphiques avant/apres (PCB/tools/plot_fast_path.py).
 *
 * Reproduit la structure de main.c : pas lent sur chaque echantillon reel
 * (~100ms, comme sur cible), pas rapide toutes les 1ms. Entre deux
 * echantillons de la capture, les valeurs Hall vues par le pas rapide sont
 * INTERPOLEES lineairement (la capture n'a pas mieux que 100ms ; sur cible
 * le pas rapide lit l'ADC reel a chaque ms). Une ligne de sortie par ms,
 * is_sample=1 sur les echantillons reels.
 *
 * Le mode initial (H/SEQ) est repris de la 1ere ligne de la capture (le
 * device pouvait deja etre en SEQ au debut de l'enregistrement).
 *
 * Calibration : si la capture a un fichier voisin <nom>_cal.csv (ecrit par
 * capture_session.py a partir des lignes CAL du firmware), sa 1ere ligne
 * est la calibration de depart et les suivantes sont appliquees a leur
 * t_host_s (recalibration pendant la session). Sinon : calibration par
 * defaut, comme avant (captures anterieures a sept. 2026).
 *
 * Build :
 *   gcc -std=c99 -Wall -Wextra -DCALIBRATION_FLASH_READ_OVERRIDE \
 *     -I ../Inc -I hal_stub replay_fast_path.c ../Src/fast_gear.c \
 *     ../Src/gear_position.c ../Src/calibration_store.c \
 *     ../Src/mode_state_machine.c ../Src/hid_report.c \
 *     ../Src/calibration_flash.c hal_stub/hal_flash_stub.c -lm -o replay_fast_path
 * Usage : replay_fast_path <capture_cdc.csv> <sortie.csv>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "calibration_store.h"
#include "fast_gear.h"
#include "hid_report.h"
#include "mode_state_machine.h"

static void buttons_str(const uint8_t *report, char *out, size_t n)
{
    out[0] = '\0';
    for (int b = 0; b < 16; b++) {
        if (report[b / 8] & (1u << (b % 8))) {
            char tmp[8];
            snprintf(tmp, sizeof tmp, "%s%d", out[0] ? " " : "", b + 1);
            strncat(out, tmp, n - strlen(out) - 1);
        }
    }
}

#define MAX_CAL_SNAPSHOTS 256

typedef struct {
    double t_host;
    hall_sample_t positions[GEAR_POSITION_COUNT];
} cal_snapshot_t;

static cal_snapshot_t g_cal[MAX_CAL_SNAPSHOTS];

/* Lit <nom>_cal.csv a cote de <nom>_cdc.csv. Retourne le nombre de
 * calibrations lues (0 si pas de fichier). */
static int load_cal_snapshots(const char *cdc_path)
{
    char path[1024];
    size_t n = strlen(cdc_path);
    const char *suffix = "_cdc.csv";
    size_t ls = strlen(suffix);
    if (n < ls || n >= sizeof path || strcmp(cdc_path + n - ls, suffix) != 0) {
        return 0;
    }
    memcpy(path, cdc_path, n - ls);
    strcpy(path + n - ls, "_cal.csv");
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return 0;
    }
    char line[1024];
    int count = 0;
    if (fgets(line, sizeof line, f) == NULL) { /* entete */
        fclose(f);
        return 0;
    }
    while (count < MAX_CAL_SNAPSHOTS && fgets(line, sizeof line, f) != NULL) {
        char *p = line;
        char *end;
        cal_snapshot_t *s = &g_cal[count];
        s->t_host = strtod(p, &end);
        if (end == p || *end != ',') {
            continue;
        }
        p = strchr(end + 1, ','); /* saute t_ms firmware */
        int k = 0;
        while (p != NULL && k < GEAR_POSITION_COUNT * HALL_SENSOR_COUNT) {
            unsigned long v = strtoul(p + 1, &end, 10);
            if (end == p + 1) {
                break;
            }
            s->positions[k / HALL_SENSOR_COUNT].v[k % HALL_SENSOR_COUNT] = (uint16_t)v;
            k++;
            p = (*end == ',') ? end : NULL;
        }
        if (k == GEAR_POSITION_COUNT * HALL_SENSOR_COUNT) {
            count++;
        }
    }
    fclose(f);
    printf("%s : %d calibration(s)\n", path, count);
    return count;
}

static void apply_cal(gear_calibration_t *cal, const cal_snapshot_t *s)
{
    memcpy(cal->positions, s->positions, sizeof(cal->positions));
    gear_position_compute_normalization(cal);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage : replay_fast_path <capture_cdc.csv> <sortie.csv>\n");
        return 2;
    }
    FILE *in = fopen(argv[1], "r");
    FILE *out = fopen(argv[2], "w");
    if (in == NULL || out == NULL) {
        printf("fichier inaccessible\n");
        return 1;
    }

    gear_calibration_t cal;
    calibration_store_defaults(&cal);
    int cal_count = load_cal_snapshots(argv[1]);
    int cal_next = 0;
    if (cal_count > 0) {
        apply_cal(&cal, &g_cal[0]);
        cal_next = 1;
    }
    mode_state_machine_t msm;
    mode_state_machine_init(&msm, &cal);
    hid_report_state_t rep;
    hid_report_init(&rep);
    fast_gear_t fg;
    fast_gear_init(&fg);

    fprintf(out, "t_host_s,t_ms,is_sample,mode,fast_pos,slow_buttons,new_buttons\n");
    char line[512];
    fgets(line, sizeof line, in); /* entete */
    bool first = true;
    double prev_host = 0.0;
    uint32_t prev_t = 0;
    hall_sample_t prev_hall = {{0}};
    char slow[64] = "";
    while (fgets(line, sizeof line, in) != NULL) {
        double t_host;
        unsigned long tl;
        unsigned int h[HALL_SENSOR_COUNT];
        int collar;
        char rest[256] = {0};
        if (sscanf(line, "%lf,%lu,%u,%u,%u,%u,%u,%u,%d,%255[^\r\n]", &t_host, &tl, &h[0], &h[1], &h[2], &h[3], &h[4],
                   &h[5], &collar, rest) < 9) {
            continue;
        }
        while (cal_next < cal_count && g_cal[cal_next].t_host <= t_host) {
            apply_cal(&msm.calibration, &g_cal[cal_next++]);
        }
        uint32_t t = (uint32_t)tl;
        hall_sample_t hall;
        for (int i = 0; i < HALL_SENSOR_COUNT; i++) {
            hall.v[i] = (uint16_t)h[i];
        }
        if (first) {
            /* rest = tracker_state,mode,... */
            char *mode = strchr(rest, ',');
            if (mode != NULL && strncmp(mode + 1, "SEQ", 3) == 0) {
                msm.mode_courant = GEAR_MODE_SEQ;
            }
            prev_t = t - 1;
            prev_host = t_host - 0.001;
            prev_hall = hall;
            first = false;
        }

        /* Boucle rapide 1ms entre deux echantillons : Hall interpole
         * lineairement ; le pas lent ne tourne que sur l'echantillon reel
         * (meme cadence 100ms que sur cible). */
        uint32_t span = t - prev_t;
        for (uint32_t k = 1; k <= span; k++) {
            uint32_t tk = prev_t + k;
            float f = (float)k / (float)span;
            hall_sample_t hk;
            for (int i = 0; i < HALL_SENSOR_COUNT; i++) {
                hk.v[i] = (uint16_t)((float)prev_hall.v[i] + ((float)hall.v[i] - (float)prev_hall.v[i]) * f + 0.5f);
            }
            bool is_sample = (k == span);
            if (is_sample) {
                /* Pas lent seul = firmware precedent (ce qui partait en USB). */
                mode_state_machine_step(&msm, tk, &hk, collar);
                hid_report_build(&rep, &msm, collar, &hk);
                buttons_str(rep.report, slow, sizeof slow);
            }
            /* Pas rapide = firmware corrige. */
            gear_position_t g = fast_gear_step(&fg, tk, &hk, &msm.calibration, msm.mode_courant);
            hid_report_apply_fast_gear(&rep, &msm, collar, g, &hk, tk);
            char fresh[64];
            buttons_str(rep.report, fresh, sizeof fresh);

            fprintf(out, "%.4f,%lu,%d,%s,%s,%s,%s\n", prev_host + (t_host - prev_host) * f, (unsigned long)tk,
                    is_sample ? 1 : 0, msm.mode_courant == GEAR_MODE_SEQ ? "SEQ" : "H",
                    g == GEAR_NONE ? "" : gear_position_name(g), slow, fresh);
        }
        prev_t = t;
        prev_host = t_host;
        prev_hall = hall;
    }
    fclose(in);
    fclose(out);
    return 0;
}
