#include "mode_state_machine.h"
#include "calibration_store.h"

#include <string.h>

const gear_position_t CALIBRATION_SEQUENCE[CALIBRATION_SEQUENCE_LEN] = {
    GEAR_1, GEAR_2, GEAR_3, GEAR_4, GEAR_5, GEAR_6, GEAR_SEQ_UP, GEAR_SEQ_DOWN, GEAR_NEUTRAL,
};

static const gear_position_t CALIBRATION_ENTRY_SEQUENCE[4] = {GEAR_1, GEAR_5, GEAR_6, GEAR_2};

static hall_sample_t mean_of(const hall_sample_t *samples, int count)
{
    hall_sample_t out = {0};
    if (count <= 0) {
        return out;
    }
    uint32_t sum[HALL_SENSOR_COUNT] = {0};
    for (int s = 0; s < count; s++) {
        for (int i = 0; i < HALL_SENSOR_COUNT; i++) {
            sum[i] += samples[s].v[i];
        }
    }
    for (int i = 0; i < HALL_SENSOR_COUNT; i++) {
        out.v[i] = (uint16_t)(sum[i] / (uint32_t)count);
    }
    return out;
}

/* Moyenne combinee de deux tampons (accum_pending + accum_rising), utilisee
 * pour la confirmation SEQ (voir mode_state_machine.py MovementTracker,
 * confirmation SEQ : tout l'episode bref appartient au vrai mouvement). */
static hall_sample_t mean_of_two(const hall_sample_t *a, int a_count, const hall_sample_t *b, int b_count)
{
    hall_sample_t out = {0};
    int count = a_count + b_count;
    if (count <= 0) {
        return out;
    }
    uint32_t sum[HALL_SENSOR_COUNT] = {0};
    for (int s = 0; s < a_count; s++) {
        for (int i = 0; i < HALL_SENSOR_COUNT; i++) {
            sum[i] += a[s].v[i];
        }
    }
    for (int s = 0; s < b_count; s++) {
        for (int i = 0; i < HALL_SENSOR_COUNT; i++) {
            sum[i] += b[s].v[i];
        }
    }
    for (int i = 0; i < HALL_SENSOR_COUNT; i++) {
        out.v[i] = (uint16_t)(sum[i] / (uint32_t)count);
    }
    return out;
}

/* ==================== MovementTracker ==================== */

void movement_tracker_init(movement_tracker_t *tracker, const gear_calibration_t *calibration)
{
    memset(tracker, 0, sizeof(*tracker));
    tracker->calibration = calibration;
    tracker->state = TRACKER_IDLE;
    tracker->last_confirmed_pos = GEAR_NONE;
}

bool movement_tracker_is_idle(const movement_tracker_t *tracker)
{
    return tracker->state == TRACKER_IDLE;
}

static void accum_push(hall_sample_t *buf, int *count, const hall_sample_t *sample)
{
    if (*count < MOVEMENT_ACCUM_MAX) {
        buf[*count] = *sample;
        (*count)++;
    }
    /* Au-dela de MOVEMENT_ACCUM_MAX, on sature silencieusement : un
     * mouvement aussi long n'arrive pas en usage normal (voir borne
     * choisie dans mode_state_machine.h), la moyenne resterait de toute
     * facon representative avec les echantillons deja accumules. */
}

static void held_window_push(movement_tracker_t *tracker, const hall_sample_t *sample)
{
    if (tracker->held_window_count < HELD_WINDOW) {
        tracker->held_window[tracker->held_window_count++] = *sample;
    } else {
        /* Fenetre glissante : decale et ajoute a la fin. */
        for (int i = 1; i < HELD_WINDOW; i++) {
            tracker->held_window[i - 1] = tracker->held_window[i];
        }
        tracker->held_window[HELD_WINDOW - 1] = *sample;
    }
}

