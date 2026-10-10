/**
 * test_fast_gear.c — tests synthetiques de fast_gear + chemin rapide de
 * hid_report, puis (optionnel) rejeu d'une capture CDC (*_cdc.csv) pour
 * comparer l'instant d'engagement rapide a l'instant ou le bouton partait
 * avec l'ancien chemin (colonnes emit_pos/emit_kind de la capture).
 *
 * Note : les captures sont a 100ms/echantillon, le gain mesure ici est donc
 * une borne basse (sur cible, fast_gear tourne a ~1ms).
 *
 * Build :
 *   gcc -std=c99 -Wall -Wextra -DCALIBRATION_FLASH_READ_OVERRIDE \
 *     -I ../Inc -I hal_stub test_fast_gear.c ../Src/fast_gear.c \
 *     ../Src/gear_position.c ../Src/calibration_store.c \
 *     ../Src/mode_state_machine.c ../Src/hid_report.c \
 *     ../Src/calibration_flash.c hal_stub/hal_flash_stub.c -lm -o test_fast_gear
 * Usage : test_fast_gear [capture_cdc.csv]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "calibration_store.h"
#include "fast_gear.h"
#include "hid_report.h"
#include "mode_state_machine.h"

static int failures = 0;

#define CHECK(cond, msg)                                                                                             \
    do {                                                                                                             \
        if (!(cond)) {                                                                                               \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                                                   \
            failures++;                                                                                              \
        }                                                                                                            \
    } while (0)

static bool bit_set(const uint8_t *report, int bit_index)
{
    return (report[bit_index / 8] & (1u << (bit_index % 8))) != 0;
}

/* Interpolation lineaire neutre -> position (f=0 neutre, f=1 point calibre). */
static hall_sample_t lerp(const gear_calibration_t *c, gear_position_t to, float f)
{
    hall_sample_t out;
    for (int i = 0; i < HALL_SENSOR_COUNT; i++) {
        float a = c->positions[GEAR_NEUTRAL].v[i];
        float b = c->positions[to].v[i];
        out.v[i] = (uint16_t)(a + (b - a) * f);
    }
    return out;
}

