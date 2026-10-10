#include "fast_gear.h"

#include <math.h>
#include <string.h>

void fast_gear_init(fast_gear_t *fg)
{
    fg->mode = GEAR_MODE_NONE;
    fg->wait_neutral = false;
    fg->engaged = GEAR_NONE;
    fg->candidate = GEAR_NONE;
    fg->candidate_t = 0;
    fg->leaving = false;
    fg->leave_t = 0;
    fg->ref_valid = false;
}

static void refresh_refs(fast_gear_t *fg, const gear_calibration_t *calibration)
{
    if (fg->ref_valid && memcmp(fg->ref_key, calibration->positions, sizeof(fg->ref_key)) == 0) {
        return;
    }
    memcpy(fg->ref_key, calibration->positions, sizeof(fg->ref_key));
    for (int p = 0; p < GEAR_POSITION_COUNT; p++) {
        fg->ref_norm[p] = gear_position_normalize(&calibration->positions[p], calibration);
    }
    fg->ref_valid = true;
}

static float distance_to(const hall_norm_t *norm, const hall_norm_t *ref)
{
    float acc = 0.0f;
    for (int i = 0; i < HALL_SENSOR_COUNT; i++) {
        float d = norm->v[i] - ref->v[i];
        acc += d * d;
    }
    return sqrtf(acc);
}

gear_position_t fast_gear_step(fast_gear_t *fg, uint32_t t_ms, const hall_sample_t *hall,
                               const gear_calibration_t *calibration, gear_mode_t mode)
{
    if (mode != fg->mode) {
        /* Pas d'attente au tout premier appel (demarrage) : seulement sur
         * un vrai changement de mode (cf fast_gear.h). */
        bool mode_switch = (fg->mode != GEAR_MODE_NONE);
        fast_gear_init(fg);
        fg->mode = mode;
        fg->wait_neutral = mode_switch;
    }

    refresh_refs(fg, calibration);
    hall_norm_t norm = gear_position_normalize(hall, calibration);
    float dist[GEAR_POSITION_COUNT];
    gear_position_t nearest = GEAR_NEUTRAL;
    float second_dist = INFINITY;
    for (int p = 0; p < GEAR_POSITION_COUNT; p++) {
        dist[p] = distance_to(&norm, &fg->ref_norm[p]);
        /* En SEQ, les points H sont hors d'atteinte (levier verrouille) et
         * un seq_down appuye se rapproche de gear3 : ils ne concourent pas.
         * En H, tout concourt (seq_down du geste de bascule != gear3). */
        if (mode == GEAR_MODE_SEQ && calibration->modes[p] == GEAR_MODE_H) {
            continue;
        }
        if (p == GEAR_NEUTRAL) {
            continue; /* deja initialise comme plus proche */
        }
        if (dist[p] < dist[nearest]) {
            second_dist = dist[nearest];
            nearest = (gear_position_t)p;
        } else if (dist[p] < second_dist) {
            second_dist = dist[p];
        }
    }

    if (fg->wait_neutral) {
        if (dist[GEAR_NEUTRAL] < GEAR_NEUTRAL_DIST_THRESHOLD) {
            fg->wait_neutral = false;
        }
        return GEAR_NONE;
    }

    /* --- Engagement (ou changement direct de position sans repasser par un
     * etat relache) : candidat stable FAST_GEAR_ENGAGE_MS. */
    bool engageable = nearest != GEAR_NEUTRAL && calibration->modes[nearest] == mode &&
                      dist[nearest] < FAST_GEAR_ENGAGE_DIST && dist[GEAR_NEUTRAL] > FAST_GEAR_MIN_NEUTRAL_DIST &&
                      (second_dist - dist[nearest]) >= FAST_GEAR_MARGIN;
    if (engageable && nearest != fg->engaged) {
        if (fg->candidate != nearest) {
            fg->candidate = nearest;
            fg->candidate_t = t_ms;
        }
        if ((t_ms - fg->candidate_t) >= FAST_GEAR_ENGAGE_MS) {
            fg->engaged = nearest;
            fg->candidate = GEAR_NONE;
            fg->leaving = false;
        }
    } else {
        fg->candidate = GEAR_NONE;
    }

    /* --- Relachement : levier hors grille de la position engagee, ou revenu
     * au neutre, depuis FAST_GEAR_RELEASE_MS. */
    if (fg->engaged != GEAR_NONE) {
        bool out = dist[fg->engaged] > FAST_GEAR_RELEASE_DIST || dist[GEAR_NEUTRAL] < GEAR_NEUTRAL_DIST_THRESHOLD;
        if (out) {
            if (!fg->leaving) {
                fg->leaving = true;
                fg->leave_t = t_ms;
            }
            if ((t_ms - fg->leave_t) >= FAST_GEAR_RELEASE_MS) {
                fg->engaged = GEAR_NONE;
                fg->leaving = false;
            }
        } else {
            fg->leaving = false;
        }
    }

    return fg->engaged;
}
