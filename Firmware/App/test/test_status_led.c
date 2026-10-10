/**
 * test_status_led.c — motifs de la LED (status_led.c) : respiration +
 * impulsions du point attendu en calibration, flashs de validation, de
 * maintien suffisant et de bascule de mode, PWM logiciel.
 *
 * Build : gcc -std=c99 -Wall -Wextra -DCALIBRATION_FLASH_READ_OVERRIDE \
 *   -I ../Inc -I hal_stub test_status_led.c ../Src/status_led.c \
 *   ../Src/gear_position.c ../Src/calibration_store.c \
 *   ../Src/mode_state_machine.c ../Src/calibration_flash.c \
 *   hal_stub/hal_flash_stub.c hal_stub/hal_gpio_stub.c -lm -o test_status_led
 */
#include <stdio.h>

#include "calibration_store.h"
#include "main.h" /* hal_stub/main.h */
#include "status_led.h"

static int failures = 0;
#define CHECK(cond, msg)                                                                                             \
    do {                                                                                                             \
        if (!(cond)) {                                                                                               \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                                                   \
            failures++;                                                                                              \
        }                                                                                                            \
    } while (0)

/* Impulsions pleines (montee a 255) apres la respiration, sur un cycle. */
static int count_pulses(const status_led_state_t *s, uint32_t t0, uint32_t *longest_ms)
{
    int pulses = 0;
    uint32_t run = 0;
    *longest_ms = 0;
    uint8_t prev = 0;
    for (uint32_t t = STATUS_LED_BREATH_MS; t < 20000u; t++) {
        uint8_t l = status_led_level(s, t0 + t);
        if (l == 255 && prev != 255) {
            pulses++;
            run = 0;
        }
        if (l == 255) {
            run++;
            if (run > *longest_ms) {
                *longest_ms = run;
            }
        }
        /* fin du cycle : debut de la respiration suivante */
        if (t > STATUS_LED_BREATH_MS + STATUS_LED_GAP_MS && l > 0 && l < 255) {
            break;
        }
        prev = l;
    }
    return pulses;
}