static void synthetic_tests(const gear_calibration_t *cal)
{
    fast_gear_t fg;
    fast_gear_init(&fg);
    uint32_t t = 0;

    /* Neutre : rien. */
    for (int i = 0; i < 20; i++, t++) {
        CHECK(fast_gear_step(&fg, t, &cal->positions[GEAR_NEUTRAL], cal, GEAR_MODE_H) == GEAR_NONE, "neutre -> aucun rapport");
    }

    /* Montee 3eme en 60ms puis maintien : engage des l'entree en grille +
     * FAST_GEAR_ENGAGE_MS, bien avant les 350ms+ de l'ancien chemin. */
    uint32_t t_engaged = 0;
    for (int i = 0; i <= 60; i++, t++) {
        hall_sample_t h = lerp(cal, GEAR_3, (float)i / 60.0f);
        if (fast_gear_step(&fg, t, &h, cal, GEAR_MODE_H) == GEAR_3 && t_engaged == 0) {
            t_engaged = t;
        }
    }
    for (int i = 0; i < 20; i++, t++) {
        if (fast_gear_step(&fg, t, &cal->positions[GEAR_3], cal, GEAR_MODE_H) == GEAR_3 && t_engaged == 0) {
            t_engaged = t;
        }
    }
    CHECK(t_engaged != 0, "3eme engagee");
    CHECK(t_engaged - 20 <= 60 + FAST_GEAR_ENGAGE_MS, "3eme engagee au plus tard FAST_GEAR_ENGAGE_MS apres arrivee");

    /* Pic isole de 2ms vers la 4eme : ignore. */
    for (int i = 0; i < 2; i++, t++) {
        fast_gear_step(&fg, t, &cal->positions[GEAR_4], cal, GEAR_MODE_H);
    }
    CHECK(fg.engaged == GEAR_3, "pic bref vers 4eme ignore");

    /* Sortie de grille vers neutre : relache apres FAST_GEAR_RELEASE_MS. */
    gear_position_t g = GEAR_3;
    uint32_t t_out = t;
    while (g != GEAR_NONE && t - t_out < 200) {
        g = fast_gear_step(&fg, t, &cal->positions[GEAR_NEUTRAL], cal, GEAR_MODE_H);
        t++;
    }
    CHECK(g == GEAR_NONE, "retour neutre relache le rapport");
    CHECK(t - t_out <= FAST_GEAR_RELEASE_MS + 2, "relachement rapide");

    /* seq_down (a 0.30 de gear3) en mode H, ex. geste de bascule : jamais
     * gear3 (seq_down est la position la plus proche), jamais seq_down. */
    fast_gear_init(&fg);
    for (int i = 0; i < 20; i++, t++) {
        g = fast_gear_step(&fg, t, &cal->positions[GEAR_SEQ_DOWN], cal, GEAR_MODE_H);
        CHECK(g == GEAR_NONE, "seq_down n'engage rien en mode H");
    }

    /* Mode SEQ : un point H loin de tout point SEQ (gear1) n'engage rien ;
     * seq_up/seq_down engagent. (gear3 n'est pas teste : en SEQ, levier
     * verrouille, une lecture proche de gear3 EST un seq_down appuye.) */
    fast_gear_init(&fg);
    for (int i = 0; i < 20; i++, t++) {
        CHECK(fast_gear_step(&fg, t, &cal->positions[GEAR_1], cal, GEAR_MODE_SEQ) == GEAR_NONE,
              "gear1 n'engage rien en mode SEQ");
    }
    for (int i = 0; i < 20; i++, t++) {
        g = fast_gear_step(&fg, t, &cal->positions[GEAR_SEQ_UP], cal, GEAR_MODE_SEQ);
    }
    CHECK(g == GEAR_SEQ_UP, "seq_up engage en mode SEQ");
    for (int i = 0; i < 20; i++, t++) {
        g = fast_gear_step(&fg, t, &cal->positions[GEAR_NEUTRAL], cal, GEAR_MODE_SEQ);
    }
    CHECK(g == GEAR_NONE, "retour neutre relache seq_up (course courte, 0.46 du neutre)");

    /* Neutre bruite vers seq_up (a mi-chemin, 0.23 du neutre) : rien. */
    for (int i = 0; i < 50; i++, t++) {
        hall_sample_t h = lerp(cal, GEAR_SEQ_UP, 0.5f);
        CHECK(fast_gear_step(&fg, t, &h, cal, GEAR_MODE_SEQ) == GEAR_NONE, "neutre bruite : pas de shift fantome");
    }

    /* Bascule SEQ->H en maintenant gear6 (geste de bascule) : rien tant que
     * le levier n'est pas repasse par le neutre, puis fonctionnement normal. */
    fast_gear_init(&fg);
    fast_gear_step(&fg, t++, &cal->positions[GEAR_NEUTRAL], cal, GEAR_MODE_SEQ);
    for (int i = 0; i < 50; i++, t++) {
        CHECK(fast_gear_step(&fg, t, &cal->positions[GEAR_6], cal, GEAR_MODE_H) == GEAR_NONE,
              "gear6 du geste de bascule jamais transmis");
    }
    for (int i = 0; i < 5; i++, t++) {
        fast_gear_step(&fg, t, &cal->positions[GEAR_NEUTRAL], cal, GEAR_MODE_H);
    }
    for (int i = 0; i < 20; i++, t++) {
        g = fast_gear_step(&fg, t, &cal->positions[GEAR_6], cal, GEAR_MODE_H);
    }
    CHECK(g == GEAR_6, "gear6 transmis apres passage au neutre");

    /* Demarrage levier deja en rapport : pas d'attente du neutre. */
    fast_gear_init(&fg);
    for (int i = 0; i < 20; i++, t++) {
        g = fast_gear_step(&fg, t, &cal->positions[GEAR_2], cal, GEAR_MODE_H);
    }
    CHECK(g == GEAR_2, "demarrage en rapport : engage directement");

    /* Mi-chemin gear3/seq_down (geste de bascule H->SEQ) : rien. */
    fast_gear_init(&fg);
    for (int i = 0; i < 20; i++, t++) {
        hall_sample_t h;
        for (int k = 0; k < HALL_SENSOR_COUNT; k++) {
            h.v[k] = (uint16_t)((cal->positions[GEAR_3].v[k] + cal->positions[GEAR_SEQ_DOWN].v[k]) / 2);
        }
        CHECK(fast_gear_step(&fg, t, &h, cal, GEAR_MODE_H) == GEAR_NONE, "mi-chemin gear3/seq_down ambigu : rien");
    }

    /* Recalibration en cours de route (mode calibration) : les points de
     * reference mis en cache doivent suivre. On echange gear2 et gear4 ;
     * le levier a l'ancien point gear4 doit alors donner gear2. */
    gear_calibration_t recal = *cal;
    hall_sample_t old2 = recal.positions[GEAR_2];
    hall_sample_t old4 = recal.positions[GEAR_4];
    fast_gear_init(&fg);
    for (int i = 0; i < 20; i++, t++) {
        g = fast_gear_step(&fg, t, &old4, &recal, GEAR_MODE_H);
    }
    CHECK(g == GEAR_4, "avant recalibration : point gear4 -> gear4");
    hall_sample_t swapped[GEAR_POSITION_COUNT];
    memcpy(swapped, recal.positions, sizeof(swapped));
    swapped[GEAR_2] = old4;
    swapped[GEAR_4] = old2;
    calibration_store_commit(&recal, swapped);
    for (int i = 0; i < 20; i++, t++) {
        g = fast_gear_step(&fg, t, &recal.positions[GEAR_NEUTRAL], &recal, GEAR_MODE_H);
    }
    for (int i = 0; i < 20; i++, t++) {
        g = fast_gear_step(&fg, t, &old4, &recal, GEAR_MODE_H);
    }
    CHECK(g == GEAR_2, "apres recalibration : cache des points de reference rafraichi");

    /* --- hid_report_apply_fast_gear --- */
    mode_state_machine_t msm;
    mode_state_machine_init(&msm, cal);
    hid_report_state_t rep;
    hid_report_init(&rep);
    hall_sample_t hall = cal->positions[GEAR_NEUTRAL];

    hid_report_apply_fast_gear(&rep, &msm, 0, GEAR_2, &hall, 0);
    CHECK(bit_set(rep.report, HID_BUTTON_GEAR2), "mode H : bouton 2 immediat");

    /* L'ancien chemin annonce encore gear2 tenu alors que le rapide est
     * deja revenu au neutre : le rapide l'emporte. */
    msm.last_emitted.pos = GEAR_2;
    msm.last_emitted.kind = MOVEMENT_KIND_HELD;
    msm.tracker.state = TRACKER_HELD;
    hid_report_build(&rep, &msm, 0, &hall);
    hid_report_apply_fast_gear(&rep, &msm, 0, GEAR_NONE, &hall, 0);
    CHECK(!bit_set(rep.report, HID_BUTTON_GEAR2), "mode H : bouton lent masque par le chemin rapide");

    /* Marche arriere : collerette levee a l'entree en 1ere. */
    hid_report_apply_fast_gear(&rep, &msm, 1, GEAR_1, &hall, 0);
    CHECK(bit_set(rep.report, HID_BUTTON_REVERSE) && !bit_set(rep.report, HID_BUTTON_GEAR1), "R latchee");
    hid_report_apply_fast_gear(&rep, &msm, 0, GEAR_1, &hall, 0);
    CHECK(bit_set(rep.report, HID_BUTTON_REVERSE), "R reste latchee si collerette relachee en 1ere");
    hid_report_apply_fast_gear(&rep, &msm, 0, GEAR_NONE, &hall, 0);
    hid_report_apply_fast_gear(&rep, &msm, 0, GEAR_1, &hall, 0);
    CHECK(bit_set(rep.report, HID_BUTTON_GEAR1) && !bit_set(rep.report, HID_BUTTON_REVERSE), "1ere sans collerette");

    /* Dernier pas du geste d'entree en calibration (gear2, collerette
     * levee, detecteur a l'index 3) : jamais transmis. */
    msm.calib_entry.index = 3;
    hid_report_apply_fast_gear(&rep, &msm, 1, GEAR_2, &hall, 0);
    CHECK(!bit_set(rep.report, HID_BUTTON_GEAR2), "gear2 final du geste calibration masque");
    hid_report_apply_fast_gear(&rep, &msm, 0, GEAR_2, &hall, 0);
    CHECK(bit_set(rep.report, HID_BUTTON_GEAR2), "gear2 sans collerette transmis");
    msm.calib_entry.index = 0;

    /* Mode SEQ : l'impulsion du chemin lent (arrivee au relachement) est
     * masquee, seule l'impulsion rapide part. */
    hid_report_init(&rep);
    msm.mode_courant = GEAR_MODE_SEQ;
    msm.last_emitted.pos = GEAR_SEQ_UP;
    msm.last_emitted.kind = MOVEMENT_KIND_SEQ;
    msm.tracker.state = TRACKER_IDLE;
    hid_report_build(&rep, &msm, 0, &hall);
    hid_report_apply_fast_gear(&rep, &msm, 0, GEAR_NONE, &hall, 1000);
    CHECK(!bit_set(rep.report, HID_BUTTON_SHIFT_UP), "mode SEQ : pulse lent masque");

    /* Engagement seq_up : impulsion immediate de FAST_PULSE_MS, une seule
     * meme si le levier reste en butee. */
    uint32_t ts = 2000;
    int up_ms = 0;
    for (int i = 0; i < 400; i++, ts++) {
        hid_report_apply_fast_gear(&rep, &msm, 0, GEAR_SEQ_UP, &hall, ts);
        if (i == 0) {
            CHECK(bit_set(rep.report, HID_BUTTON_SHIFT_UP), "shift+ des l'engagement");
        }
        up_ms += bit_set(rep.report, HID_BUTTON_SHIFT_UP);
    }
    CHECK(up_ms == (int)FAST_PULSE_MS, "une seule impulsion de FAST_PULSE_MS en butee maintenue");

    /* Deux shift- enchaines en 60ms (< FAST_PULSE_MS) : deux fronts distincts. */
    int rising = 0;
    bool prev = false;
    gear_position_t seq[] = {GEAR_SEQ_DOWN, GEAR_NONE, GEAR_SEQ_DOWN, GEAR_NONE};
    uint32_t dur[] = {30, 30, 30, 400};
    for (int k = 0; k < 4; k++) {
        for (uint32_t i = 0; i < dur[k]; i++, ts++) {
            hid_report_apply_fast_gear(&rep, &msm, 0, seq[k], &hall, ts);
            bool b = bit_set(rep.report, HID_BUTTON_SHIFT_DOWN);
            rising += (b && !prev);
            prev = b;
        }
    }
    CHECK(rising == 2, "deux shift- rapides -> deux impulsions distinctes");
    CHECK(!bit_set(rep.report, HID_BUTTON_GEAR3), "mode SEQ : aucun bouton H");
}

