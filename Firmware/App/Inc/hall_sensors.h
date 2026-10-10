/**
 * hall_sensors.h — lecture des 6 capteurs Hall (U2..U7) via ADC1+DMA.
 *
 * Seul module autorise a connaitre l'ADC (cf decouplage voulu en Phase 5,
 * PLAN.md). Reprend le pattern deja valide dans Diag/Gx100_DiagFirm/Core/Src/
 * main.c : ADC1 demarre une fois en DMA circulaire (HAL_ADC_Start_DMA),
 * le buffer DMA est rempli en continu par le HAL/DMA en tache de fond,
 * hall_sensors_read() ne fait qu'une copie (snapshot) sans jamais arreter
 * ou relancer la conversion.
 *
 * Brochage (PLAN.md Phase 2) : ADC1_IN0..IN5 = PA0..PA5 = U2..U7, dans cet
 * ordre — correspond directement a l'ordre attendu par hall_sample_t.v[0..5]
 * (gear_position.h).
 */
#ifndef HALL_SENSORS_H
#define HALL_SENSORS_H

#include "gear_position.h"

/* A appeler une fois au demarrage, apres MX_ADC1_Init()/MX_DMA_Init().
 * Demarre l'acquisition continue en DMA circulaire ; ne retourne jamais
 * d'erreur recuperable (echec = Error_Handler(), comme le reste du HAL). */
void hall_sensors_init(void);

/* Copie l'etat courant des 6 canaux ADC (rempli en continu par le DMA en
 * tache de fond) dans *out. Ne bloque jamais, ne redemarre rien. */
void hall_sensors_read(hall_sample_t *out);

#endif /* HALL_SENSORS_H */
