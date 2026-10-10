/**
 * mode_state_machine.h — portage C de PCB/mode_state_machine.py.
 *
 * Assemble :
 *  - movement_tracker_t  : suivi unique de mouvement (IDLE->PENDING->
 *    RISING->HELD), classifie une seule fois sur la moyenne de l'episode.
 *  - switch_gesture_detector_t : detection des deux gestes de bascule de
 *    mode explicites (maintien gear6+2 blips ; N x seq_down+neutre+2 blips).
 *  - calibration_entry_detector_t : detection du geste d'entree en mode
 *    calibration (gear1->gear5->gear6->gear2, collerette levee).
 *  - calibration_mode_t : sequence guidee des 9 points de calibration.
 *  - mode_state_machine_t : assemble le tout, expose step() a appeler a
 *    chaque nouvel echantillon Hall/collerette.
 *
 * Voir PLAN.md Phase 5 pour l'historique complet des essais/abandons qui
 * ont mene a cette conception (bascule automatique par signature abandonnee,
 * recalibration "a la volee" abandonnee au profit du mode calibration dedie).
 *
 * Aucune dependance HAL/USB : module testable hors cible (voir App/test).
 */
#ifndef MODE_STATE_MACHINE_H
#define MODE_STATE_MACHINE_H

#include "gear_position.h"

/* --- Constantes de classification/confirmation d'un mouvement --- */
#define HOLD_TIMEOUT_MS    250u  /* redescente immediate = SEQ ; maintien au-dela = HELD */
#define CONFIRM_SAMPLES    2     /* echantillons consecutifs hors neutre avant RISING */
#define HELD_WINDOW        5     /* fenetre glissante de re-evaluation pendant HELD */

/* --- Constantes du geste de bascule de mode --- */
/* GX100 : releve de 2500 a 5000ms (2026-09-13, test terrain Phase 6) — le
 * chrono demarre a la CONFIRMATION de gear6 (pas au debut du geste), et
 * sur capture reelle (test_H_pattern2_cdc.csv) il s'ecoulait deja ~2350ms
 * entre cette confirmation et le debut du premier blip collerette,
 * laissant a peine 150ms pour les deux blips + leur intervalle — budget
 * structurellement intenable. Aucun cout de reactivite en jeu pour ce
 * geste (hors course pendant la bascule de mode), contrairement au budget
 * <250ms qui s'applique a la classification normale en cours de partie. */
#define GESTURE_TIMEOUT_MS     5000u
#define BLIP_MIN_MS            50u
/* GX100 : releve de 500 a 700ms (2026-09-13, test terrain Phase 6) — un
 * blip collerette reel mesure regulierement ~510-550ms (capture
 * test_H_pattern2_cdc.csv, plusieurs blips a 510ms rejetes de justesse par
 * l'ancien seuil de 500ms), le geste "instantane" suppose initialement
 * (cf Phase 5, calibre uniquement sur captures avec l'utilisateur absent
 * physiquement) etait trop optimiste. Marge confortable au-dessus des
 * 510ms mesures, reste tres distinct de CALIBRATION_HOLD_MS=2000ms. */
#define BLIP_MAX_MS             700u
#define INTER_BLIP_MAX_MS      1200u
#define SEQDOWN_MIN_COUNT      2

/* --- Constante du mode calibration dedie --- */
#define CALIBRATION_HOLD_MS 2000u

/* Points refuses (2026-10-05, renforce 2026-10-07) : distance L1 (somme
 * des ecarts absolus sur les 6 capteurs, en pas ADC) entre le point
 * maintenu et :
 *  - un point deja enregistre dans la session : en dessous de
 *    CALIBRATION_MIN_GAP_L1, c'est le meme rapport (levier pas bouge, ou
 *    remis dans une grille deja enregistree : calib_refus, neutre pris en
 *    1ere a 157 de la 1ere de la session). Points distincts reels : >= 700
 *    sauf gear3/seq_down, qui gardent CALIBRATION_MIN_GAP_CLOSE_L1 (ecart
 *    reel 347-516 sur 5 calibrations : defauts, oct. 2026, test_Ben,
 *    test_H_pattern8/9). Neutre <-> point le plus proche : 756-859.
 *  - le neutre de la calibration courante, pour tous les points sauf le
 *    neutre : en dessous de CALIBRATION_MIN_NEUTRAL_L1, le levier est reste
 *    au repos. Neutre re-mesure en session : 63-95 du neutre enregistre ;
 *    point reel le plus proche du neutre : seq_up, ~810.
 * Et sens du levier sequentiel (memes 5 calibrations) : seq_down est
 * toujours du cote de la 3e (L1 347-516 de la 3e, 2503-2662 de la 4e),
 * seq_up du cote de la 4e (1072-1142 de la 4e, 2069-2137 de la 3e). A
 * l'etape seq_up, un point plus proche de la 3e que de la 4e de la session
 * est refuse ; a l'etape seq_down, l'inverse (seq_up/seq_down inverses,
 * calib_refus).
 * Un point refuse n'avance pas l'etape (rejected_count++, motif LED). */
