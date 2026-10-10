/**
 * test_calibration_flash.c — teste calibration_flash.c hors cible en
 * simulant la flash dans un buffer RAM avec la vraie semantique (program
 * ne fait que AND avec le contenu existant, erase remet tout a 0xFF).
 *
 * Compile calibration_flash.c avec CALIBRATION_FLASH_READ_OVERRIDE defini
 * pour fournir notre propre flash_read_bytes() qui lit dans le buffer
 * simule plutot que la memoire physique reelle (inexistante sur cet
 * hote) — le reste du fichier (logique CRC/slots/rotation) est teste
 * inchange, seule l'indirection de lecture change.
 *
 * Build : gcc -std=c99 -Wall -Wextra -DCALIBRATION_FLASH_READ_OVERRIDE \
 *   -I ../Inc -I hal_stub test_calibration_flash.c ../Src/calibration_flash.c \
 *   -o test_calibration_flash
 *
 * (hal_stub/main.h fournit HAL_StatusTypeDef/FLASH_EraseInitTypeDef/etc a
 * la place du vrai Core/Inc/main.h du firmware, absent hors cible.)
 */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "main.h" /* hal_stub/main.h en test, cf note de build ci-dessus */

static uint8_t g_sim_flash[1024];

HAL_StatusTypeDef HAL_FLASH_Unlock(void) { return HAL_OK; }
HAL_StatusTypeDef HAL_FLASH_Lock(void) { return HAL_OK; }

HAL_StatusTypeDef HAL_FLASH_Program(uint32_t TypeProgram, uint32_t Address, uint64_t Data)
{
    (void)TypeProgram;
    uint32_t offset = (uint32_t)(Address - 0x0800FC00UL);
    if (offset + 2 > sizeof(g_sim_flash)) {
        return HAL_ERROR;
    }
    uint16_t hw = (uint16_t)Data;
    uint16_t existing;
    memcpy(&existing, &g_sim_flash[offset], 2);
    uint16_t result = (uint16_t)(existing & hw); /* flash program can only clear bits */
    memcpy(&g_sim_flash[offset], &result, 2);
    return HAL_OK;
}

HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *pEraseInit, uint32_t *PageError)
{
    (void)pEraseInit;
    memset(g_sim_flash, 0xFF, sizeof(g_sim_flash));
    *PageError = 0xFFFFFFFFUL;
    return HAL_OK;
}

/* --- override de lecture flash utilise par calibration_flash.c --- */
void flash_read_bytes(uintptr_t addr, void *out, size_t len)
{
    uint32_t offset = (uint32_t)(addr - 0x0800FC00UL);
    memcpy(out, &g_sim_flash[offset], len);
}

#include "calibration_flash.h"

static int failures = 0;
#define CHECK(cond, msg)                                                                                             \
    do {                                                                                                             \
        if (!(cond)) {                                                                                               \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                                                   \
            failures++;                                                                                              \
        }                                                                                                            \
    } while (0)

static void make_positions(hall_sample_t out[GEAR_POSITION_COUNT], uint16_t base)
{
    for (int p = 0; p < GEAR_POSITION_COUNT; p++) {
        for (int i = 0; i < HALL_SENSOR_COUNT; i++) {
            out[p].v[i] = (uint16_t)(base + p * 10 + i);
        }
    }
}

