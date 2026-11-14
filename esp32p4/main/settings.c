//
// settings.c - device settings (master volume, EQ and stereo width) in NVS on the
// ESP32-P4. Implements firmware/settings.h, as the RP2350 firmware's
// settings.c does with a raw flash sector.
//
// Stored as one blob, key "settings" in namespace "fsynth" of the "nvs"
// partition (partitions.csv). The record has a version and size, so a
// record from another firmware version is ignored rather than misread.
//
// NVS writes flash, and while flash is written the flash cache is off:
// ESP-IDF holds the other core in internal RAM meanwhile. The render task
// stalls for that time (a few ms; longer if NVS has to erase a sector),
// so the audio may drop out briefly, as on the Pico.
//
#include <stddef.h>
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"

#include "settings.h"

#define NAMESPACE "fsynth"
#define KEY "settings"
#define SETTINGS_VERSION 2   // 2 added width_pct

typedef struct {
    uint32_t version;
    uint32_t size;              // sizeof(Settings) when written
    float master_volume;        // 0..1
    uint8_t eq_gs[4];           // GS EQ parameters: low freq, low gain, high freq, high gain
    uint8_t width_pct;          // Stereo width, percent (version 2)
} Settings;

// Size of a version 1 record (no width)
#define SETTINGS_V1_SIZE (offsetof(Settings, width_pct))

static bool nvs_ready;

// Mount NVS once. A partition from an older NVS version, or a full one, is
// erased and started again (it only holds these settings).
static bool nvs_init(void) {
    if (nvs_ready) return true;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    nvs_ready = err == ESP_OK;
    return nvs_ready;
}

bool settings_load(FsSynth *synth) {
    if (!nvs_init()) return false;
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    Settings s;
    size_t len = sizeof(s);
    const esp_err_t err = nvs_get_blob(h, KEY, &s, &len);
    nvs_close(h);
    if (err != ESP_OK) return false;
    const bool v1 = s.version == 1 && len == SETTINGS_V1_SIZE && s.size == SETTINGS_V1_SIZE;
    if (!v1 && (len != sizeof(s) || s.version != SETTINGS_VERSION || s.size != sizeof(s))) return false;
    float v = s.master_volume;
    if (!(v >= 0.0f && v <= 1.0f)) v = 1.0f;
    fs_synth_set_master_volume(synth, (uint16_t)(v * 16383.0f + 0.5f));
    for (uint8_t p = 0; p < 4; ++p) fs_synth_set_eq_gs(synth, p, s.eq_gs[p]);
    if (!v1) fs_synth_set_width(synth, s.width_pct);
    return true;
}

// The settings have their own partition, so the font never gets in the way.
bool settings_available(const FsFont *font) {
    (void)font;
    return nvs_init();
}

void settings_save(const FsSynth *synth) {
    if (!nvs_init()) return;
    Settings s = {
        .version = SETTINGS_VERSION,
        .size = sizeof(Settings),
        .master_volume = synth->master_volume,
        .width_pct = synth->width_pct,
    };
    memcpy(s.eq_gs, synth->eq_gs, sizeof(s.eq_gs));
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, KEY, &s, sizeof(s));
    nvs_commit(h);
    nvs_close(h);
}

void settings_erase(void) {
    if (!nvs_init()) return;
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, KEY);
    nvs_commit(h);
    nvs_close(h);
}
