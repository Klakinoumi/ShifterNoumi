/**
 * test_calibration_mode.c — mode calibration de bout en bout via
 * mode_state_machine : les points sont gardes en RAM et ecrits en flash en
 * une seule fois apres le 9eme ; une calibration interrompue ne change rien.
 * Flash simulee en RAM avec la vraie semantique, comme
 * test_calibration_flash.c.
 *
 * Points refuses (levier au repos, point deja enregistre) : synthetiques,
 * puis rejeu des vraies sessions de calibration passees en argument
 * (*_cdc.csv) : les 9 points doivent tous etre acceptes.
 *
 * Build : gcc -std=c99 -Wall -Wextra -DCALIBRATION_FLASH_READ_OVERRIDE \
 *   -I ../Inc -I hal_stub test_calibration_mode.c ../Src/gear_position.c \
 *   ../Src/calibration_store.c ../Src/mode_state_machine.c \
 *   ../Src/calibration_flash.c -lm -o test_calibration_mode
 * Usage : test_calibration_mode [session_cdc.csv[:acceptes:refuses] ...]
 * calib_refus (2026-10-07) : erreurs volontaires et involontaires ; avec
 * les regles actuelles 4 refus (neutre, 1ere en double, seq inverses,
 * neutre pris en 1ere). Le 9eme point accepte vient d'un maintien qui,
 * en vrai, a eu lieu apres la fin de la calibration.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "calibration_flash.h"
#include "calibration_store.h"
#include "main.h" /* hal_stub/main.h */
#include "mode_state_machine.h"

static uint8_t g_flash[CALIBRATION_FLASH_PAGE_SIZE];

HAL_StatusTypeDef HAL_FLASH_Unlock(void) { return HAL_OK; }
HAL_StatusTypeDef HAL_FLASH_Lock(void) { return HAL_OK; }

HAL_StatusTypeDef HAL_FLASH_Program(uint32_t TypeProgram, uint32_t Address, uint64_t Data)
{
    (void)TypeProgram;
    uint32_t offset = Address - CALIBRATION_FLASH_PAGE_ADDR;
    uint16_t existing, hw = (uint16_t)Data;
    memcpy(&existing, &g_flash[offset], 2);
    existing &= hw;
    memcpy(&g_flash[offset], &existing, 2);
    return HAL_OK;
}

HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *pEraseInit, uint32_t *PageError)
{
    (void)pEraseInit;
    memset(g_flash, 0xFF, sizeof(g_flash));
    *PageError = 0xFFFFFFFFUL;
    return HAL_OK;
}

void flash_read_bytes(uintptr_t addr, void *out, size_t len)
{
    memcpy(out, &g_flash[addr - CALIBRATION_FLASH_PAGE_ADDR], len);
}

static int failures = 0;
#define CHECK(cond, msg)                                                                                             \
    do {                                                                                                             \
        if (!(cond)) {                                                                                               \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                                                   \
            failures++;                                                                                              \
        }                                                                                                            \
    } while (0)

static int slots_used(void)
{
    int n = 0;
    for (unsigned s = 0; s < CALIBRATION_FLASH_SLOT_COUNT; s++) {
        for (unsigned i = 0; i < CALIBRATION_FLASH_SLOT_SIZE; i++) {
            if (g_flash[s * CALIBRATION_FLASH_SLOT_SIZE + i] != 0xFF) {
                n++;
                break;
            }
        }
    }
    return n;
}

/* Point "mesure" pendant la session : defaut + decalage, distinct partout. */
static hall_sample_t measured(const gear_calibration_t *def, gear_position_t pos)
{
    hall_sample_t h = def->positions[pos];
    for (int i = 0; i < HALL_SENSOR_COUNT; i++) {
        h.v[i] = (uint16_t)(h.v[i] - 7 - pos);
    }
    return h;
}