movement_event_t movement_tracker_step(movement_tracker_t *tracker, uint32_t t_ms, const hall_sample_t *hall)
{
    movement_event_t confirmed = {.pos = GEAR_NONE, .kind = MOVEMENT_KIND_NONE};
    bool neutral_now = gear_position_is_neutral(hall, tracker->calibration);

    switch (tracker->state) {
    case TRACKER_IDLE:
        if (!neutral_now) {
            tracker->state = TRACKER_PENDING;
            tracker->accum_pending_count = 0;
            accum_push(tracker->accum_pending, &tracker->accum_pending_count, hall);
        }
        break;

    case TRACKER_PENDING:
        if (neutral_now) {
            tracker->state = TRACKER_IDLE;
            tracker->accum_pending_count = 0;
        } else {
            accum_push(tracker->accum_pending, &tracker->accum_pending_count, hall);
            if (tracker->accum_pending_count >= CONFIRM_SAMPLES) {
                tracker->state = TRACKER_RISING;
                tracker->accum_rising_count = 0;
                tracker->t_rise_start = t_ms;
            }
        }
        break;

    case TRACKER_RISING:
        accum_push(tracker->accum_rising, &tracker->accum_rising_count, hall);
        if (neutral_now) {
            /* Confirmation SEQ : moyenne sur accum_pending + accum_rising
             * (mouvement bref, tout l'episode appartient au vrai
             * mouvement — cf correctif "suite 9" dans le prototype). */
            hall_sample_t mean = mean_of_two(tracker->accum_pending, tracker->accum_pending_count,
                                              tracker->accum_rising, tracker->accum_rising_count);
            confirmed.pos = gear_position_classify(&mean, tracker->calibration, NULL);
            confirmed.kind = MOVEMENT_KIND_SEQ;
            tracker->state = TRACKER_IDLE;
            tracker->accum_rising_count = 0;
        } else if ((t_ms - tracker->t_rise_start) >= HOLD_TIMEOUT_MS) {
            /* Confirmation HELD : moyenne sur accum_rising SEUL (exclut
             * accum_pending, encore proche du neutre par construction —
             * cf correctif "suite 9"). */
            hall_sample_t mean = mean_of(tracker->accum_rising, tracker->accum_rising_count);
            confirmed.pos = gear_position_classify(&mean, tracker->calibration, NULL);
            confirmed.kind = MOVEMENT_KIND_HELD;
            tracker->state = TRACKER_HELD;
            tracker->last_confirmed_pos = confirmed.pos;

            tracker->held_window_count = 0;
            int start = tracker->accum_rising_count > HELD_WINDOW ? tracker->accum_rising_count - HELD_WINDOW : 0;
            for (int i = start; i < tracker->accum_rising_count; i++) {
                held_window_push(tracker, &tracker->accum_rising[i]);
            }
        }
        break;

    case TRACKER_HELD:
        if (neutral_now) {
            tracker->state = TRACKER_IDLE;
            tracker->accum_rising_count = 0;
            tracker->held_window_count = 0;
            tracker->last_confirmed_pos = GEAR_NONE;
        } else {
            held_window_push(tracker, hall);
            hall_sample_t mean = mean_of(tracker->held_window, tracker->held_window_count);
            gear_position_t pos = gear_position_classify(&mean, tracker->calibration, NULL);
            if (pos != tracker->last_confirmed_pos) {
                confirmed.pos = pos;
                confirmed.kind = MOVEMENT_KIND_HELD_CORRECTED;
                tracker->last_confirmed_pos = pos;
            }
        }
        break;
    }

    return confirmed;
}

/* ==================== CalibrationEntryDetector ==================== */

void calibration_entry_detector_init(calibration_entry_detector_t *det)
{
    det->index = 0;
}

