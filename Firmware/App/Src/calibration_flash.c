#include "calibration_flash.h"

#include <string.h>

#include "main.h"

#define CALIBRATION_FLASH_MAGIC 0x47583130UL /* "GX10" */

/* Layout d'un slot (tient dans CALIBRATION_FLASH_SLOT_SIZE=128 octets) :
 *   uint32_t magic;                                  4
 *   uint32_t seq;                                     4
 *   hall_sample_t positions[GEAR_POSITION_COUNT];   108
 *   uint16_t crc16;                                   2
 *   -------------------------------------------------
 *   total                                           118, reste 10 octets
 *   de marge (jamais ecrits, la page est vierge=0xFFFF a cet endroit tant
 *   qu'aucun HAL_FLASH_Program n'y touche).
 * Tout le slot est ecrit par mots de 16 bits (HAL_FLASH_Program F1 ne
 * supporte que FLASH_TYPEPROGRAM_HALFWORD), donc chaque champ est copie
 * dans un buffer local aligne avant programmation. */
typedef struct {
    uint32_t magic;
    uint32_t seq;
    hall_sample_t positions[GEAR_POSITION_COUNT];
    uint16_t crc16;
} calibration_slot_t;

#define CALIBRATION_SLOT_PAYLOAD_SIZE (sizeof(uint32_t) + sizeof(uint32_t) + sizeof(hall_sample_t) * GEAR_POSITION_COUNT)

static uint16_t crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static uintptr_t slot_addr(unsigned slot_index)
{
    return (uintptr_t)CALIBRATION_FLASH_PAGE_ADDR + (uintptr_t)slot_index * CALIBRATION_FLASH_SLOT_SIZE;
}

/* Indirection de lecture flash : sur cible, la flash est memory-mapped,
 * donc une simple lecture a l'adresse suffit (pas besoin du HAL pour
 * lire, seule l'ecriture necessite HAL_FLASH_Program). Isolee dans sa
 * propre fonction pour rester substituable par un banc de test hors
 * cible (voir App/test/test_calibration_flash.c), qui redirige les
 * adresses de page vers un buffer RAM simulant la flash. */
#ifdef CALIBRATION_FLASH_READ_OVERRIDE
void flash_read_bytes(uintptr_t addr, void *out, size_t len); /* fournie par le banc de test */
#else
static void flash_read_bytes(uintptr_t addr, void *out, size_t len)
{
    memcpy(out, (const void *)addr, len);
}
#endif

static bool read_slot(unsigned slot_index, calibration_slot_t *out)
{
    uint8_t raw[CALIBRATION_FLASH_SLOT_SIZE];
    flash_read_bytes(slot_addr(slot_index), raw, sizeof(raw));
    const uint8_t *base = raw;
    uint32_t magic;
    memcpy(&magic, base, sizeof(magic));
    if (magic != CALIBRATION_FLASH_MAGIC) {
        return false;
    }

    uint32_t seq;
    memcpy(&seq, base + sizeof(uint32_t), sizeof(seq));

    hall_sample_t positions[GEAR_POSITION_COUNT];
    memcpy(positions, base + 2 * sizeof(uint32_t), sizeof(positions));

    uint16_t stored_crc;
    memcpy(&stored_crc, base + CALIBRATION_SLOT_PAYLOAD_SIZE, sizeof(stored_crc));

    uint16_t computed_crc = crc16_ccitt(base, CALIBRATION_SLOT_PAYLOAD_SIZE);
    if (stored_crc != computed_crc) {
        return false;
    }

    out->magic = magic;
    out->seq = seq;
    memcpy(out->positions, positions, sizeof(positions));
    out->crc16 = stored_crc;
    return true;
}

/* Trouve le slot valide au plus grand numero de sequence. Retourne -1 si
 * aucun slot valide. *out_seq n'est ecrit que si un slot est trouve. */
static int find_latest_slot(uint32_t *out_seq, calibration_slot_t *out_slot)
{
    int best_index = -1;
    uint32_t best_seq = 0;
    calibration_slot_t best_slot;

    for (unsigned i = 0; i < CALIBRATION_FLASH_SLOT_COUNT; i++) {
        calibration_slot_t slot;
        if (read_slot(i, &slot)) {
            if (best_index < 0 || slot.seq > best_seq) {
                best_index = (int)i;
                best_seq = slot.seq;
                best_slot = slot;
            }
        }
    }

    if (best_index >= 0) {
        *out_seq = best_seq;
        *out_slot = best_slot;
    }
    return best_index;
}

bool calibration_flash_load(hall_sample_t out_positions[GEAR_POSITION_COUNT])
{
    uint32_t seq;
    calibration_slot_t slot;
    int index = find_latest_slot(&seq, &slot);
    if (index < 0) {
        return false;
    }
    memcpy(out_positions, slot.positions, sizeof(slot.positions));
    return true;
}

