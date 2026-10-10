/**
 * gear_position.h — classification de la position du levier a partir des
 * 6 valeurs Hall (U2..U7), portage C de PCB/gear_position.py.
 *
 * Aucune dependance HAL/USB : module testable hors cible (voir App/test).
 */
#ifndef GEAR_POSITION_H
#define GEAR_POSITION_H

#include <stdint.h>
#include <stdbool.h>

#define HALL_SENSOR_COUNT 6

/* Une position calibree = nom + 6 valeurs Hall brutes de reference. */
typedef enum {
    GEAR_NEUTRAL = 0,
    GEAR_1,
    GEAR_2,
    GEAR_3,
    GEAR_4,
    GEAR_5,
    GEAR_6,
    GEAR_SEQ_UP,
    GEAR_SEQ_DOWN,
    GEAR_POSITION_COUNT,
    GEAR_NONE = -1, /* aucune position (ne doit pas apparaitre dans calibration[]) */
} gear_position_t;

typedef enum {
    GEAR_MODE_NONE = 0, /* neutre : pas de mode intrinseque */
    GEAR_MODE_H,
    GEAR_MODE_SEQ,
} gear_mode_t;

/* Valeurs Hall brutes (ADC 12 bits, 0-4095), ordre U2..U7. */
typedef struct {
    uint16_t v[HALL_SENSOR_COUNT];
} hall_sample_t;

/* Vecteur normalise (voir gear_position_normalize). */
typedef struct {
    float v[HALL_SENSOR_COUNT];
} hall_norm_t;

/* Jeu de calibration complet : un point de reference par position,
 * plus neutral_i/scale_i par capteur derives de ces points. */
typedef struct {
    hall_sample_t positions[GEAR_POSITION_COUNT];
    gear_mode_t modes[GEAR_POSITION_COUNT]; /* GEAR_MODE_NONE pour GEAR_NEUTRAL */
    float neutral[HALL_SENSOR_COUNT];
    float scale[HALL_SENSOR_COUNT];
} gear_calibration_t;

/* Seuil de distance normalisee (euclidienne) au neutre. Calibre
 * empiriquement (cf PLAN.md Phase 5) : bruit de mesure au repos < 0.2,
 * positions actives > 0.5. */
#define GEAR_NEUTRAL_DIST_THRESHOLD 0.25f

/* Nom court d'une position (pour logs/debug), jamais NULL. */
const char *gear_position_name(gear_position_t pos);

/* Recalcule neutral[]/scale[] a partir de calibration->positions[].
 * scale_i = max(|position_j[i] - neutral_i|) sur toutes les positions
 * hors GEAR_NEUTRAL ; garde-fou scale_i >= 1 pour eviter une division
 * par zero si un capteur ne bouge jamais. A appeler une fois au demarrage
 * (calibration chargee depuis calibration_store) et a chaque fois qu'un
 * point est mis a jour (mode calibration). */
void gear_position_compute_normalization(gear_calibration_t *calibration);

/* Normalise un echantillon Hall brut vers [-1,+1] approx, par capteur. */
hall_norm_t gear_position_normalize(const hall_sample_t *hall, const gear_calibration_t *calibration);

/* Test leger de proximite au neutre (distance euclidienne normalisee). */
bool gear_position_is_neutral(const hall_sample_t *hall, const gear_calibration_t *calibration);

/* Plus-proche-voisin sur les positions calibrees (distance euclidienne
 * normalisee). Renvoie la position la plus proche ; si out_distance
 * n'est pas NULL, y ecrit la distance normalisee correspondante. */
gear_position_t gear_position_classify(const hall_sample_t *hall, const gear_calibration_t *calibration,
                                        float *out_distance);

#endif /* GEAR_POSITION_H */