bool calibration_entry_detector_step(calibration_entry_detector_t *det, int collar, gear_position_t confirmed_pos,
                                      movement_kind_t confirmed_kind)
{
    if (collar != 1) {
        det->index = 0;
        return false;
    }

    /* GX100 (2026-09-13, meme famille de bug que switch_gesture_detector
     * gear6, trouvee en test terrain) : accepte aussi HELD_CORRECTED, pas
     * seulement HELD/SEQ. Une reconfirmation de la meme position (ex. un
     * second maintien de gear1) arrive typiquement en HELD_CORRECTED, pas
     * HELD — sans ce cas, une telle reconfirmation etait silencieusement
     * ignoree (ni avancee ni reset de l'index), pouvant laisser l'index
     * bloque et faire echouer toute la sequence d'entree malgre un geste
     * par ailleurs correct (confirme sur capture reelle test_H_pattern4). */
    if (confirmed_pos == GEAR_NONE ||
        (confirmed_kind != MOVEMENT_KIND_HELD && confirmed_kind != MOVEMENT_KIND_SEQ &&
         confirmed_kind != MOVEMENT_KIND_HELD_CORRECTED)) {
        return false;
    }

    /* GX100 (2026-09-13, bug trouve en test terrain sur la meme capture,
     * generalise apres un premier fix cible sur GEAR_NEUTRAL) : une
     * correction HELD_CORRECTED qui ne fait pas avancer la sequence peut
     * viser n'importe quelle position transitoire (pas seulement neutral —
     * confirme sur la meme capture : "seq_up" transitoire juste avant la
     * confirmation finale de gear6), artefact connu de la re-evaluation
     * continue en HELD pendant la montee vers la position suivante (le
     * tracker reste en TRACKER_HELD, ce n'est pas un vrai changement de
     * position). Une telle correction ne doit donc ni faire avancer la
     * sequence ni la reinitialiser : on l'ignore purement. Seule une
     * confirmation FRANCHE (HELD ou SEQ, pas une correction) hors sequence,
     * ou le relachement de la collerette (collar != 1, deja gere plus
     * haut), reste un vrai reset — cf la branche else ci-dessous, qui ne
     * s'applique plus qu'a HELD/SEQ grace au filtre juste en dessous. */
    if (confirmed_kind == MOVEMENT_KIND_HELD_CORRECTED && confirmed_pos != CALIBRATION_ENTRY_SEQUENCE[det->index] &&
        confirmed_pos != CALIBRATION_ENTRY_SEQUENCE[0]) {
        return false;
    }

    if (confirmed_pos == CALIBRATION_ENTRY_SEQUENCE[det->index]) {
        det->index++;
        if (det->index >= 4) {
            det->index = 0;
            return true;
        }
    } else if (confirmed_pos == CALIBRATION_ENTRY_SEQUENCE[0]) {
        det->index = 1;
    } else {
        det->index = 0;
    }

    return false;
}

/* ==================== CalibrationMode ==================== */

void calibration_mode_init(calibration_mode_t *cal, calibration_commit_cb_t on_commit, void *on_commit_ctx)
{
    memset(cal, 0, sizeof(*cal));
    cal->on_commit = on_commit;
    cal->on_commit_ctx = on_commit_ctx;
}

gear_position_t calibration_mode_current_target(const calibration_mode_t *cal)
{
    if (!cal->active || cal->step_index >= CALIBRATION_SEQUENCE_LEN) {
        return GEAR_NONE;
    }
    return CALIBRATION_SEQUENCE[cal->step_index];
}

void calibration_mode_enter(calibration_mode_t *cal, const hall_sample_t *ref_neutral)
{
    cal->active = true;
    cal->step_index = 0;
    cal->ref_neutral = *ref_neutral;
    cal->rejected_count = 0;
    cal->collar_prev = 0;
    cal->collar_rise_valid = false;
    memset(cal->accum_sum, 0, sizeof(cal->accum_sum));
    cal->accum_count = 0;
    cal->triggered_for_this_hold = false;
}

static uint32_t hall_l1(const hall_sample_t *a, const hall_sample_t *b)
{
    uint32_t d = 0;
    for (int i = 0; i < HALL_SENSOR_COUNT; i++) {
        d += (a->v[i] > b->v[i]) ? (uint32_t)(a->v[i] - b->v[i]) : (uint32_t)(b->v[i] - a->v[i]);
    }
    return d;
}

bool calibration_mode_rejects(const calibration_mode_t *cal, const hall_sample_t *mean)
{
    gear_position_t target = calibration_mode_current_target(cal);
    if (target != GEAR_NEUTRAL && hall_l1(mean, &cal->ref_neutral) < CALIBRATION_MIN_NEUTRAL_L1) {
        return true;
    }
    for (int k = 0; k < cal->step_index; k++) {
        gear_position_t recorded = CALIBRATION_SEQUENCE[k];
        bool close_pair = (target == GEAR_SEQ_DOWN && recorded == GEAR_3) ||
                          (target == GEAR_3 && recorded == GEAR_SEQ_DOWN);
        uint32_t min_gap = close_pair ? CALIBRATION_MIN_GAP_CLOSE_L1 : CALIBRATION_MIN_GAP_L1;
        if (hall_l1(mean, &cal->pending[recorded]) < min_gap) {
            return true;
        }
    }
    /* Sens du levier sequentiel, par rapport aux 3e et 4e de la session
     * (enregistrees avant dans CALIBRATION_SEQUENCE). */
    uint32_t to_3rd = hall_l1(mean, &cal->pending[GEAR_3]);
    uint32_t to_4th = hall_l1(mean, &cal->pending[GEAR_4]);
    if (target == GEAR_SEQ_UP && to_3rd < to_4th) {
        return true;
    }
    if (target == GEAR_SEQ_DOWN && to_4th < to_3rd) {
        return true;
    }
    return false;
}