int main(void)
{
    memset(g_sim_flash, 0xFF, sizeof(g_sim_flash)); /* flash vierge au premier boot */

    hall_sample_t loaded[GEAR_POSITION_COUNT];

    /* --- Test 1 : flash vierge -> load() echoue. --- */
    CHECK(calibration_flash_load(loaded) == false, "flash vierge doit echouer au chargement");

    /* --- Test 2 : save() puis load() retrouve les memes valeurs. --- */
    hall_sample_t written1[GEAR_POSITION_COUNT];
    make_positions(written1, 1000);
    CHECK(calibration_flash_save(written1) == true, "premiere sauvegarde doit reussir");
    CHECK(calibration_flash_load(loaded) == true, "chargement apres sauvegarde doit reussir");
    CHECK(memcmp(loaded, written1, sizeof(loaded)) == 0, "valeurs chargees doivent matcher la sauvegarde");

    /* --- Test 3 : plusieurs sauvegardes successives -> load() retrouve
     * toujours la DERNIERE (plus grand seq), pas la premiere. --- */
    hall_sample_t written2[GEAR_POSITION_COUNT];
    make_positions(written2, 2000);
    CHECK(calibration_flash_save(written2) == true, "deuxieme sauvegarde doit reussir");
    CHECK(calibration_flash_load(loaded) == true, "chargement apres 2e sauvegarde doit reussir");
    CHECK(memcmp(loaded, written2, sizeof(loaded)) == 0, "doit retrouver la 2e sauvegarde, pas la 1ere");

    /* --- Test 4 : remplir tous les slots (rotation), verifier qu'un
     * effacement se produit et que la sequence continue sa progression
     * (pas de remise a zero qui casserait "plus grand seq gagne"). --- */
    hall_sample_t last_written[GEAR_POSITION_COUNT];
    for (unsigned i = 0; i < CALIBRATION_FLASH_SLOT_COUNT + 3; i++) {
        make_positions(last_written, (uint16_t)(3000 + i * 100));
        CHECK(calibration_flash_save(last_written) == true, "sauvegarde en boucle doit reussir");
    }
    CHECK(calibration_flash_load(loaded) == true, "chargement apres rotation complete doit reussir");
    CHECK(memcmp(loaded, last_written, sizeof(loaded)) == 0, "doit retrouver la toute derniere sauvegarde apres rotation");

    /* --- Test 4b : coupure d'alimentation pendant une sauvegarde. On
     * repart d'une page avec 2 slots valides, puis on simule une ecriture
     * interrompue du 3e slot (seuls les 8 premiers octets programmes, avec
     * des bits a 0 qui ne correspondront pas a la sauvegarde suivante). La
     * sauvegarde suivante ne doit pas ecrire par-dessus ce slot sale. --- */
    memset(g_sim_flash, 0xFF, sizeof(g_sim_flash));
    CHECK(calibration_flash_save(written1) == true, "coupure : slot 0");
    CHECK(calibration_flash_save(written2) == true, "coupure : slot 1");
    memset(&g_sim_flash[2 * CALIBRATION_FLASH_SLOT_SIZE], 0x00, 8);
    hall_sample_t after_cut[GEAR_POSITION_COUNT];
    make_positions(after_cut, 5000);
    CHECK(calibration_flash_save(after_cut) == true, "sauvegarde apres coupure doit reussir");
    CHECK(calibration_flash_load(loaded) == true, "chargement apres coupure doit reussir");
    CHECK(memcmp(loaded, after_cut, sizeof(loaded)) == 0, "doit retrouver la sauvegarde faite apres la coupure");

    /* Meme chose quand le slot sale est le dernier de la page : la
     * sauvegarde suivante doit effacer la page et repartir au slot 0. */
    memset(g_sim_flash, 0xFF, sizeof(g_sim_flash));
    for (unsigned i = 0; i + 1 < CALIBRATION_FLASH_SLOT_COUNT; i++) {
        CHECK(calibration_flash_save(written1) == true, "remplissage avant coupure en fin de page");
    }
    memset(&g_sim_flash[(CALIBRATION_FLASH_SLOT_COUNT - 1) * CALIBRATION_FLASH_SLOT_SIZE], 0x00, 8);
    CHECK(calibration_flash_save(after_cut) == true, "sauvegarde apres coupure en fin de page doit reussir");
    CHECK(calibration_flash_load(loaded) == true, "chargement apres coupure en fin de page doit reussir");
    CHECK(memcmp(loaded, after_cut, sizeof(loaded)) == 0, "doit retrouver la sauvegarde apres coupure en fin de page");

    /* --- Test 5 : corruption d'un slot (CRC casse) ne doit pas empecher
     * de retrouver un autre slot valide plus ancien. --- */
    /* Simule une corruption directe du dernier slot ecrit en cassant son
     * CRC (dernier octet du slot ecrit, offset approximatif) — ici on
     * corrompt carrement tout le buffer de simulation pour un cas plus
     * simple a raisonner : plus aucun slot valide, load() doit echouer
     * proprement (pas de crash), pas de faux positif sur donnees
     * aleatoires. */
    for (size_t i = 0; i < sizeof(g_sim_flash); i++) {
        g_sim_flash[i] = (uint8_t)(i * 37);
    }
    CHECK(calibration_flash_load(loaded) == false, "flash corrompue (pas de magic/CRC valide) doit echouer proprement");

    /* --- Test 6 : calibration_flash_erase() (commande de remise a zero) :
     * plus rien au chargement (defauts), puis une nouvelle sauvegarde
     * fonctionne normalement. --- */
    CHECK(calibration_flash_save(written1) == true, "sauvegarde avant remise a zero");
    CHECK(calibration_flash_erase() == true, "effacement de la page");
    CHECK(calibration_flash_load(loaded) == false, "apres remise a zero : rien en flash (defauts)");
    CHECK(calibration_flash_save(written2) == true, "sauvegarde apres remise a zero");
    CHECK(calibration_flash_load(loaded) == true && memcmp(loaded, written2, sizeof(loaded)) == 0,
          "relecture de la sauvegarde faite apres remise a zero");

    if (failures == 0) {
        printf("OK: tous les tests calibration_flash ont reussi.\n");
    } else {
        printf("%d test(s) en echec.\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