static bool flash_write_halfwords(uintptr_t addr, const uint8_t *data, size_t len)
{
    /* len doit etre pair : chaque HAL_FLASH_Program halfword ecrit 2
     * octets. Le buffer appelant (slot local) est toujours dimensionne
     * en multiple de 2, jamais de reste ici. */
    for (size_t i = 0; i < len; i += 2) {
        uint16_t halfword;
        memcpy(&halfword, data + i, sizeof(halfword));
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr + i, halfword) != HAL_OK) {
            return false;
        }
    }
    return true;
}

static bool slot_is_blank(unsigned slot_index)
{
    uint8_t raw[CALIBRATION_FLASH_SLOT_SIZE];
    flash_read_bytes(slot_addr(slot_index), raw, sizeof(raw));
    for (size_t i = 0; i < sizeof(raw); i++) {
        if (raw[i] != 0xFF) {
            return false;
        }
    }
    return true;
}

static bool erase_calibration_page(void)
{
    FLASH_EraseInitTypeDef erase = {0};
    erase.TypeErase = FLASH_TYPEERASE_PAGES;
    erase.PageAddress = CALIBRATION_FLASH_PAGE_ADDR;
    erase.NbPages = 1;

    uint32_t page_error = 0;
    return HAL_FLASHEx_Erase(&erase, &page_error) == HAL_OK && page_error == 0xFFFFFFFFUL;
}

bool calibration_flash_erase(void)
{
    if (HAL_FLASH_Unlock() != HAL_OK) {
        return false;
    }
    bool erased = erase_calibration_page();
    HAL_FLASH_Lock();
    return erased;
}

bool calibration_flash_save(const hall_sample_t positions[GEAR_POSITION_COUNT])
{
    /* next_index part du slot suivant le slot valide au plus grand seq,
     * puis saute tout slot qui n'est pas entierement vierge (0xFF) : une
     * coupure d'alimentation pendant un HAL_FLASH_Program peut laisser un
     * slot ni vierge ni valide, et la flash ne sait que forcer des bits a
     * 0 — ecrire par-dessus fusionnerait (AND) le nouveau contenu avec les
     * restes, et cette sauvegarde serait perdue (CRC KO au load()). */
    uint32_t last_seq = 0;
    calibration_slot_t last_slot;
    int last_index = find_latest_slot(&last_seq, &last_slot);

    unsigned next_index = (last_index < 0) ? 0 : ((unsigned)last_index + 1);
    uint32_t next_seq = last_seq + 1;

    while (next_index < CALIBRATION_FLASH_SLOT_COUNT && !slot_is_blank(next_index)) {
        next_index++;
    }

    if (next_index >= CALIBRATION_FLASH_SLOT_COUNT) {
        /* Aucun slot vierge apres le dernier valide : on efface la page et on
         * repart au premier slot, mais next_seq continue sa progression
         * (jamais remis a zero) pour que "plus grand seq gagne" reste
         * valide sans ambiguite au prochain scan. */
        if (HAL_FLASH_Unlock() != HAL_OK) {
            return false;
        }
        bool erased = erase_calibration_page();
        HAL_FLASH_Lock();
        if (!erased) {
            return false;
        }
        next_index = 0;
    }

    /* Construit le slot dans un buffer local aligne, CRC calcule sur le
     * payload avant d'ajouter le CRC lui-meme. */
    uint8_t buf[CALIBRATION_FLASH_SLOT_SIZE];
    memset(buf, 0xFF, sizeof(buf));

    uint32_t magic = CALIBRATION_FLASH_MAGIC;
    memcpy(buf, &magic, sizeof(magic));
    memcpy(buf + sizeof(uint32_t), &next_seq, sizeof(next_seq));
    memcpy(buf + 2 * sizeof(uint32_t), positions, sizeof(hall_sample_t) * GEAR_POSITION_COUNT);

    uint16_t crc = crc16_ccitt(buf, CALIBRATION_SLOT_PAYLOAD_SIZE);
    memcpy(buf + CALIBRATION_SLOT_PAYLOAD_SIZE, &crc, sizeof(crc));

    /* N'ecrit que le payload utile (arrondi au halfword superieur), pas
     * tout le slot : le reste reste a 0xFF (vierge), sans effet sur
     * read_slot() qui ne lit que magic/seq/positions/crc a des offsets
     * fixes. */
    size_t write_len = CALIBRATION_SLOT_PAYLOAD_SIZE + sizeof(uint16_t);
    if (write_len % 2 != 0) {
        write_len++;
    }

    if (HAL_FLASH_Unlock() != HAL_OK) {
        return false;
    }
    bool ok = flash_write_halfwords(slot_addr(next_index), buf, write_len);
    HAL_FLASH_Lock();

    return ok;
}
