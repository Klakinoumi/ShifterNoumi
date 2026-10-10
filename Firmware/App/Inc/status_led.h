/**
 * status_led.h — retour visuel sur la LED integree de la Blue Pill (PC13,
 * active a l'etat bas) pendant le mode calibration et lors d'une bascule
 * de mode H<->SEQ.
 *
 * Objectif : donner un signal terrain, sans CSV ni outil PC, pour savoir
 * (1) qu'on est bien en mode calibration et QUEL point est attendu, (2)
 * qu'un maintien en cours a deja accumule assez de donnees pour etre
 * valide si on relache maintenant, (3) qu'un point vient d'etre valide, et
 * (4) qu'une bascule de mode H<->SEQ vient d'etre appliquee.
 *
 * Motifs (priorite du haut vers le bas) :
 *  - Bascule de mode appliquee (msm->last_switch_applied) : double flash
 *    (100ms allume / 100ms eteint x2), puis LED eteinte.
 *  - Point de calibration valide (calib_mode.step_index qui avance, ou fin
 *    de calibration apres le dernier point) : 3 flashs rapides (80ms).
 *  - Point de calibration refuse (calib_mode.rejected_count qui augmente :
 *    levier au repos ou sur un point deja enregistre) : scintillement
 *    rapide d'une seconde (10 flashs de 50ms), puis de nouveau le point
 *    attendu.
 *  - Maintien collerette deja assez long pour etre valide
 *    (calib_mode.triggered_for_this_hold) : "blip-blip" en boucle.
 *  - En calibration, en attente d'un point : une respiration lente (fondu
 *    allume/eteint sur STATUS_LED_BREATH_MS, ~0.5Hz), pause, puis le point
 *    attendu en impulsions : gear1..6 = 1..6 impulsions, seq_up = 7,
 *    seq_down = 8, neutre = une impulsion longue ; pause, et on recommence.
 *  - Sinon : LED eteinte.
 *
 * Deux appels : status_led_step() au pas lent choisit le motif (evenements
 * de mode_state_machine) ; status_led_output() a chaque tour de boucle
 * (1ms) produit la luminosite, en PWM logiciel (PC13 n'a pas de sortie
 * timer). status_led_level() est la partie pure (testable hors cible).
 */
#ifndef STATUS_LED_H
#define STATUS_LED_H

#include <stdint.h>

#include "mode_state_machine.h"

/* Respiration + impulsions du motif "en attente d'un point". */
#define STATUS_LED_BREATH_MS      2000u /* une respiration complete, ~0.5Hz */
#define STATUS_LED_GAP_MS         500u  /* entre respiration et impulsions */
#define STATUS_LED_PULSE_ON_MS    250u
#define STATUS_LED_PULSE_OFF_MS   250u
#define STATUS_LED_NEUTRAL_ON_MS  1000u /* neutre : une impulsion longue */
#define STATUS_LED_REST_MS        1000u /* apres les impulsions, avant la respiration suivante */

typedef enum {
    STATUS_LED_PATTERN_NONE = 0,      /* eteinte fixe */
    STATUS_LED_PATTERN_WAITING,       /* respiration + impulsions du point attendu */
    STATUS_LED_PATTERN_HOLD_OK,       /* "blip-blip" en boucle (maintien deja suffisant) */
    STATUS_LED_PATTERN_VALIDATED,     /* 3 flashs rapides (point de calibration valide) */
    STATUS_LED_PATTERN_MODE_SWITCHED, /* double flash (bascule de mode H<->SEQ appliquee) */
    STATUS_LED_PATTERN_REJECTED,      /* scintillement 1s (point de calibration refuse) */
} status_led_pattern_t;

typedef struct {
    int last_step_index;         /* -1 hors calibration */
    uint8_t last_rejected_count; /* calib_mode.rejected_count au pas precedent */
    bool was_triggered_for_hold; /* etat precedent de triggered_for_this_hold */

    status_led_pattern_t pattern; /* motif actuellement joue */
    uint32_t pattern_t0;          /* debut du motif (t_ms) */
    gear_position_t target;       /* point attendu (motif WAITING) */

    uint16_t pwm_acc; /* accumulateur du PWM logiciel (sigma-delta) */
    bool led_on;
} status_led_state_t;

void status_led_init(status_led_state_t *state);

/* A appeler a chaque pas lent (100ms), apres mode_state_machine_step() :
 * choisit le motif. N'ecrit pas la broche. */
void status_led_step(status_led_state_t *state, uint32_t t_ms, const mode_state_machine_t *msm);

/* Luminosite voulue (0-255) a l'instant t_ms pour le motif courant. */
uint8_t status_led_level(const status_led_state_t *state, uint32_t t_ms);

/* A appeler a chaque tour de boucle (1ms) : PWM logiciel de
 * status_led_level(), ecrit PC13 seulement quand l'etat change. */
void status_led_output(status_led_state_t *state, uint32_t t_ms);

#endif /* STATUS_LED_H */
