/**
 * test_hid_report.c — tests synthetiques de hid_report_build (pas de
 * capture reelle necessaire, on construit directement des mode_state_machine_t
 * dans l'etat voulu). Verifie : latch gear1-6 jusqu'au neutre, remplacement
 * gear1->bouton11 avec collerette, pulse court shift+/-, octet debug.
 *
 * Build (voir test_replay.c pour la note sur calibration_flash) :
 *   gcc -std=c99 -Wall -Wextra -DCALIBRATION_FLASH_READ_OVERRIDE \
 *     -I ../Inc -I hal_stub test_hid_report.c ../Src/gear_position.c \
 *     ../Src/calibration_store.c ../Src/mode_state_machine.c \
 *     ../Src/hid_report.c ../Src/calibration_flash.c \
 *     hal_stub/hal_flash_stub.c -lm -o test_hid_report
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "calibration_store.h"
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

int main(void)
{
    gear_calibration_t calibration;
    calibration_store_defaults(&calibration);

    mode_state_machine_t msm;
    mode_state_machine_init(&msm, &calibration);

    hid_report_state_t rep;
    hid_report_init(&rep);

    /* Echantillon Hall factice reutilise par tous les tests bouton/mode
     * ci-dessous (le contenu ne les concerne pas) — verifie separement au
     * Test 8. */
    hall_sample_t hall = {{0, 0, 0, 0, 0, 0}};

    /* --- Test 1 : gear1 tenu, sans collerette -> bouton 1 actif. --- */
    msm.last_emitted.pos = GEAR_1;
    msm.last_emitted.kind = MOVEMENT_KIND_HELD;
    msm.tracker.state = TRACKER_HELD;
    hid_report_build(&rep, &msm, /*collar=*/0, &hall);
    CHECK(bit_set(rep.report, HID_BUTTON_GEAR1), "gear1 tenu doit activer bouton 1");
    CHECK(!bit_set(rep.report, HID_BUTTON_REVERSE), "bouton 11 ne doit pas s'activer sans collerette");

    /* --- Test 2 : meme position tenue, pas de nouvel evenement ce pas
     * (last_emitted vide), le latch doit rester actif. --- */
    msm.last_emitted.pos = GEAR_NONE;
    msm.last_emitted.kind = MOVEMENT_KIND_NONE;
    hid_report_build(&rep, &msm, /*collar=*/0, &hall);
    CHECK(bit_set(rep.report, HID_BUTTON_GEAR1), "gear1 doit rester latch tant que HELD");

    /* --- Test 3 : collerette levee APRES la confirmation, pendant que
     * gear1 est deja tenu -> ne doit PAS basculer vers le bouton 11 (latch
     * fige a l'etat collerette du moment de la confirmation, pas relu en
     * continu — comportement corrige suite a un test terrain ou lever/
     * relacher la collerette en 1ere faisait osciller 1/11 a chaque pas,
     * cf PLAN.md Phase 6). --- */
    hid_report_build(&rep, &msm, /*collar=*/1, &hall);
    CHECK(bit_set(rep.report, HID_BUTTON_GEAR1), "bouton 1 doit rester actif, collerette levee apres coup");
    CHECK(!bit_set(rep.report, HID_BUTTON_REVERSE), "bouton 11 ne doit pas s'activer hors confirmation");

    /* --- Test 3bis : relacher la collerette ne change rien non plus. --- */
    hid_report_build(&rep, &msm, /*collar=*/0, &hall);
    CHECK(bit_set(rep.report, HID_BUTTON_GEAR1), "bouton 1 doit rester actif apres relachement collerette");

    /* --- Test 4 : retour au neutre -> plus aucun bouton gear actif. --- */
    msm.tracker.state = TRACKER_IDLE;
    hid_report_build(&rep, &msm, /*collar=*/0, &hall);
    CHECK(!bit_set(rep.report, HID_BUTTON_GEAR1), "retour au neutre doit relacher le bouton 1");
    CHECK(!bit_set(rep.report, HID_BUTTON_REVERSE), "retour au neutre doit relacher le bouton 11");

    /* --- Test 4bis : passer en 1ere avec la collerette DEJA levee -> latch
     * marche arriere actif pour toute la duree du maintien, meme si on
     * relache la collerette ensuite. --- */
    msm.last_emitted.pos = GEAR_1;
    msm.last_emitted.kind = MOVEMENT_KIND_HELD;
    msm.tracker.state = TRACKER_HELD;
    hid_report_build(&rep, &msm, /*collar=*/1, &hall);
    CHECK(bit_set(rep.report, HID_BUTTON_REVERSE), "passer en 1ere collerette levee doit latcher bouton 11");
    CHECK(!bit_set(rep.report, HID_BUTTON_GEAR1), "bouton 1 ne doit pas etre actif en marche arriere");

    msm.last_emitted.pos = GEAR_NONE;
    msm.last_emitted.kind = MOVEMENT_KIND_NONE;
    hid_report_build(&rep, &msm, /*collar=*/0, &hall);
    CHECK(bit_set(rep.report, HID_BUTTON_REVERSE), "relacher la collerette ne doit pas desactiver le latch 11");
    CHECK(!bit_set(rep.report, HID_BUTTON_GEAR1), "bouton 1 doit rester inactif tant que le latch 11 est actif");

    msm.tracker.state = TRACKER_IDLE;
    hid_report_build(&rep, &msm, /*collar=*/0, &hall);
    CHECK(!bit_set(rep.report, HID_BUTTON_REVERSE), "retour au neutre doit relacher le latch 11");

    /* --- Test 5 : HELD_CORRECTED gear1->gear2 sans neutre, remplace le
     * bouton actif. --- */
    msm.tracker.state = TRACKER_HELD;
    msm.last_emitted.pos = GEAR_1;
    msm.last_emitted.kind = MOVEMENT_KIND_HELD;
    hid_report_build(&rep, &msm, 0, &hall);
    CHECK(bit_set(rep.report, HID_BUTTON_GEAR1), "gear1 tenu (setup test 5)");
    msm.last_emitted.pos = GEAR_2;
    msm.last_emitted.kind = MOVEMENT_KIND_HELD_CORRECTED;
    hid_report_build(&rep, &msm, 0, &hall);
    CHECK(bit_set(rep.report, HID_BUTTON_GEAR2), "HELD_CORRECTED doit activer le nouveau gear");
    CHECK(!bit_set(rep.report, HID_BUTTON_GEAR1), "HELD_CORRECTED doit relacher l'ancien gear");

    /* --- Test 6 : SEQ_UP confirme -> pulse court sur shift_up, retombe
     * apres HID_PULSE_STEPS pas. --- */
    msm.tracker.state = TRACKER_IDLE;
    msm.last_emitted.pos = GEAR_SEQ_UP;
    msm.last_emitted.kind = MOVEMENT_KIND_SEQ;
    hid_report_build(&rep, &msm, 0, &hall);
    CHECK(bit_set(rep.report, HID_BUTTON_SHIFT_UP), "SEQ_UP doit activer shift_up au premier pas");
    msm.last_emitted.pos = GEAR_NONE;
    msm.last_emitted.kind = MOVEMENT_KIND_NONE;
    int pulse_steps_seen = 0;
    for (int i = 0; i < HID_PULSE_STEPS + 2; i++) {
        hid_report_build(&rep, &msm, 0, &hall);
        if (bit_set(rep.report, HID_BUTTON_SHIFT_UP)) {
            pulse_steps_seen++;
        }
    }
    CHECK(pulse_steps_seen == HID_PULSE_STEPS - 1, "pulse shift_up doit durer HID_PULSE_STEPS pas au total");
    hid_report_build(&rep, &msm, 0, &hall);
    CHECK(!bit_set(rep.report, HID_BUTTON_SHIFT_UP), "shift_up doit retomber apres le pulse");

    /* --- Test 7 : octet debug reflete mode_courant / calibration active. --- */
    msm.mode_courant = GEAR_MODE_SEQ;
    msm.calib_mode.active = false;
    hid_report_build(&rep, &msm, 0, &hall);
    CHECK((rep.report[HID_DEBUG_BYTE_INDEX] & (1u << HID_DEBUG_BIT_MODE_SEQ)) != 0, "mode SEQ doit lever le bit debug");

    msm.mode_courant = GEAR_MODE_H;
    msm.calib_mode.active = true;
    hid_report_build(&rep, &msm, 0, &hall);
    CHECK((rep.report[HID_DEBUG_BYTE_INDEX] & (1u << HID_DEBUG_BIT_CALIBRATION)) != 0,
          "calibration active doit lever le bit debug");
    CHECK((rep.report[HID_DEBUG_BYTE_INDEX] & (1u << HID_DEBUG_BIT_MODE_SEQ)) == 0,
          "mode H ne doit pas lever le bit mode SEQ");

    /* --- Test 8 : les octets vendor 2-13 portent les 6 valeurs Hall
     * brutes en uint16 LE (reproduit le comportement du firmware
     * d'origine, cf hid_report.h). --- */
    hall_sample_t hall2 = {{0x1234, 0xABCD, 0x0001, 0x0FFF, 0x8000, 0x7654}};
    hid_report_build(&rep, &msm, 0, &hall2);
    for (int i = 0; i < HALL_SENSOR_COUNT; i++) {
        int idx = HID_AXES_BYTE_OFFSET + i * HID_AXES_STRIDE;
        uint16_t readback = (uint16_t)(rep.report[idx] | (rep.report[idx + 1] << 8));
        CHECK(readback == hall2.v[i], "octet vendor doit reproduire la valeur Hall brute (LE)");
    }

    if (failures == 0) {
        printf("OK: tous les tests hid_report ont reussi.\n");
    } else {
        printf("%d test(s) en echec.\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
