#include "collar_switch.h"

#include "main.h"

void collar_switch_init(collar_switch_t *sw)
{
    sw->stable_state = 0;
    sw->candidate_state = 0;
    sw->candidate_count = 0;
}

int collar_switch_read_raw(void)
{
    return HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_1) == GPIO_PIN_SET ? 1 : 0;
}

int collar_switch_debounce_step(collar_switch_t *sw, int raw_state)
{
    if (raw_state == sw->stable_state) {
        sw->candidate_state = raw_state;
        sw->candidate_count = 0;
        return sw->stable_state;
    }

    if (raw_state == sw->candidate_state) {
        sw->candidate_count++;
    } else {
        sw->candidate_state = raw_state;
        sw->candidate_count = 1;
    }

    if (sw->candidate_count >= COLLAR_DEBOUNCE_STEPS) {
        sw->stable_state = raw_state;
        sw->candidate_count = 0;
    }

    return sw->stable_state;
}

int collar_switch_read(collar_switch_t *sw)
{
    return collar_switch_debounce_step(sw, collar_switch_read_raw());
}
