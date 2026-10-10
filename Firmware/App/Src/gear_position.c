#include "gear_position.h"

#include <math.h>
#include <stddef.h>

static const char *const POSITION_NAMES[GEAR_POSITION_COUNT] = {
    [GEAR_NEUTRAL]   = "neutral",
    [GEAR_1]         = "gear1",
    [GEAR_2]         = "gear2",
    [GEAR_3]         = "gear3",
    [GEAR_4]         = "gear4",
    [GEAR_5]         = "gear5",
    [GEAR_6]         = "gear6",
    [GEAR_SEQ_UP]    = "seq_up",
    [GEAR_SEQ_DOWN]  = "seq_down",
};

const char *gear_position_name(gear_position_t pos)
{
    if (pos < 0 || pos >= GEAR_POSITION_COUNT) {
        return "none";
    }
    return POSITION_NAMES[pos];
}

void gear_position_compute_normalization(gear_calibration_t *calibration)
{
    const hall_sample_t *neutral = &calibration->positions[GEAR_NEUTRAL];

    for (int i = 0; i < HALL_SENSOR_COUNT; i++) {
        calibration->neutral[i] = (float)neutral->v[i];
        calibration->scale[i] = 0.0f;
    }

    for (int p = 0; p < GEAR_POSITION_COUNT; p++) {
        if (p == GEAR_NEUTRAL) {
            continue;
        }
        const hall_sample_t *ref = &calibration->positions[p];
        for (int i = 0; i < HALL_SENSOR_COUNT; i++) {
            float d = fabsf((float)ref->v[i] - calibration->neutral[i]);
            if (d > calibration->scale[i]) {
                calibration->scale[i] = d;
            }
        }
    }

    for (int i = 0; i < HALL_SENSOR_COUNT; i++) {
        if (calibration->scale[i] <= 0.0f) {
            calibration->scale[i] = 1.0f;
        }
    }
}

hall_norm_t gear_position_normalize(const hall_sample_t *hall, const gear_calibration_t *calibration)
{
    hall_norm_t out;
    for (int i = 0; i < HALL_SENSOR_COUNT; i++) {
        out.v[i] = ((float)hall->v[i] - calibration->neutral[i]) / calibration->scale[i];
    }
    return out;
}

static float squared_distance(const hall_norm_t *a, const hall_norm_t *b)
{
    float acc = 0.0f;
    for (int i = 0; i < HALL_SENSOR_COUNT; i++) {
        float d = a->v[i] - b->v[i];
        acc += d * d;
    }
    return acc;
}

bool gear_position_is_neutral(const hall_sample_t *hall, const gear_calibration_t *calibration)
{
    hall_norm_t norm = gear_position_normalize(hall, calibration);
    float acc = 0.0f;
    for (int i = 0; i < HALL_SENSOR_COUNT; i++) {
        acc += norm.v[i] * norm.v[i];
    }
    return sqrtf(acc) < GEAR_NEUTRAL_DIST_THRESHOLD;
}

gear_position_t gear_position_classify(const hall_sample_t *hall, const gear_calibration_t *calibration,
                                        float *out_distance)
{
    hall_norm_t norm = gear_position_normalize(hall, calibration);

    gear_position_t best = GEAR_NEUTRAL;
    float best_dist2 = -1.0f;

    for (int p = 0; p < GEAR_POSITION_COUNT; p++) {
        hall_norm_t ref_norm = gear_position_normalize(&calibration->positions[p], calibration);
        float dist2 = squared_distance(&norm, &ref_norm);
        if (best_dist2 < 0.0f || dist2 < best_dist2) {
            best_dist2 = dist2;
            best = (gear_position_t)p;
        }
    }

    if (out_distance != NULL) {
        *out_distance = sqrtf(best_dist2);
    }
    return best;
}