/* Rejeu capture : pour chaque rapport H emis par l'ancien chemin (emit_kind
 * HELD), mesure de combien de ms le chemin rapide l'aurait devance. */
static void replay(const char *path, const gear_calibration_t *cal)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        printf("impossible d'ouvrir %s\n", path);
        failures++;
        return;
    }
    char line[512];
    fast_gear_t fg;
    fast_gear_init(&fg);
    gear_position_t prev_fast = GEAR_NONE;
    uint32_t fast_onset_t[GEAR_POSITION_COUNT] = {0};
    int n = 0, n_mismatch = 0;
    int n_fast = 0;
    long gain_sum = 0, gain_min = 1000000, gain_max = -1000000;

    fgets(line, sizeof line, f); /* entete */
    while (fgets(line, sizeof line, f) != NULL) {
        double t_host;
        unsigned long t;
        unsigned int h[HALL_SENSOR_COUNT];
        int collar;
        char rest[256] = {0};
        if (sscanf(line, "%lf,%lu,%u,%u,%u,%u,%u,%u,%d,%255[^\r\n]", &t_host, &t, &h[0], &h[1], &h[2], &h[3], &h[4],
                   &h[5], &collar, rest) < 9) {
            continue;
        }
        hall_sample_t hall;
        for (int i = 0; i < HALL_SENSOR_COUNT; i++) {
            hall.v[i] = (uint16_t)h[i];
        }
        gear_position_t g = fast_gear_step(&fg, (uint32_t)t, &hall, cal, GEAR_MODE_H);
        if (g != GEAR_NONE && g != prev_fast) {
            fast_onset_t[g] = (uint32_t)t;
            n_fast++;
        }
        prev_fast = g;

        /* rest = tracker_state,mode,report_debug,emit_pos,emit_kind,... */
        char *fields[6] = {0};
        char *p = rest;
        for (int k = 0; k < 6 && p != NULL; k++) {
            fields[k] = p;
            p = strchr(p, ',');
            if (p != NULL) {
                *p++ = '\0';
            }
        }
        if (fields[4] == NULL || strcmp(fields[4], "HELD") != 0 || strcmp(fields[1], "H") != 0) {
            continue;
        }
        gear_position_t slow = GEAR_NONE;
        for (int p2 = GEAR_1; p2 <= GEAR_6; p2++) {
            if (strcmp(fields[3], gear_position_name((gear_position_t)p2)) == 0) {
                slow = (gear_position_t)p2;
            }
        }
        if (slow == GEAR_NONE) {
            continue;
        }
        n++;
        if (g != slow || fast_onset_t[slow] == 0) {
            n_mismatch++;
            continue;
        }
        long gain = (long)t - (long)fast_onset_t[slow];
        gain_sum += gain;
        if (gain < gain_min) gain_min = gain;
        if (gain > gain_max) gain_max = gain;
    }
    fclose(f);
    int ok = n - n_mismatch;
    printf("%s : %d rapports H (chemin lent), %d engagements rapides, %d identiques cote rapide", path, n, n_fast, ok);
    if (ok > 0) {
        printf(", avance moyenne %ldms (min %ld, max %ld)", gain_sum / ok, gain_min, gain_max);
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    gear_calibration_t cal;
    calibration_store_defaults(&cal);
    synthetic_tests(&cal);
    for (int i = 1; i < argc; i++) {
        replay(argv[i], &cal);
    }
    if (failures == 0) {
        printf("test_fast_gear : OK\n");
        return 0;
    }
    printf("test_fast_gear : %d echec(s)\n", failures);
    return 1;
}