void calibration_mode_step(calibration_mode_t *cal, uint32_t t_ms, const hall_sample_t *hall, int collar)
{
    if (!cal->active) {
        return;
    }

    bool collar_falling_edge = (collar == 0 && cal->collar_prev == 1);
    if (collar == 1 && cal->collar_prev == 0) {
        cal->collar_rise_t = t_ms;
        cal->collar_rise_valid = true;
        memset(cal->accum_sum, 0, sizeof(cal->accum_sum));
        cal->accum_count = 0;
        cal->triggered_for_this_hold = false;
    }
    cal->collar_prev = collar;

    if (collar == 1) {
        if (cal->collar_rise_valid && (t_ms - cal->collar_rise_t) >= CALIBRATION_HOLD_MS) {
            for (int i = 0; i < HALL_SENSOR_COUNT; i++) {
                cal->accum_sum[i] += (double)hall->v[i];
            }
            cal->accum_count++;
            cal->triggered_for_this_hold = true;
        }
    } else if (collar_falling_edge) {
        if (cal->triggered_for_this_hold && cal->accum_count > 0) {
            hall_sample_t mean = {0};
            for (int i = 0; i < HALL_SENSOR_COUNT; i++) {
                mean.v[i] = (uint16_t)(cal->accum_sum[i] / (double)cal->accum_count);
            }
            if (calibration_mode_rejects(cal, &mean)) {
                /* Levier au repos ou sur un point deja enregistre : on
                 * reste sur la meme etape (motif LED de refus). */
                cal->rejected_count++;
            } else {
                /* Garde en RAM ; rien n'est applique ni ecrit en flash
                 * avant le dernier point (cf calibration_commit_cb_t). */
                cal->pending[calibration_mode_current_target(cal)] = mean;
                cal->step_index++;
                if (cal->step_index >= CALIBRATION_SEQUENCE_LEN) {
                    cal->active = false;
                    if (cal->on_commit != NULL) {
                        cal->on_commit(cal->on_commit_ctx, cal->pending);
                    }
                }
            }
        }
        /* sinon : relachement avant CALIBRATION_HOLD_MS, tentative
         * annulee silencieusement, on reste sur le meme point. */
        memset(cal->accum_sum, 0, sizeof(cal->accum_sum));
        cal->accum_count = 0;
        cal->triggered_for_this_hold = false;
        cal->collar_rise_valid = false;
    }
}

/* ==================== SwitchGestureDetector ==================== */

void switch_gesture_detector_init(switch_gesture_detector_t *det)
{
    memset(det, 0, sizeof(*det));
}

static void register_blip(blip_t *blips, int *count, uint32_t start, uint32_t end)
{
    uint32_t duration = end - start;
    if (duration < BLIP_MIN_MS || duration > BLIP_MAX_MS) {
        return;
    }
    if (*count < GESTURE_BLIP_MAX) {
        blips[*count].start = start;
        blips[*count].end = end;
        (*count)++;
    } else {
        /* Decale (FIFO) pour garder les blips les plus recents. */
        for (int i = 1; i < GESTURE_BLIP_MAX; i++) {
            blips[i - 1] = blips[i];
        }
        blips[GESTURE_BLIP_MAX - 1].start = start;
        blips[GESTURE_BLIP_MAX - 1].end = end;
    }
}