int main(void)
{
    gear_calibration_t cal;
    calibration_store_defaults(&cal);
    mode_state_machine_t msm;
    mode_state_machine_init(&msm, &cal);
    status_led_state_t led;
    status_led_init(&led);
    uint32_t t = 1000;

    /* Hors calibration : eteinte, aucune ecriture de la broche. */
    status_led_step(&led, t, &msm);
    int writes = hal_stub_gpio_writes;
    for (int i = 0; i < 500; i++) {
        status_led_output(&led, t + (uint32_t)i);
    }
    CHECK(status_led_level(&led, t) == 0, "hors calibration : eteinte");
    CHECK(hal_stub_gpio_writes == writes, "broche ecrite seulement sur changement");

    /* Entree en calibration : respiration puis 1 impulsion (gear1). */
    calibration_mode_enter(&msm.calib_mode, &cal.positions[GEAR_NEUTRAL]);
    status_led_step(&led, t, &msm);
    CHECK(led.pattern == STATUS_LED_PATTERN_WAITING, "calibration : motif d'attente");
    CHECK(status_led_level(&led, t) == 0, "respiration : part de 0");
    CHECK(status_led_level(&led, t + STATUS_LED_BREATH_MS / 2) == 255, "respiration : 255 au milieu");
    bool rising = true;
    for (uint32_t k = 1; k <= STATUS_LED_BREATH_MS / 2; k++) {
        if (status_led_level(&led, t + k) < status_led_level(&led, t + k - 1)) {
            rising = false;
        }
    }
    CHECK(rising, "respiration : montee reguliere");
    uint32_t longest;
    CHECK(count_pulses(&led, t, &longest) == 1, "gear1 : 1 impulsion");

    /* PWM : fraction allumee ~ luminosite moyenne sur la respiration. */
    unsigned long want = 0, got = 0;
    for (uint32_t k = 0; k < STATUS_LED_BREATH_MS; k++) {
        uint8_t l = status_led_level(&led, t + k);
        want += (l >= 16) ? l : 0;
        status_led_output(&led, t + k);
        got += led.led_on ? 255u : 0u;
    }
    CHECK(got + 255u * 5u >= want && got <= want + 255u * 5u, "PWM : rapport cyclique = luminosite");

    /* Validation d'un point : 3 flashs, puis 3 impulsions (gear3 apres
     * gear2). */
    msm.calib_mode.step_index = 2;
    t += 10000;
    status_led_step(&led, t, &msm);
    CHECK(led.pattern == STATUS_LED_PATTERN_VALIDATED, "point valide : flash de validation");
    CHECK(status_led_level(&led, t) == 255 && status_led_level(&led, t + 80) == 0, "flash de validation");
    status_led_step(&led, t + 200, &msm);
    CHECK(led.pattern == STATUS_LED_PATTERN_VALIDATED, "flash de validation va au bout");
    status_led_step(&led, t + 500, &msm);
    CHECK(led.pattern == STATUS_LED_PATTERN_WAITING, "puis retour au motif d'attente");
    CHECK(count_pulses(&led, t + 500, &longest) == 3, "gear3 : 3 impulsions");
    CHECK(longest == STATUS_LED_PULSE_ON_MS, "impulsion de STATUS_LED_PULSE_ON_MS");

    /* Maintien suffisant : blip-blip, puis retour au point attendu. */
    t += 10000;
    msm.calib_mode.triggered_for_this_hold = true;
    status_led_step(&led, t, &msm);
    CHECK(led.pattern == STATUS_LED_PATTERN_HOLD_OK, "maintien suffisant : blip-blip");
    status_led_step(&led, t + 100, &msm);
    CHECK(led.pattern == STATUS_LED_PATTERN_HOLD_OK, "blip-blip tant que le maintien dure");
    msm.calib_mode.triggered_for_this_hold = false;
    status_led_step(&led, t + 200, &msm);
    CHECK(led.pattern == STATUS_LED_PATTERN_WAITING, "maintien interrompu : retour a l'attente");

    /* seq_up = 7, seq_down = 8, neutre = une impulsion longue. */
    const struct {
        int step;
        int pulses;
        uint32_t longest;
    } cases[] = {{6, 7, STATUS_LED_PULSE_ON_MS}, {7, 8, STATUS_LED_PULSE_ON_MS}, {8, 1, STATUS_LED_NEUTRAL_ON_MS}};
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        t += 10000;
        msm.calib_mode.step_index = cases[i].step;
        status_led_step(&led, t, &msm);
        status_led_step(&led, t + 500, &msm);
        CHECK(count_pulses(&led, t + 500, &longest) == cases[i].pulses, "nombre d'impulsions du point attendu");
        CHECK(longest == cases[i].longest, "duree d'impulsion du point attendu");
    }

    /* Point refuse (meme etape) : scintillement 1s, puis de nouveau le
     * point attendu (neutre : impulsion longue). */
    t += 10000;
    msm.calib_mode.rejected_count++;
    status_led_step(&led, t, &msm);
    CHECK(led.pattern == STATUS_LED_PATTERN_REJECTED, "point refuse : scintillement");
    CHECK(status_led_level(&led, t) == 255 && status_led_level(&led, t + 50) == 0 &&
              status_led_level(&led, t + 100) == 255,
          "scintillement a 50ms");
    status_led_step(&led, t + 900, &msm);
    CHECK(led.pattern == STATUS_LED_PATTERN_REJECTED, "scintillement va au bout (1s)");
    status_led_step(&led, t + 1000, &msm);
    CHECK(led.pattern == STATUS_LED_PATTERN_WAITING, "puis retour au point attendu");
    CHECK(count_pulses(&led, t + 1000, &longest) == 1 && longest == STATUS_LED_NEUTRAL_ON_MS,
          "meme point attendu apres un refus");

    /* Fin de calibration : flash de validation, puis eteinte. */
    t += 10000;
    msm.calib_mode.active = false;
    status_led_step(&led, t, &msm);
    CHECK(led.pattern == STATUS_LED_PATTERN_VALIDATED, "dernier point : flash de validation");
    status_led_step(&led, t + 100, &msm);
    CHECK(led.pattern == STATUS_LED_PATTERN_VALIDATED, "flash final va au bout");
    status_led_step(&led, t + 500, &msm);
    CHECK(led.pattern == STATUS_LED_PATTERN_NONE && status_led_level(&led, t + 500) == 0, "puis eteinte");

    /* Bascule de mode : double flash puis eteinte. */
    t += 10000;
    msm.last_switch_applied = true;
    status_led_step(&led, t, &msm);
    msm.last_switch_applied = false;
    CHECK(led.pattern == STATUS_LED_PATTERN_MODE_SWITCHED, "bascule : double flash");
    CHECK(status_led_level(&led, t + 50) == 255 && status_led_level(&led, t + 150) == 0 &&
              status_led_level(&led, t + 250) == 255 && status_led_level(&led, t + 450) == 0,
          "double flash 100ms");
    status_led_step(&led, t + 100, &msm);
    CHECK(led.pattern == STATUS_LED_PATTERN_MODE_SWITCHED, "double flash va au bout");
    status_led_step(&led, t + 500, &msm);
    CHECK(led.pattern == STATUS_LED_PATTERN_NONE, "puis eteinte");

    if (failures == 0) {
        printf("OK: tous les tests status_led ont reussi.\n");
    } else {
        printf("%d test(s) en echec.\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