#define CALIBRATION_MIN_GAP_L1        400u
#define CALIBRATION_MIN_GAP_CLOSE_L1  150u
#define CALIBRATION_MIN_NEUTRAL_L1    300u

typedef enum {
    MOVEMENT_KIND_NONE = 0,
    MOVEMENT_KIND_SEQ,
    MOVEMENT_KIND_HELD,
    MOVEMENT_KIND_HELD_CORRECTED,
} movement_kind_t;

typedef struct {
    gear_position_t pos;   /* GEAR_NONE si aucun mouvement confirme ce pas */
    movement_kind_t kind;
} movement_event_t;

/* ---------------- MovementTracker ---------------- */

typedef enum {
    TRACKER_IDLE = 0,
    TRACKER_PENDING,
    TRACKER_RISING,
    TRACKER_HELD,
} tracker_state_t;

/* Bornes d'accumulation : a 100ms/echantillon, GESTURE_TIMEOUT_MS et les
 * mouvements les plus longs observes tiennent largement en dessous de 64
 * echantillons. Marge large pour rester correct meme a cadence
 * d'echantillonnage plus rapide. */
#define MOVEMENT_ACCUM_MAX 64

typedef struct {
    const gear_calibration_t *calibration;

    tracker_state_t state;

    hall_sample_t accum_pending[MOVEMENT_ACCUM_MAX];
    int accum_pending_count;

    hall_sample_t accum_rising[MOVEMENT_ACCUM_MAX];
    int accum_rising_count;

    hall_sample_t held_window[HELD_WINDOW];
    int held_window_count; /* jusqu'a HELD_WINDOW, puis glisse */

    uint32_t t_rise_start;
    gear_position_t last_confirmed_pos;
} movement_tracker_t;

void movement_tracker_init(movement_tracker_t *tracker, const gear_calibration_t *calibration);

/* A appeler a chaque nouvel echantillon. Retourne un evenement avec
 * pos=GEAR_NONE si rien n'est confirme a ce pas. */
movement_event_t movement_tracker_step(movement_tracker_t *tracker, uint32_t t_ms, const hall_sample_t *hall);

bool movement_tracker_is_idle(const movement_tracker_t *tracker);

/* ---------------- CalibrationEntryDetector ---------------- */

typedef struct {
    int index;
} calibration_entry_detector_t;

void calibration_entry_detector_init(calibration_entry_detector_t *det);

/* Retourne true le pas ou la sequence gear1->gear5->gear6->gear2
 * (collerette levee tout du long) vient d'etre detectee. */
bool calibration_entry_detector_step(calibration_entry_detector_t *det, int collar, gear_position_t confirmed_pos,
                                      movement_kind_t confirmed_kind);

/* ---------------- CalibrationMode ---------------- */

#define CALIBRATION_SEQUENCE_LEN 9
extern const gear_position_t CALIBRATION_SEQUENCE[CALIBRATION_SEQUENCE_LEN];

/* Appele UNE fois, apres le 9eme point, avec les 9 points de la session
 * (indexes par gear_position_t). Les points valides sont gardes en RAM
 * jusque-la : une calibration interrompue (debranchement) ne change rien,
 * ni en RAM ni en flash. */
typedef void (*calibration_commit_cb_t)(void *ctx, const hall_sample_t positions[GEAR_POSITION_COUNT]);