/* Purge les blips trop vieux et teste si les 2 derniers forment une paire
 * valide (ecart entre eux <= INTER_BLIP_MAX_MS, le tout dans la fenetre
 * GESTURE_TIMEOUT_MS depuis window_start_t).
 *
 * GX100 (2026-09-13, bug trouve en test terrain) : la purge ne filtrait
 * qu'au global GESTURE_TIMEOUT_MS (relatif a now_t), jamais par rapport a
 * INTER_BLIP_MAX_MS — un blip isole tres ancien (ex. une premiere
 * tentative de gear6 ratee, blip enregistre puis le levier relache et
 * retente plusieurs secondes apres, cf fix "gear6_currently_held" qui ne
 * remet plus gear6_blip_count a 0 sur un simple flottement) restait donc
 * indefiniment le "blip precedent" candidat, meme des secondes plus tard,
 * bloquant toute vraie paire de blips rapproches de matcher (confirme sur
 * capture reelle test_H_pattern3 : count passait bien a 2 mais
 * check_two_blips renvoyait false a chaque fois, ecart b2.start-b1.end
 * >> INTER_BLIP_MAX_MS a cause d'un premier blip vieux de plusieurs
 * secondes). Fix : purge aussi tout blip dont l'ecart avec le PLUS RECENT
 * depasse INTER_BLIP_MAX_MS, pour qu'il ne pollue plus les evaluations
 * suivantes une fois devenu trop vieux pour faire une paire. */
static bool check_two_blips(blip_t *blips, int *count, uint32_t window_start_t, uint32_t now_t)
{
    int kept = 0;
    for (int i = 0; i < *count; i++) {
        if (now_t - blips[i].start <= GESTURE_TIMEOUT_MS) {
            blips[kept++] = blips[i];
        }
    }
    *count = kept;

    if (*count >= 2) {
        uint32_t most_recent_start = blips[*count - 1].start;
        int kept2 = 0;
        for (int i = 0; i < *count; i++) {
            if ((most_recent_start - blips[i].end) <= INTER_BLIP_MAX_MS || i == *count - 1) {
                blips[kept2++] = blips[i];
            }
        }
        *count = kept2;
    }

    if (now_t - window_start_t > GESTURE_TIMEOUT_MS) {
        return false;
    }
    if (*count >= 2) {
        blip_t b1 = blips[*count - 2];
        blip_t b2 = blips[*count - 1];
        if ((b2.start - b1.end) <= INTER_BLIP_MAX_MS) {
            return true;
        }
    }
    return false;
}

