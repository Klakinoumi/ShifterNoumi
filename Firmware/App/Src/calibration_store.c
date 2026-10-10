#include "calibration_store.h"

#include <string.h>

#include "calibration_flash.h"

/* Valeurs par defaut = PCB/gear_calibration.json, session Phase 4/5.
 * Ordre des capteurs : U2 U3 U4 U5 U6 U7. Le neutre fusionne les anciens
 * points neutral/seq_neutral (meme position physique de repos). */
static const gear_calibration_t DEFAULT_CALIBRATION = {
    .positions = {
        [GEAR_NEUTRAL]  = {{2977, 3093, 2917, 2979, 2772, 2902}},
        [GEAR_1]        = {{3792, 3771, 2946, 3017, 2826, 2990}},
        [GEAR_2]        = {{3012, 3109, 2980, 3771, 3375, 2938}},
        [GEAR_3]        = {{3183, 4095, 3146, 3024, 2800, 2949}},
        [GEAR_4]        = {{3020, 3078, 2973, 3188, 4046, 3067}},
        [GEAR_5]        = {{3008, 3747, 3727, 3045, 2823, 2942}},
        [GEAR_6]        = {{3062, 3103, 2942, 3008, 3538, 3664}},
        [GEAR_SEQ_UP]   = {{2970, 3016, 2923, 3082, 3321, 2940}},
        [GEAR_SEQ_DOWN] = {{3037, 3942, 2998, 2991, 2789, 2924}},
    },
    .modes = {
        [GEAR_NEUTRAL]  = GEAR_MODE_NONE,
        [GEAR_1]        = GEAR_MODE_H,
        [GEAR_2]        = GEAR_MODE_H,
        [GEAR_3]        = GEAR_MODE_H,
        [GEAR_4]        = GEAR_MODE_H,
        [GEAR_5]        = GEAR_MODE_H,
        [GEAR_6]        = GEAR_MODE_H,
        [GEAR_SEQ_UP]   = GEAR_MODE_SEQ,
        [GEAR_SEQ_DOWN] = GEAR_MODE_SEQ,
    },
    /* neutral[]/scale[] recalcules par gear_position_compute_normalization
     * avant toute utilisation (voir calibration_store_defaults) ; les
     * zeros ici ne sont jamais utilises tels quels. */
    .neutral = {0},
    .scale = {0},
};

void calibration_store_defaults(gear_calibration_t *out)
{
    memcpy(out, &DEFAULT_CALIBRATION, sizeof(*out));
    gear_position_compute_normalization(out);
}

bool calibration_store_load(gear_calibration_t *out)
{
    calibration_store_defaults(out);

    hall_sample_t flash_positions[GEAR_POSITION_COUNT];
    if (!calibration_flash_load(flash_positions)) {
        return false;
    }

    memcpy(out->positions, flash_positions, sizeof(flash_positions));
    gear_position_compute_normalization(out);
    return true;
}

void calibration_store_commit(gear_calibration_t *calibration, const hall_sample_t positions[GEAR_POSITION_COUNT])
{
    memcpy(calibration->positions, positions, sizeof(calibration->positions));
    gear_position_compute_normalization(calibration);

    /* Une seule sauvegarde (un slot) pour la calibration complete. Si
     * l'ecriture echoue (flash usee/erreur materielle), la calibration
     * reste valide en RAM pour la session en cours ; seule la persistance
     * entre reboots est perdue — pas de remontee d'erreur a l'appelant
     * (CalibrationMode n'a pas de mecanisme pour la signaler a
     * l'utilisateur, cf conception Phase 5). */
    calibration_flash_save(calibration->positions);
}