typedef struct {
    calibration_commit_cb_t on_commit;
    void *on_commit_ctx;

    bool active;
    int step_index;
    hall_sample_t pending[GEAR_POSITION_COUNT]; /* points valides de la session en cours */
    hall_sample_t ref_neutral;  /* neutre de la calibration courante a l'entree */
    uint8_t rejected_count;     /* points refuses depuis l'entree (cf CALIBRATION_MIN_GAP_L1) */

    int collar_prev;
    bool collar_rise_valid;
    uint32_t collar_rise_t;

    /* Accumulation en float pour une moyenne exacte sur un maintien long
     * (jusqu'a plusieurs secondes a 100ms/echantillon = quelques dizaines
     * d'echantillons, pas de risque de depassement d'un buffer fixe ici :
     * on accumule une somme courante au lieu d'un tableau). */
    double accum_sum[HALL_SENSOR_COUNT];
    int accum_count;
    bool triggered_for_this_hold;
} calibration_mode_t;

void calibration_mode_init(calibration_mode_t *cal, calibration_commit_cb_t on_commit, void *on_commit_ctx);

gear_position_t calibration_mode_current_target(const calibration_mode_t *cal);

/* ref_neutral : neutre de la calibration courante, pour refuser un point
 * pris au repos (cf CALIBRATION_MIN_NEUTRAL_L1). */
void calibration_mode_enter(calibration_mode_t *cal, const hall_sample_t *ref_neutral);

/* true si le point moyen `mean` serait refuse a l'etape courante (pur,
 * expose pour les tests). */
bool calibration_mode_rejects(const calibration_mode_t *cal, const hall_sample_t *mean);

/* A appeler a la place de la classification normale tant que cal->active
 * est vrai. */
void calibration_mode_step(calibration_mode_t *cal, uint32_t t_ms, const hall_sample_t *hall, int collar);

/* ---------------- SwitchGestureDetector ---------------- */

typedef enum {
    GESTURE_TARGET_NONE = 0,
    GESTURE_TARGET_H,
    GESTURE_TARGET_SEQ,
} gesture_target_t;

#define GESTURE_BLIP_MAX 8

typedef struct {
    uint32_t start;
    uint32_t end;
} blip_t;

typedef struct {
    /* gear6_active : geste en cours (dans la fenetre GESTURE_TIMEOUT_MS
     * depuis le premier maintien confirme de gear6), englobe les blips deja
     * comptes. gear6_currently_held : levier effectivement sur gear6 a ce
     * pas precis — seul un nouveau blip collerette requiert cette
     * condition ; un flottement transitoire hors gear6 pendant le geste
     * (observe en test terrain) ne fait plus perdre gear6_active ni les
     * blips deja valides, seul l'expiration du timeout global le fait. */
    bool gear6_active;
    bool gear6_currently_held;
    uint32_t gear6_t_start;
    blip_t gear6_blips[GESTURE_BLIP_MAX];
    int gear6_blip_count;

    bool seqdown_window_active;
    uint32_t seqdown_window_start_t;
    int seqdown_count;
    blip_t seqdown_blips[GESTURE_BLIP_MAX];
    int seqdown_blip_count;

    int collar_prev;
    bool collar_rise_valid;
    uint32_t collar_rise_t;

    /* Dernier geste detecte a ce pas (GESTURE_TARGET_NONE si aucun). */
    gesture_target_t last_event;
    uint32_t last_event_t;
} switch_gesture_detector_t;

void switch_gesture_detector_init(switch_gesture_detector_t *det);

void switch_gesture_detector_step(switch_gesture_detector_t *det, uint32_t t_ms, int collar, bool tracker_is_idle,
                                   gear_position_t confirmed_pos, movement_kind_t confirmed_kind);

/* ---------------- ModeStateMachine ---------------- */

typedef struct {
    gear_calibration_t calibration;

    movement_tracker_t tracker;
    switch_gesture_detector_t gesture;
    calibration_entry_detector_t calib_entry;
    calibration_mode_t calib_mode;

    gear_mode_t mode_courant;

    /* Dernier mouvement transmis en HID a ce pas (pos=GEAR_NONE si rien). */
    movement_event_t last_emitted;
    /* true si une bascule de mode vient d'etre appliquee a ce pas. */
    bool last_switch_applied;
    gesture_target_t last_switch_target;
} mode_state_machine_t;

void mode_state_machine_init(mode_state_machine_t *msm, const gear_calibration_t *calibration);

/* A appeler a chaque nouvel echantillon Hall/collerette. Met a jour
 * msm->last_emitted / msm->last_switch_applied pour ce pas (l'appelant
 * doit les lire immediatement apres l'appel, ils sont ecrases au pas
 * suivant). */
void mode_state_machine_step(mode_state_machine_t *msm, uint32_t t_ms, const hall_sample_t *hall, int collar);

#endif /* MODE_STATE_MACHINE_H */