void switch_gesture_detector_step(switch_gesture_detector_t *det, uint32_t t_ms, int collar, bool tracker_is_idle,
                                   gear_position_t confirmed_pos, movement_kind_t confirmed_kind)
{
    det->last_event = GESTURE_TARGET_NONE;

    /* --- Front collerette (0->1->0) --- */
    bool blip_completed = false;
    uint32_t blip_start = 0, blip_end = 0;
    if (collar == 1 && det->collar_prev == 0) {
        det->collar_rise_t = t_ms;
        det->collar_rise_valid = true;
    } else if (collar == 0 && det->collar_prev == 1 && det->collar_rise_valid) {
        blip_completed = true;
        blip_start = det->collar_rise_t;
        blip_end = t_ms;
        det->collar_rise_valid = false;
    }
    det->collar_prev = collar;

    /* --- Sous-detecteur A : maintien gear6 ---
     * GX100 (2026-09-13, deux bugs trouves en test terrain) :
     * 1) Accepte aussi HELD_CORRECTED, pas seulement HELD. La confirmation
     *    initiale d'un mouvement classe parfois a tort "neutral" en HELD
     *    (accum_rising encore trop court, cf PLAN.md Phase 5 suite 10 point
     *    3), corrigee au pas suivant vers gear6 en HELD_CORRECTED — sans ce
     *    cas, gear6_active ne s'activait jamais quand la confirmation
     *    initiale ratait gear6.
     * 2) Les blips deja valides ne sont plus perdus si le levier ressort
     *    transitoirement de gear6 entre les deux blips (observe en test
     *    terrain : le maintien manuel du levier pendant la manipulation de
     *    la collerette n'est pas parfaitement stable, le retour au neutre a
     *    dure plus que HELD_TOLERANCE_SAMPLES). On distingue desormais
     *    "geste en cours" (gear6_active, dans la fenetre GESTURE_TIMEOUT_MS
     *    depuis le premier maintien) de "levier actuellement sur gear6"
     *    (gear6_currently_held) : un nouveau blip n'est accepte que si le
     *    levier est effectivement sur gear6 a cet instant, mais le geste
     *    entier (et les blips deja comptes) ne s'annule que par expiration
     *    du timeout, jamais par un simple flottement du levier. */
    bool gear6_reconfirmed = (confirmed_kind == MOVEMENT_KIND_HELD || confirmed_kind == MOVEMENT_KIND_HELD_CORRECTED) &&
                              confirmed_pos == GEAR_6;
    if (gear6_reconfirmed) {
        det->gear6_currently_held = true;
        /* GX100 (2026-09-13, bug trouve en test terrain) : gear6_t_start
         * doit repartir a CHAQUE reconfirmation, pas seulement la premiere
         * fois que gear6_active passe a true — sinon, avec plusieurs
         * tentatives de maintien etalees dans le temps (le levier ressort
         * de gear6 puis y revient, cf point 2 ci-dessus), le chrono du
         * timeout continuait de courir depuis la toute premiere tentative,
         * expirant presque a coup sur avant qu'une tentative recente n'ait
         * pu accumuler ses 2 blips (confirme sur capture reelle
         * test_H_pattern3 : timeout expire a 5086ms mesures depuis la
         * 1ere confirmation, alors que la tentative en cours avait a peine
         * commence). Chaque tentative recente doit disposer du budget
         * complet GESTURE_TIMEOUT_MS, le nombre de tentatives n'a pas
         * besoin d'etre borne (l'utilisateur est hors course pendant la
         * bascule de mode). */
        det->gear6_t_start = t_ms;
        if (!det->gear6_active) {
            det->gear6_active = true;
            det->gear6_blip_count = 0;
        }
    } else if (tracker_is_idle || confirmed_pos != GEAR_NONE) {
        /* Le levier a quitte gear6 (neutre ou autre position confirmee) :
         * le geste reste "actif" (blips conserves), seul le fait d'accepter
         * de NOUVEAUX blips est suspendu tant qu'on n'est pas revenu sur
         * gear6. */
        det->gear6_currently_held = false;
    }

    if (det->gear6_active) {
        if ((t_ms - det->gear6_t_start) > GESTURE_TIMEOUT_MS) {
            det->gear6_active = false;
            det->gear6_currently_held = false;
            det->gear6_blip_count = 0;
        } else if (det->gear6_currently_held && blip_completed) {
            register_blip(det->gear6_blips, &det->gear6_blip_count, blip_start, blip_end);
            bool matched = check_two_blips(det->gear6_blips, &det->gear6_blip_count, det->gear6_t_start, t_ms);
            det->gear6_t_start = t_ms; /* renouvelle le budget a partir de ce blip, meme raisonnement que le sous-detecteur B */
            if (matched) {
                det->last_event = GESTURE_TARGET_H;
                det->last_event_t = t_ms;
                det->gear6_active = false;
                det->gear6_currently_held = false;
                det->gear6_blip_count = 0;
            }
        }
    }

    /* --- Sous-detecteur B : N x seq_down puis neutre puis 2 blips ---
     * GX100 (2026-09-13, meme famille de bug que le sous-detecteur A
     * gear6, trouvee en test terrain sur la meme capture) :
     * seqdown_window_start_t ne repartait qu'a l'OUVERTURE de la fenetre
     * (premier seq_down apres expiration), jamais renouvele ensuite — le
     * budget GESTURE_TIMEOUT_MS courait donc depuis le tout premier
     * seq_down, pas depuis l'evenement le plus recent. Confirme sur
     * capture reelle : fenetre ouverte a t=36682, timeout expire a
     * t=41734 (5052ms), juste apres le 1er blip (t=37694), avant qu'un
     * 2eme blip n'ait pu arriver — le geste SEQ echouait donc pour la
     * meme raison de fond que gear6. Fix : seqdown_window_start_t repart a
     * chaque nouveau seq_down valide ET a chaque blip enregistre, pour que
     * le budget complet reste disponible depuis le dernier evenement
     * pertinent plutot que le tout premier. */
    if (confirmed_kind == MOVEMENT_KIND_SEQ && confirmed_pos == GEAR_SEQ_DOWN) {
        if (!det->seqdown_window_active || (t_ms - det->seqdown_window_start_t) > GESTURE_TIMEOUT_MS) {
            det->seqdown_window_active = true;
            det->seqdown_count = 1;
            det->seqdown_blip_count = 0;
        } else {
            det->seqdown_count++;
        }
        det->seqdown_window_start_t = t_ms;
    }

    if (det->seqdown_window_active) {
        if (t_ms - det->seqdown_window_start_t > GESTURE_TIMEOUT_MS) {
            det->seqdown_window_active = false;
            det->seqdown_count = 0;
            det->seqdown_blip_count = 0;
        } else if (det->seqdown_count >= SEQDOWN_MIN_COUNT && tracker_is_idle && blip_completed) {
            register_blip(det->seqdown_blips, &det->seqdown_blip_count, blip_start, blip_end);
            bool matched = check_two_blips(det->seqdown_blips, &det->seqdown_blip_count, det->seqdown_window_start_t, t_ms);
            det->seqdown_window_start_t = t_ms; /* renouvelle le budget a partir de ce blip */
            if (matched) {
                /* Ne pas ecraser un evenement H deja emis ce pas (ne peut
                 * pas arriver en pratique : les deux sous-detecteurs ne
                 * se declenchent jamais sur le meme mouvement confirme,
                 * mais on reste explicite). */
                if (det->last_event == GESTURE_TARGET_NONE) {
                    det->last_event = GESTURE_TARGET_SEQ;
                    det->last_event_t = t_ms;
                }
                det->seqdown_window_active = false;
                det->seqdown_count = 0;
                det->seqdown_blip_count = 0;
            }
        }
    }
}

