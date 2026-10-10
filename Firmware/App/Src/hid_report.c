#include "hid_report.h"

#include <string.h>

static void set_bit(uint8_t *report, int bit_index)
{
    report[bit_index / 8] |= (uint8_t)(1u << (bit_index % 8));
}

static gear_position_t gear_from_button_slot(int slot)
{
    /* slot 0..5 -> GEAR_1..GEAR_6, cf HID_BUTTON_GEAR1..6. */
    switch (slot) {
    case HID_BUTTON_GEAR1:
        return GEAR_1;
    case HID_BUTTON_GEAR2:
        return GEAR_2;
    case HID_BUTTON_GEAR3:
        return GEAR_3;
    case HID_BUTTON_GEAR4:
        return GEAR_4;
    case HID_BUTTON_GEAR5:
        return GEAR_5;
    case HID_BUTTON_GEAR6:
        return GEAR_6;
    default:
        return GEAR_NONE;
    }
}

static int button_slot_from_gear(gear_position_t pos)
{
    switch (pos) {
    case GEAR_1:
        return HID_BUTTON_GEAR1;
    case GEAR_2:
        return HID_BUTTON_GEAR2;
    case GEAR_3:
        return HID_BUTTON_GEAR3;
    case GEAR_4:
        return HID_BUTTON_GEAR4;
    case GEAR_5:
        return HID_BUTTON_GEAR5;
    case GEAR_6:
        return HID_BUTTON_GEAR6;
    default:
        return -1;
    }
}

void hid_report_init(hid_report_state_t *state)
{
    memset(state, 0, sizeof(*state));
    state->gear_active = -1;
    state->shift_up_pulse = 0;
    state->shift_down_pulse = 0;
    state->reverse_latched = 0;
    state->fast_prev = GEAR_NONE;
    state->fast_reverse_latched = 0;
}

static void clear_bit(uint8_t *report, int bit_index)
{
    report[bit_index / 8] &= (uint8_t)~(1u << (bit_index % 8));
}

/* --- Axes vendor : 6 valeurs Hall brutes en uint16 LE, octets 2-13 ---
 * cf hid_report.h pour le pourquoi (reproduit le comportement du
 * firmware d'origine, dont chaque trame HID porte des axes vivants). */
static void write_axes(uint8_t *report, const hall_sample_t *hall)
{
    for (int i = 0; i < HALL_SENSOR_COUNT; i++) {
        int idx = HID_AXES_BYTE_OFFSET + i * HID_AXES_STRIDE;
        report[idx] = (uint8_t)(hall->v[i] & 0xFFu);
        report[idx + 1] = (uint8_t)((hall->v[i] >> 8) & 0xFFu);
    }
}

/* Avance une impulsion rapide a l'instant t_ms, retourne true si le bouton
 * doit etre actif. trigger = nouvel engagement a ce pas. */
static bool fast_pulse_step(fast_pulse_t *p, bool trigger, uint32_t t_ms)
{
    if (trigger) {
        p->pending++;
    }
    /* (int32_t) : comparaison robuste au rebouclage de HAL_GetTick. */
    if (p->active && (int32_t)(t_ms - p->end_t) >= 0) {
        p->active = false;
        p->end_t = t_ms + FAST_PULSE_GAP_MS;
    }
    if (!p->active && p->pending > 0 && (int32_t)(t_ms - p->end_t) >= 0) {
        p->pending--;
        p->active = true;
        p->end_t = t_ms + FAST_PULSE_MS;
    }
    return p->active;
}

void hid_report_apply_fast_gear(hid_report_state_t *state, const mode_state_machine_t *msm, int collar,
                                gear_position_t fast_gear, const hall_sample_t *hall, uint32_t t_ms)
{
    write_axes(state->report, hall);

    if (msm->calib_mode.active) {
        state->fast_prev = GEAR_NONE;
        return;
    }

    /* Geste d'entree en calibration (gear1->gear5->gear6->gear2, collerette
     * levee) : son dernier pas, gear2, ne doit jamais partir en jeu (meme
     * regle que mode_state_machine_step). Le detecteur lent n'a pas encore
     * vu gear2 quand le chemin rapide l'engage : on le masque des que le
     * detecteur attend precisement ce dernier pas. */
    if (fast_gear == GEAR_2 && collar == 1 && msm->calib_entry.index == 3) {
        fast_gear = GEAR_NONE;
    }

    bool new_engage = (fast_gear != GEAR_NONE && fast_gear != state->fast_prev);
    if (fast_gear == GEAR_1 && state->fast_prev != GEAR_1) {
        state->fast_reverse_latched = (collar == 1);
    }
    state->fast_prev = fast_gear;

    /* Le chemin rapide remplace entierement les boutons du chemin lent ;
     * fast_gear ne rend que des positions du mode courant (cf fast_gear.h). */
    for (int slot = HID_BUTTON_GEAR1; slot <= HID_BUTTON_GEAR6; slot++) {
        clear_bit(state->report, slot);
    }
    clear_bit(state->report, HID_BUTTON_REVERSE);
    clear_bit(state->report, HID_BUTTON_SHIFT_UP);
    clear_bit(state->report, HID_BUTTON_SHIFT_DOWN);

    int slot = button_slot_from_gear(fast_gear);
    if (slot >= 0) {
        set_bit(state->report, (fast_gear == GEAR_1 && state->fast_reverse_latched) ? HID_BUTTON_REVERSE : slot);
    }

    if (fast_pulse_step(&state->fast_up, new_engage && fast_gear == GEAR_SEQ_UP, t_ms)) {
        set_bit(state->report, HID_BUTTON_SHIFT_UP);
    }
    if (fast_pulse_step(&state->fast_down, new_engage && fast_gear == GEAR_SEQ_DOWN, t_ms)) {
        set_bit(state->report, HID_BUTTON_SHIFT_DOWN);
    }
}

