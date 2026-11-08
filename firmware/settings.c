#include <string.h>
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/regs/addressmap.h"
#include "fs_crc32.h"
#include "settings.h"

#define SETTINGS_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define SETTINGS_MAGIC 0x54534653u   // "FSST"
#define SETTINGS_VERSION 1

// Stored record. New fields go at the end with a version bump; older
// records keep working for the fields they have.
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t size;              // sizeof(Settings) when written
    float master_volume;        // 0..1
    uint8_t eq_gs[4];           // GS EQ parameters: low freq, low gain, high freq, high gain
    uint32_t crc;               // CRC32 of the bytes before it
} Settings;

_Static_assert(sizeof(Settings) <= FLASH_PAGE_SIZE, "settings fit in one flash page");

bool settings_available(const FsFont *font) {
    return FS_FONT_FLASH_OFFSET + font->header->total_size <= SETTINGS_OFFSET;
}

bool settings_load(FsSynth *synth) {
    const Settings *s = (const Settings *)(XIP_BASE + SETTINGS_OFFSET);
    if (s->magic != SETTINGS_MAGIC || s->version != SETTINGS_VERSION || s->size != sizeof(Settings)) return false;
    if (s->crc != fs_crc32(0, s, offsetof(Settings, crc))) return false;
    float v = s->master_volume;
    if (!(v >= 0.0f && v <= 1.0f)) v = 1.0f;
    fs_synth_set_master_volume(synth, (uint16_t)(v * 16383.0f + 0.5f));
    for (uint8_t p = 0; p < 4; ++p) fs_synth_set_eq_gs(synth, p, s->eq_gs[p]);
    return true;
}

static void __not_in_flash_func(write_sector)(const uint8_t *page) {
    const uint32_t irq = save_and_disable_interrupts();
    flash_range_erase(SETTINGS_OFFSET, FLASH_SECTOR_SIZE);
    if (page) flash_range_program(SETTINGS_OFFSET, page, FLASH_PAGE_SIZE);
    restore_interrupts(irq);
}

void settings_save(const FsSynth *synth) {
    static uint8_t page[FLASH_PAGE_SIZE] __attribute__((aligned(4)));
    memset(page, 0xff, sizeof(page));
    Settings s = {
        .magic = SETTINGS_MAGIC,
        .version = SETTINGS_VERSION,
        .size = sizeof(Settings),
        .master_volume = synth->master_volume,
    };
    memcpy(s.eq_gs, synth->eq_gs, sizeof(s.eq_gs));
    s.crc = fs_crc32(0, &s, offsetof(Settings, crc));
    memcpy(page, &s, sizeof(s));
    write_sector(page);
}

void settings_erase(void) {
    write_sector(NULL);
}
