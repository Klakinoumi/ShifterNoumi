#include "status_led.h"

#include "main.h"

#define STATUS_LED_FLASH_MS        80u  /* flash rapide de validation d'un point */
#define STATUS_LED_FLASH_COUNT     3u
#define STATUS_LED_SWITCH_MS       100u /* double flash de bascule de mode */
#define STATUS_LED_SWITCH_COUNT    2u
#define STATUS_LED_REJECT_MS       50u  /* scintillement de refus d'un point */
#define STATUS_LED_REJECT_COUNT    10u

/* Motif "blip-blip" (maintien deja assez long pour etre valide). */
#define STATUS_LED_HOLD_BLIP_MS  100u
#define STATUS_LED_HOLD_GAP_MS   150u
#define STATUS_LED_HOLD_REST_MS  500u

/* PWM logiciel a 1kHz : en dessous de ce niveau, les impulsions
 * deviendraient plus lentes que ~60Hz et la LED scintillerait ; on eteint
 * plutot. */
#define STATUS_LED_MIN_LEVEL 16u

static void set_led(bool on)
{
    /* LED integree Blue Pill : active a l'etat bas (allumee = pin a 0). */
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, on ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

static void start_pattern(status_led_state_t *state, uint32_t t_ms, status_led_pattern_t pattern)
{
    state->pattern = pattern;
    state->pattern_t0 = t_ms;
}

/* n flashs de on_ms allume / on_ms eteint, puis eteint. */
static uint8_t flashes(uint32_t elapsed, uint32_t on_ms, uint32_t count)
{
    if (elapsed >= 2u * on_ms * count) {
        return 0;
    }
    return ((elapsed / on_ms) % 2u) == 0u ? 255u : 0u;
}

static uint32_t flashes_duration(uint32_t on_ms, uint32_t count)
{
    return 2u * on_ms * count;
}

/* Nombre d'impulsions du point attendu (0 = neutre, impulsion longue). */
static uint32_t pulse_count(gear_position_t target)
{
    switch (target) {
    case GEAR_1: return 1;
    case GEAR_2: return 2;
    case GEAR_3: return 3;
    case GEAR_4: return 4;
    case GEAR_5: return 5;
    case GEAR_6: return 6;
    case GEAR_SEQ_UP: return 7;
    case GEAR_SEQ_DOWN: return 8;
    default: return 0;
    }
}

static uint8_t waiting_level(gear_position_t target, uint32_t elapsed)
{
    uint32_t n = pulse_count(target);
    uint32_t pulses_ms = (n == 0u) ? STATUS_LED_NEUTRAL_ON_MS : n * (STATUS_LED_PULSE_ON_MS + STATUS_LED_PULSE_OFF_MS);
    uint32_t cycle = STATUS_LED_BREATH_MS + STATUS_LED_GAP_MS + pulses_ms + STATUS_LED_REST_MS;
    uint32_t t = elapsed % cycle;

    if (t < STATUS_LED_BREATH_MS) {
        /* Triangle 0..1000..0, au carre pour un fondu qui parait regulier
         * a l'oeil (la perception de luminosite n'est pas lineaire). */
        uint32_t half = STATUS_LED_BREATH_MS / 2u;
        uint32_t x = (t < half) ? t : (STATUS_LED_BREATH_MS - t);
        x = x * 1000u / half;
        return (uint8_t)(x * x * 255u / 1000000u);
    }
    t -= STATUS_LED_BREATH_MS;
    if (t < STATUS_LED_GAP_MS) {
        return 0;
    }
    t -= STATUS_LED_GAP_MS;
    if (t < pulses_ms) {
        if (n == 0u) {
            return 255;
        }
        return (t % (STATUS_LED_PULSE_ON_MS + STATUS_LED_PULSE_OFF_MS)) < STATUS_LED_PULSE_ON_MS ? 255u : 0u;
    }
    return 0;
}

void status_led_init(status_led_state_t *state)
{
    state->last_step_index = -1;
    state->last_rejected_count = 0;
    state->was_triggered_for_hold = false;
    state->pattern = STATUS_LED_PATTERN_NONE;
    state->pattern_t0 = 0;
    state->target = GEAR_NONE;
    state->pwm_acc = 0;
    state->led_on = false;
    set_led(false);
}

void status_led_step(status_led_state_t *state, uint32_t t_ms, const mode_state_machine_t *msm)
{
    uint32_t elapsed = t_ms - state->pattern_t0;

    /* Bascule de mode appliquee : jamais pendant la calibration (cf
     * mode_state_machine_step), va au bout de son double flash. */
    if (msm->last_switch_applied) {
        start_pattern(state, t_ms, STATUS_LED_PATTERN_MODE_SWITCHED);
        elapsed = 0;
    }

    if (!msm->calib_mode.active) {
        if (state->last_step_index != -1) {
            /* Sortie de calibration = dernier point valide : flash de
             * validation, puis LED eteinte. */
            start_pattern(state, t_ms, STATUS_LED_PATTERN_VALIDATED);
            elapsed = 0;
        }
        bool finishing =
            (state->pattern == STATUS_LED_PATTERN_MODE_SWITCHED &&
             elapsed < flashes_duration(STATUS_LED_SWITCH_MS, STATUS_LED_SWITCH_COUNT)) ||
            (state->pattern == STATUS_LED_PATTERN_VALIDATED &&
             elapsed < flashes_duration(STATUS_LED_FLASH_MS, STATUS_LED_FLASH_COUNT));
        if (!finishing) {
            state->pattern = STATUS_LED_PATTERN_NONE;
        }
        state->last_step_index = -1;
        state->was_triggered_for_hold = false;
        return;
    }

    bool triggered_now = msm->calib_mode.triggered_for_this_hold;
    gear_position_t target = calibration_mode_current_target(&msm->calib_mode);
    bool entering = state->last_step_index == -1;

    if (!entering && msm->calib_mode.step_index != state->last_step_index) {
        /* Point valide (vient forcement d'un relachement collerette). */
        start_pattern(state, t_ms, STATUS_LED_PATTERN_VALIDATED);
    } else if (!entering && msm->calib_mode.rejected_count != state->last_rejected_count) {
        /* Point refuse : meme etape, on le signale avant de remontrer le
         * point attendu. */
        start_pattern(state, t_ms, STATUS_LED_PATTERN_REJECTED);
    } else if (state->pattern == STATUS_LED_PATTERN_VALIDATED &&
               elapsed < flashes_duration(STATUS_LED_FLASH_MS, STATUS_LED_FLASH_COUNT)) {
        /* laisse finir le flash de validation */
    } else if (state->pattern == STATUS_LED_PATTERN_REJECTED &&
               elapsed < flashes_duration(STATUS_LED_REJECT_MS, STATUS_LED_REJECT_COUNT)) {
        /* laisse finir le scintillement de refus */
    } else if (triggered_now && !state->was_triggered_for_hold) {
        start_pattern(state, t_ms, STATUS_LED_PATTERN_HOLD_OK);
    } else if (state->pattern != STATUS_LED_PATTERN_WAITING &&
               !(triggered_now && state->pattern == STATUS_LED_PATTERN_HOLD_OK)) {
        /* Entree en calibration, fin du flash de validation, ou maintien
         * interrompu : on (re)montre le point attendu depuis le debut. */
        start_pattern(state, t_ms, STATUS_LED_PATTERN_WAITING);
    }
    state->target = target;
    state->last_step_index = msm->calib_mode.step_index;
    state->last_rejected_count = msm->calib_mode.rejected_count;
    state->was_triggered_for_hold = triggered_now;
}

uint8_t status_led_level(const status_led_state_t *state, uint32_t t_ms)
{
    uint32_t elapsed = t_ms - state->pattern_t0;
    switch (state->pattern) {
    case STATUS_LED_PATTERN_MODE_SWITCHED:
        return flashes(elapsed, STATUS_LED_SWITCH_MS, STATUS_LED_SWITCH_COUNT);
    case STATUS_LED_PATTERN_VALIDATED:
        return flashes(elapsed, STATUS_LED_FLASH_MS, STATUS_LED_FLASH_COUNT);
    case STATUS_LED_PATTERN_REJECTED:
        return flashes(elapsed, STATUS_LED_REJECT_MS, STATUS_LED_REJECT_COUNT);
    case STATUS_LED_PATTERN_HOLD_OK: {
        uint32_t t = elapsed % (2u * STATUS_LED_HOLD_BLIP_MS + STATUS_LED_HOLD_GAP_MS + STATUS_LED_HOLD_REST_MS);
        bool on = t < STATUS_LED_HOLD_BLIP_MS ||
                  (t >= STATUS_LED_HOLD_BLIP_MS + STATUS_LED_HOLD_GAP_MS &&
                   t < 2u * STATUS_LED_HOLD_BLIP_MS + STATUS_LED_HOLD_GAP_MS);
        return on ? 255u : 0u;
    }
    case STATUS_LED_PATTERN_WAITING:
        return waiting_level(state->target, elapsed);
    case STATUS_LED_PATTERN_NONE:
    default:
        return 0;
    }
}

void status_led_output(status_led_state_t *state, uint32_t t_ms)
{
    uint8_t level = status_led_level(state, t_ms);
    bool on;
    if (level < STATUS_LED_MIN_LEVEL) {
        on = false;
        state->pwm_acc = 0;
    } else {
        /* Sigma-delta : allume sur une fraction level/255 des tours. */
        state->pwm_acc = (uint16_t)(state->pwm_acc + level);
        on = state->pwm_acc >= 255u;
        if (on) {
            state->pwm_acc = (uint16_t)(state->pwm_acc - 255u);
        }
    }
    if (on != state->led_on) {
        state->led_on = on;
        set_led(on);
    }
}
