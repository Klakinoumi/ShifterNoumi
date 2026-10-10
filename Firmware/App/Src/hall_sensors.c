#include "hall_sensors.h"

#include "adc.h"
#include "main.h"

static volatile uint16_t hall_adc_buf[HALL_SENSOR_COUNT];

void hall_sensors_init(void)
{
    if (HAL_ADC_Start_DMA(&hadc1, (uint32_t *)hall_adc_buf, HALL_SENSOR_COUNT) != HAL_OK) {
        Error_Handler();
    }
}

void hall_sensors_read(hall_sample_t *out)
{
    for (int i = 0; i < HALL_SENSOR_COUNT; i++) {
        out->v[i] = hall_adc_buf[i];
    }
}