void hid_report_build(hid_report_state_t *state, const mode_state_machine_t *msm, int collar,
                       const hall_sample_t *hall)
{
    memset(state->report, 0, sizeof(state->report));

    write_axes(state->report, hall);

    /* --- Position H-pattern tenue (gear1..gear6) : latch jusqu'au retour
     * au neutre (movement_tracker repasse en TRACKER_IDLE), pour reproduire
     * le comportement d'origine (bouton maintenu a 1 tout le temps que le
     * levier reste en position, cf Phase 3). HELD_CORRECTED remplace la
     * position active sans repasser par un etat "aucun bouton". */
    const movement_event_t *ev = &msm->last_emitted;
    int prev_gear_active = state->gear_active;
    if (ev->pos != GEAR_NONE && (ev->kind == MOVEMENT_KIND_HELD || ev->kind == MOVEMENT_KIND_HELD_CORRECTED)) {
        int slot = button_slot_from_gear(ev->pos);
        if (slot >= 0) {
            state->gear_active = slot;
        }
    } else if (movement_tracker_is_idle(&msm->tracker)) {
        state->gear_active = -1;
    }

    /* Latch marche arriere : ne capturer l'etat collerette qu'a l'instant
     * ou l'on VIENT de passer en 1ere (transition), pas a chaque pas —
     * lever/relacher la collerette pendant le maintien ne doit plus faire
     * osciller le bouton transmis (cf hid_report.h). */
    if (state->gear_active == HID_BUTTON_GEAR1 && prev_gear_active != HID_BUTTON_GEAR1) {
        state->reverse_latched = (collar == 1);
    }

    if (state->gear_active >= 0) {
        gear_position_t active_pos = gear_from_button_slot(state->gear_active);
        if (active_pos == GEAR_1 && state->reverse_latched) {
            /* Marche arriere : bouton 11 remplace le bouton 1 pour toute la
             * duree du maintien en 1ere, fige a l'etat de la collerette au
             * moment de la confirmation (cf mapping Phase 3, pas de cumul
             * observe sur la capture live). */
            set_bit(state->report, HID_BUTTON_REVERSE);
        } else {
            set_bit(state->report, state->gear_active);
        }
    }

    /* --- Mouvement sequentiel : impulsion breve shift+/shift-,
     * independante du maintien physique du levier (cf Phase 5, le levier
     * sequentiel peut lui aussi etre maintenu sans que ca change le nombre
     * de rapports envoyes au jeu).
     * GX100 (2026-09-13, bug trouve en test terrain) : declenche le pulse
     * quel que soit ev->kind (SEQ ou HELD/HELD_CORRECTED), pas seulement
     * SEQ. Un passage de rapport appuye/qui traine en longueur (>250ms,
     * HOLD_TIMEOUT_MS) etait classe HELD par le tracker — mais
     * button_slot_from_gear(GEAR_SEQ_UP/DOWN) renvoie -1 (ces positions
     * n'ont pas de bouton gear1-6 dedie), donc la branche gear_active
     * ci-dessus ne faisait rien : le passage de rapport disparaissait
     * completement cote jeu (confirme sur capture reelle test_Seq_cdc.csv,
     * 4 mouvements sur 17 classes HELD, aucun des 4 n'a produit de bouton
     * dans test_Seq_hid.csv). Le changement de rapport doit toujours
     * rester une impulsion transmise au jeu, peu importe combien de temps
     * l'utilisateur maintient physiquement le levier ensuite. */
    if (ev->pos == GEAR_SEQ_UP) {
        state->shift_up_pulse = HID_PULSE_STEPS;
    } else if (ev->pos == GEAR_SEQ_DOWN) {
        state->shift_down_pulse = HID_PULSE_STEPS;
    }

    if (state->shift_up_pulse > 0) {
        set_bit(state->report, HID_BUTTON_SHIFT_UP);
        state->shift_up_pulse--;
    }
    if (state->shift_down_pulse > 0) {
        set_bit(state->report, HID_BUTTON_SHIFT_DOWN);
        state->shift_down_pulse--;
    }

    /* --- Octet debug (byte 14, cf HID_DEBUG_BYTE_INDEX), sans effet sur le
     * jeu. --- */
    if (msm->mode_courant == GEAR_MODE_SEQ) {
        state->report[HID_DEBUG_BYTE_INDEX] |= (uint8_t)(1u << HID_DEBUG_BIT_MODE_SEQ);
    }
    if (msm->calib_mode.active) {
        state->report[HID_DEBUG_BYTE_INDEX] |= (uint8_t)(1u << HID_DEBUG_BIT_CALIBRATION);
    }
}