/* Un point : levier en place, collerette levee 2.5s, relachee. */
static void record_point(mode_state_machine_t *msm, uint32_t *t, const hall_sample_t *h)
{
    for (int i = 0; i < 3; i++, *t += 100) {
        mode_state_machine_step(msm, *t, h, 0);
    }
    for (int i = 0; i < 25; i++, *t += 100) {
        mode_state_machine_step(msm, *t, h, 1);
    }
    mode_state_machine_step(msm, *t, h, 0);
    *t += 100;
}

/* Rejoue les lignes calib_active=1 d'une capture CDC dans un mode
 * calibration neuf (neutre de reference = defauts). Retourne le nombre de
 * points acceptes, *rejected = points refuses ; -1 si illisible. */
static int replay_session(const char *path, const gear_calibration_t *def, int *rejected)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }
    char line[1024];
    if (fgets(line, sizeof line, f) == NULL) {
        fclose(f);
        return -1;
    }
    /* Colonnes utiles : t_ms(1), u2..u7(2-7), collar(8), calib_active(20). */
    calibration_mode_t cal;
    calibration_mode_init(&cal, NULL, NULL);
    bool entered = false;
    int accepted = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        char *fields[32];
        int n = 0;
        for (char *p = line; n < 32;) {
            fields[n++] = p;
            p = strchr(p, ',');
            if (p == NULL) {
                break;
            }
            *p++ = '\0';
        }
        /* La trace montre l'etat apres le pas : la ligne du relachement
         * du 9eme point a deja calib_active=0, elle est rejouee aussi. */
        if (n < 21 || (atoi(fields[20]) != 1 && !entered)) {
            continue;
        }
        if (!entered) {
            calibration_mode_enter(&cal, &def->positions[GEAR_NEUTRAL]);
            entered = true;
        }
        hall_sample_t h;
        for (int i = 0; i < HALL_SENSOR_COUNT; i++) {
            h.v[i] = (uint16_t)atoi(fields[2 + i]);
        }
        int before = cal.step_index;
        calibration_mode_step(&cal, (uint32_t)atol(fields[1]), &h, atoi(fields[8]));
        accepted += (cal.step_index != before || (!cal.active && before == CALIBRATION_SEQUENCE_LEN - 1)) ? 1 : 0;
        if (!cal.active) {
            break;
        }
    }
    fclose(f);
    *rejected = cal.rejected_count;
    return accepted;
}

