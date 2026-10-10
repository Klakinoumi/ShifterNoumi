/**
 * test_collar_switch.c — teste collar_switch_debounce_step (algorithme pur,
 * pas de dependance HAL) : rejet d'un rebond isole, confirmation apres
 * COLLAR_DEBOUNCE_STEPS lectures stables ; puis collar_switch_read() via
 * le stub GPIO (hal_stub/hal_gpio_stub.c).
 *
 * Build : gcc -std=c99 -Wall -Wextra -I ../Inc -I hal_stub \
 *   test_collar_switch.c ../Src/collar_switch.c hal_stub/hal_gpio_stub.c \
 *   -o test_collar_switch
 */
#include <stdio.h>

#include "collar_switch.h"
#include "main.h" /* hal_stub/main.h : hal_stub_gpio_level */

static int failures = 0;

#define CHECK(cond, msg)                                                                                             \
    do {                                                                                                             \
        if (!(cond)) {                                                                                               \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                                                   \
            failures++;                                                                                              \
        }                                                                                                            \
    } while (0)

int main(void)
{
    collar_switch_t sw;
    collar_switch_init(&sw);

    CHECK(collar_switch_debounce_step(&sw, 0) == 0, "etat initial stable a 0");

    /* Rebond isole (un seul pas a 1 puis retour a 0) ne doit pas etre
     * confirme si COLLAR_DEBOUNCE_STEPS > 1. */
    CHECK(collar_switch_debounce_step(&sw, 1) == 0, "1 pas a 1 : pas encore confirme");
    CHECK(collar_switch_debounce_step(&sw, 0) == 0, "retour a 0 avant confirmation : rebond rejete");

    /* Front reel 0->1 confirme apres COLLAR_DEBOUNCE_STEPS pas consecutifs. */
    for (int i = 0; i < COLLAR_DEBOUNCE_STEPS - 1; i++) {
        CHECK(collar_switch_debounce_step(&sw, 1) == 0, "front montant pas encore confirme");
    }
    CHECK(collar_switch_debounce_step(&sw, 1) == 1, "front montant confirme apres COLLAR_DEBOUNCE_STEPS pas");

    /* Front descendant confirme de la meme facon. */
    for (int i = 0; i < COLLAR_DEBOUNCE_STEPS - 1; i++) {
        CHECK(collar_switch_debounce_step(&sw, 0) == 1, "front descendant pas encore confirme");
    }
    CHECK(collar_switch_debounce_step(&sw, 0) == 0, "front descendant confirme apres COLLAR_DEBOUNCE_STEPS pas");

    /* Chemin GPIO complet : PB1 a l'etat haut = collerette levee (1). */
    collar_switch_init(&sw);
    hal_stub_gpio_level = GPIO_PIN_SET;
    CHECK(collar_switch_read_raw() == 1, "GPIO_PIN_SET lu comme 1");
    for (int i = 0; i < COLLAR_DEBOUNCE_STEPS - 1; i++) {
        CHECK(collar_switch_read(&sw) == 0, "read : front montant pas encore confirme");
    }
    CHECK(collar_switch_read(&sw) == 1, "read : front montant confirme");
    hal_stub_gpio_level = GPIO_PIN_RESET;
    CHECK(collar_switch_read_raw() == 0, "GPIO_PIN_RESET lu comme 0");

    if (failures == 0) {
        printf("OK: tous les tests collar_switch ont reussi.\n");
    } else {
        printf("%d test(s) en echec.\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