/* ==================== ModeStateMachine ==================== */

static void on_calibration_commit(void *ctx, const hall_sample_t positions[GEAR_POSITION_COUNT])
{
    mode_state_machine_t *msm = (mode_state_machine_t *)ctx;
    /* Remplace les 9 points et recalcule neutral/scale, puis une seule
     * sauvegarde flash. tracker/gesture pointent vers msm->calibration (la
     * meme instance), donc le changement est visible partout des le pas
     * suivant. */
    calibration_store_commit(&msm->calibration, positions);
}

void mode_state_machine_init(mode_state_machine_t *msm, const gear_calibration_t *calibration)
{
    memset(msm, 0, sizeof(*msm));
    msm->calibration = *calibration;

    movement_tracker_init(&msm->tracker, &msm->calibration);
    switch_gesture_detector_init(&msm->gesture);
    calibration_entry_detector_init(&msm->calib_entry);
    calibration_mode_init(&msm->calib_mode, on_calibration_commit, msm);

    msm->mode_courant = GEAR_MODE_H; /* defaut au demarrage */
    msm->last_emitted.pos = GEAR_NONE;
    msm->last_emitted.kind = MOVEMENT_KIND_NONE;
    msm->last_switch_applied = false;
    msm->last_switch_target = GESTURE_TARGET_NONE;
}

void mode_state_machine_step(mode_state_machine_t *msm, uint32_t t_ms, const hall_sample_t *hall, int collar)
{
    msm->last_emitted.pos = GEAR_NONE;
    msm->last_emitted.kind = MOVEMENT_KIND_NONE;
    msm->last_switch_applied = false;
    msm->last_switch_target = GESTURE_TARGET_NONE;

    if (msm->calib_mode.active) {
        /* Mode calibration : aucun input HID, pas de classification
         * normale ni de detection de bascule pendant ce mode. */
        calibration_mode_step(&msm->calib_mode, t_ms, hall, collar);
        return;
    }

    movement_event_t ev = movement_tracker_step(&msm->tracker, t_ms, hall);

    /* Verifie l'entree en mode calibration AVANT toute emission HID : le
     * dernier mouvement de la sequence d'entree (gear2, collerette levee)
     * ne doit jamais partir en jeu. */
    if (calibration_entry_detector_step(&msm->calib_entry, collar, ev.pos, ev.kind)) {
        calibration_mode_enter(&msm->calib_mode, &msm->calibration.positions[GEAR_NEUTRAL]);
        return;
    }

    if (ev.pos != GEAR_NONE) {
        gear_mode_t intrinsic = msm->calibration.modes[ev.pos];
        if (intrinsic == msm->mode_courant) {
            msm->last_emitted = ev;
        }
    }

    switch_gesture_detector_step(&msm->gesture, t_ms, collar, movement_tracker_is_idle(&msm->tracker), ev.pos,
                                  ev.kind);
    if (msm->gesture.last_event != GESTURE_TARGET_NONE) {
        gear_mode_t target = (msm->gesture.last_event == GESTURE_TARGET_H) ? GEAR_MODE_H : GEAR_MODE_SEQ;
        if (target != msm->mode_courant) {
            msm->mode_courant = target;
            msm->last_switch_applied = true;
            msm->last_switch_target = msm->gesture.last_event;
        }
        /* Si le mode cible == mode deja actif : ne rien faire (cf conception). */
    }
}