int main(int argc, char **argv)
{
    memset(g_flash, 0xFF, sizeof(g_flash));
    gear_calibration_t def;
    calibration_store_defaults(&def);
    CHECK(calibration_flash_save(def.positions), "calibration de depart en flash");
    uint32_t t = 1000;

    /* --- Calibration interrompue apres 5 points (debranchement) : rien
     * n'a change, ni en RAM ni en flash. --- */
    mode_state_machine_t msm;
    mode_state_machine_init(&msm, &def);
    calibration_mode_enter(&msm.calib_mode, &def.positions[GEAR_NEUTRAL]);
    for (int k = 0; k < 5; k++) {
        hall_sample_t h = measured(&def, CALIBRATION_SEQUENCE[k]);
        record_point(&msm, &t, &h);
    }
    CHECK(msm.calib_mode.active && msm.calib_mode.step_index == 5, "5 points enregistres, calibration en cours");
    CHECK(memcmp(msm.calibration.positions, def.positions, sizeof(def.positions)) == 0,
          "calibration active inchangee pendant la session");
    CHECK(slots_used() == 1, "aucune ecriture flash pendant la session");

    /* --- Calibration complete : les 9 points appliques et ecrits en
     * flash en une seule fois. --- */
    mode_state_machine_init(&msm, &def);
    calibration_mode_enter(&msm.calib_mode, &def.positions[GEAR_NEUTRAL]);
    for (int k = 0; k < CALIBRATION_SEQUENCE_LEN; k++) {
        hall_sample_t h = measured(&def, CALIBRATION_SEQUENCE[k]);
        record_point(&msm, &t, &h);
        if (k < CALIBRATION_SEQUENCE_LEN - 1) {
            CHECK(slots_used() == 1, "rien d'ecrit avant le dernier point");
        }
    }
    CHECK(!msm.calib_mode.active, "calibration terminee apres le 9eme point");
    CHECK(slots_used() == 2, "une seule sauvegarde pour les 9 points");

    bool all_applied = true;
    for (int p = 0; p < GEAR_POSITION_COUNT; p++) {
        hall_sample_t want = measured(&def, (gear_position_t)p);
        if (memcmp(&msm.calibration.positions[p], &want, sizeof(want)) != 0) {
            all_applied = false;
        }
    }
    CHECK(all_applied, "les 9 points mesures sont appliques");

    gear_calibration_t reloaded;
    CHECK(calibration_store_load(&reloaded), "relecture de la flash");
    CHECK(memcmp(reloaded.positions, msm.calibration.positions, sizeof(reloaded.positions)) == 0,
          "la flash contient la nouvelle calibration");
    CHECK(memcmp(reloaded.neutral, msm.calibration.neutral, sizeof(reloaded.neutral)) == 0,
          "neutral/scale recalcules a l'identique");

    /* --- Points refuses : levier reste au repos, puis point deja
     * enregistre (levier pas bouge entre deux etapes). L'etape n'avance
     * pas, rien n'est ecrit ; le bon point est ensuite accepte. --- */
    mode_state_machine_init(&msm, &def);
    calibration_mode_enter(&msm.calib_mode, &def.positions[GEAR_NEUTRAL]);
    int slots_before = slots_used();
    hall_sample_t at_rest = measured(&def, GEAR_NEUTRAL);
    record_point(&msm, &t, &at_rest);
    CHECK(msm.calib_mode.step_index == 0 && msm.calib_mode.rejected_count == 1, "levier au repos : point refuse");
    hall_sample_t g1 = measured(&def, GEAR_1);
    record_point(&msm, &t, &g1);
    CHECK(msm.calib_mode.step_index == 1, "gear1 accepte");
    hall_sample_t g1_again = g1;
    g1_again.v[0] = (uint16_t)(g1_again.v[0] + 40); /* bruit : meme position */
    record_point(&msm, &t, &g1_again);
    CHECK(msm.calib_mode.step_index == 1 && msm.calib_mode.rejected_count == 2,
          "gear1 de nouveau a l'etape gear2 : refuse");
    hall_sample_t g2 = measured(&def, GEAR_2);
    record_point(&msm, &t, &g2);
    CHECK(msm.calib_mode.step_index == 2, "gear2 accepte apres un refus");
    CHECK(slots_used() == slots_before, "rien d'ecrit pendant la session");

    /* Le neutre (dernier point) est accepte meme s'il est proche du neutre
     * de reference ; gear3 et seq_down (les deux points distincts les plus
     * proches, ecart L1 516 sur les defauts) sont acceptes tous les deux. */
    for (int k = 2; k < CALIBRATION_SEQUENCE_LEN; k++) {
        hall_sample_t h = measured(&def, CALIBRATION_SEQUENCE[k]);
        record_point(&msm, &t, &h);
    }
    CHECK(!msm.calib_mode.active && msm.calib_mode.rejected_count == 2, "session terminee, 9 points acceptes");

    /* Seuils : juste sous / au-dessus de CALIBRATION_MIN_GAP_L1. */
    calibration_mode_t c;
    calibration_mode_init(&c, NULL, NULL);
    calibration_mode_enter(&c, &def.positions[GEAR_NEUTRAL]);
    c.pending[GEAR_1] = def.positions[GEAR_1];
    c.step_index = 1;
    hall_sample_t near = def.positions[GEAR_1];
    near.v[2] = (uint16_t)(near.v[2] + CALIBRATION_MIN_GAP_L1 - 1);
    CHECK(calibration_mode_rejects(&c, &near), "ecart L1 < seuil : refuse");
    near.v[2] = (uint16_t)(near.v[2] + 1);
    CHECK(!calibration_mode_rejects(&c, &near), "ecart L1 = seuil : accepte");

    /* Session aux points par defaut (gear1..6 enregistres) pour les regles
     * qui dependent des points de la session. */
    calibration_mode_init(&c, NULL, NULL);
    calibration_mode_enter(&c, &def.positions[GEAR_NEUTRAL]);
    for (int k = 0; k < 6; k++) {
        c.pending[CALIBRATION_SEQUENCE[k]] = def.positions[CALIBRATION_SEQUENCE[k]];
    }

    /* seq_up / seq_down inverses (calib_refus) : refuses par le sens. */
    c.step_index = 6; /* seq_up attendu */
    CHECK(calibration_mode_rejects(&c, &def.positions[GEAR_SEQ_DOWN]), "seq_down a l'etape seq_up : refuse");
    CHECK(!calibration_mode_rejects(&c, &def.positions[GEAR_SEQ_UP]), "seq_up a l'etape seq_up : accepte");
    c.pending[GEAR_SEQ_UP] = def.positions[GEAR_SEQ_UP];
    c.step_index = 7; /* seq_down attendu */
    hall_sample_t up_side = def.positions[GEAR_SEQ_UP];
    up_side.v[4] = (uint16_t)(up_side.v[4] + 500); /* loin de seq_up, cote 4e */
    CHECK(calibration_mode_rejects(&c, &up_side), "point cote 4e a l'etape seq_down : refuse");
    CHECK(!calibration_mode_rejects(&c, &def.positions[GEAR_SEQ_DOWN]), "seq_down a l'etape seq_down : accepte");

    /* gear3/seq_down : proches pour de vrai (347-516), seuil reduit. */
    hall_sample_t sd = def.positions[GEAR_3];
    sd.v[1] = (uint16_t)(sd.v[1] - 200); /* a 200 de la 3e */
    CHECK(!calibration_mode_rejects(&c, &sd), "seq_down a 200 de la 3e : accepte (paire proche)");
    sd.v[1] = (uint16_t)(sd.v[1] + 51); /* a 149 */
    CHECK(calibration_mode_rejects(&c, &sd), "seq_down a 149 de la 3e : refuse");

    /* Neutre pris en 1ere remise en grille (calib_refus : 157 de la 1ere). */
    c.pending[GEAR_SEQ_DOWN] = def.positions[GEAR_SEQ_DOWN];
    c.step_index = 8; /* neutre attendu */
    hall_sample_t reseated = def.positions[GEAR_1];
    reseated.v[1] = (uint16_t)(reseated.v[1] + 116);
    reseated.v[5] = (uint16_t)(reseated.v[5] - 41);
    CHECK(calibration_mode_rejects(&c, &reseated), "neutre pris en 1ere (157) : refuse");
    CHECK(!calibration_mode_rejects(&c, &def.positions[GEAR_NEUTRAL]), "vrai neutre : accepte");

    /* Vraies sessions : chemin[:acceptes:refuses], par defaut 9 acceptes et
     * aucun refus. */
    for (int a = 1; a < argc; a++) {
        char path[512];
        int want_accepted = CALIBRATION_SEQUENCE_LEN, want_rejected = 0;
        strncpy(path, argv[a], sizeof path - 1);
        path[sizeof path - 1] = '\0';
        char *colon = strrchr(path, ':');
        if (colon != NULL && colon != path + 1) { /* pas le "C:" d'un chemin Windows */
            *colon = '\0';
            want_rejected = atoi(colon + 1);
            colon = strrchr(path, ':');
            if (colon != NULL && colon != path + 1) {
                *colon = '\0';
                want_accepted = atoi(colon + 1);
            }
        }
        int rejected = 0;
        int accepted = replay_session(path, &def, &rejected);
        printf("%s : %d point(s) accepte(s), %d refuse(s) (attendu %d, %d)\n", path, accepted, rejected,
               want_accepted, want_rejected);
        CHECK(accepted == want_accepted && rejected == want_rejected, "session reelle : decomptes attendus");
    }

    if (failures == 0) {
        printf("OK: tous les tests calibration_mode ont reussi.\n");
    } else {
        printf("%d test(s) en echec.\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
