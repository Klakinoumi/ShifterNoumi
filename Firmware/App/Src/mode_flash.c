#include "mode_flash.h"

#include <stdint.h>
#include <string.h>

#include "main.h"

/* Lecture flash substituable par un banc de test, comme calibration_flash.c
 * (meme symbole flash_read_bytes, fourni par le test). */
#ifdef CALIBRATION_FLASH_READ_OVERRIDE
void flash_read_bytes(uintptr_t addr, void *out, size_t len);
#else
static void flash_read_bytes(uintptr_t addr, void *out, size_t len)
{
    memcpy(out, (const void *)addr, len);
}
#endif

static uint16_t read_record(unsigned index)
{
    uint16_t v;
    flash_read_bytes((uintptr_t)MODE_FLASH_PAGE_ADDR + 2U * index, &v, sizeof(v));
    return v;
}

/* Parcourt la page : *last_valid = dernier enregistrement H/SEQ (0xFFFF si
 * aucun), retour = index du premier emplacement libre apres le dernier
 * emplacement non vierge (MODE_FLASH_RECORD_COUNT si la page est pleine). */
static unsigned scan(uint16_t *last_valid)
{
    unsigned next_free = 0;
    *last_valid = 0xFFFFU;
    for (unsigned i = 0; i < MODE_FLASH_RECORD_COUNT; i++) {
        uint16_t v = read_record(i);
        if (v == 0xFFFFU) {
            continue;
        }
        next_free = i + 1U;
        if (v == MODE_FLASH_RECORD_H || v == MODE_FLASH_RECORD_SEQ) {
            *last_valid = v;
        }
    }
    return next_free;
}

gear_mode_t mode_flash_load(void)
{
    uint16_t last;
    scan(&last);
    if (last == MODE_FLASH_RECORD_H) {
        return GEAR_MODE_H;
    }
    if (last == MODE_FLASH_RECORD_SEQ) {
        return GEAR_MODE_SEQ;
    }
    return GEAR_MODE_NONE;
}

bool mode_flash_save(gear_mode_t mode)
{
    uint16_t record;
    if (mode == GEAR_MODE_H) {
        record = MODE_FLASH_RECORD_H;
    } else if (mode == GEAR_MODE_SEQ) {
        record = MODE_FLASH_RECORD_SEQ;
    } else {
        return false;
    }

    uint16_t last;
    unsigned next = scan(&last);
    if (last == record) {
        return true;
    }

    if (HAL_FLASH_Unlock() != HAL_OK) {
        return false;
    }
    bool ok = true;
    if (next >= MODE_FLASH_RECORD_COUNT) {
        FLASH_EraseInitTypeDef erase = {0};
        erase.TypeErase = FLASH_TYPEERASE_PAGES;
        erase.PageAddress = MODE_FLASH_PAGE_ADDR;
        erase.NbPages = 1;
        uint32_t page_error = 0;
        ok = HAL_FLASHEx_Erase(&erase, &page_error) == HAL_OK && page_error == 0xFFFFFFFFUL;
        next = 0;
    }
    if (ok) {
        ok = HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, MODE_FLASH_PAGE_ADDR + 2U * next, record) == HAL_OK;
    }
    HAL_FLASH_Lock();
    return ok;
}
