#pragma once
//
// fs_font.h - the compiled font (.fsf) format and its reader.
//
// A SoundFont is a tree: presets contain zones that point at instruments,
// whose zones point at samples, with values that combine across levels.
// Walking that tree at note-on is too slow and too memory hungry for a
// microcontroller, so sf2conv flattens it on the host. Each preset becomes
// a list of regions, and each region is everything needed to play one
// sample: key/velocity range, sample positions, summed generator values
// and a modulator list.
//
// Produced on the host by sf2conv from a SoundFont 2 file and read in place
// (e.g. from XIP flash) by the synth. All values are little-endian and the
// structures use natural alignment, so the same layout is valid on x86-64
// and Cortex-M33.
//
// Layout:
//   FsFontHeader
//   FsPreset[preset_count]     sorted by (bank, program)
//   FsRegion[region_count]     fully resolved preset x instrument zones
//   FsModulator[mod_count]     shared, de-duplicated modulator lists
//   (padding to FS_FONT_DATA_ALIGN)
//   int16_t[data_frames]       mono 16-bit sample data
//
// Generator values in FsRegion are in native SF2 units (timecents, cents,
// centibels...) with instrument and preset contributions already summed.
// Modulators (including the SF2 default modulators) still need applying
// at note-on, which is why units are not converted offline.
//
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "fs_gen.h"

#define FS_FONT_MAGIC       0x31465346u  // "FSF1"
#define FS_FONT_VERSION     1
#define FS_FONT_DATA_ALIGN  4096         // Flash sector size

// File header, at offset 0. Offsets are bytes from the start of the blob.
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;    // sizeof(FsFontHeader)
    uint32_t total_size;     // Size of the whole blob in bytes
    uint32_t table_crc;      // CRC32 of bytes [header_size, data_offset)
    uint32_t data_crc;       // CRC32 of the sample data
    uint32_t preset_count;
    uint32_t preset_offset;
    uint32_t region_count;
    uint32_t region_offset;
    uint32_t mod_count;
    uint32_t mod_offset;
    uint32_t data_offset;    // Byte offset of the int16 sample data
    uint32_t data_frames;    // Number of int16 sample frames
    char name[32];           // From the SoundFont INAM chunk
} FsFontHeader;

// One preset (a playable program): a contiguous run of regions
// [region_first, region_first + region_count) in the region table.
typedef struct {
    char name[20];           // Not necessarily NUL terminated
    uint16_t bank;           // 128 = GM percussion
    uint16_t program;
    uint32_t region_first;
    uint32_t region_count;
} FsPreset;

// One region: a sample plus everything needed to play it. A note-on
// starts one voice for every region of the channel's preset whose key and
// velocity ranges contain the note (several for layered sounds).
typedef struct {
    // Sample positions as frame indices into the font sample data.
    // Address offset generators have already been applied and clamped.
    uint32_t start;          // First frame
    uint32_t end;            // One past the last frame
    uint32_t loop_start;     // First frame of the loop
    uint32_t loop_end;       // One past the last frame of the loop
    uint32_t sample_rate;    // Hz
    uint16_t mod_first;      // Index of first modulator in the shared table
    uint16_t mod_count;
    uint8_t key_lo;
    uint8_t key_hi;
    uint8_t vel_lo;
    uint8_t vel_hi;
    uint8_t root_key;        // Overriding root key or the sample's original pitch
    int8_t pitch_correction; // Sample pitch correction in cents
    uint8_t loop_mode;       // FsLoopMode
    uint8_t exclusive_class; // 0 = none; a new note in a class cuts the others
    int16_t gen[FS_GEN_COUNT];  // Generator values, indexed by FsGen
} FsRegion;

_Static_assert(sizeof(FsFontHeader) == 84, "FsFontHeader layout");
_Static_assert(sizeof(FsPreset) == 32, "FsPreset layout");
_Static_assert(sizeof(FsRegion) == 156, "FsRegion layout");
_Static_assert(sizeof(FsModulator) == 12, "FsModulator layout");

// An attached font: pointers into the blob, set up by fs_font_init. The
// blob is never copied and must stay mapped while the font is in use.
typedef struct {
    const uint8_t *base;
    const FsFontHeader *header;
    const FsPreset *presets;
    const FsRegion *regions;
    const FsModulator *mods;
    const int16_t *data;
} FsFont;

// Result of fs_font_init. fs_font_result_str gives a readable message.
typedef enum {
    FS_FONT_OK = 0,
    FS_FONT_ERR_MAGIC,
    FS_FONT_ERR_VERSION,
    FS_FONT_ERR_SIZE,
    FS_FONT_ERR_LAYOUT,
    FS_FONT_ERR_TABLE_CRC,
    FS_FONT_ERR_DATA_CRC,
    FS_FONT_ERR_PRESET,
    FS_FONT_ERR_REGION,
} FsFontResult;

// Attach to a compiled font. If size is 0 the size in the header is trusted
// (e.g. when the font lives at a fixed flash address). Checking the data CRC
// reads the whole font and can take a while from flash.
//
// blob   the font image, 4-byte aligned, read in place (not copied).
// size   bytes available at blob: the font must fit. 0 = don't check.
// Checks the header, that every table lies inside the blob, the optional
// CRCs, and every preset and region reference, so the synth never needs to
// bounds check at run time. Returns FS_FONT_OK and fills *font on success;
// on failure *font is left unusable.
FsFontResult fs_font_init(FsFont *font, const void *blob, size_t size,
                          bool check_table_crc, bool check_data_crc);

// Short English description of a result code, for error messages.
const char *fs_font_result_str(FsFontResult result);

// Find a preset by bank and program, NULL if not present. Binary search
// (the preset table is sorted), so it is cheap enough for program changes.
const FsPreset *fs_font_find_preset(const FsFont *font, uint16_t bank, uint16_t program);

// Region i (0 .. region_count-1) of a preset.
static inline const FsRegion *fs_font_preset_region(const FsFont *font, const FsPreset *preset, uint32_t i) {
    return &font->regions[preset->region_first + i];
}

// True if a note with this key and velocity should play the region.
static inline bool fs_region_matches(const FsRegion *region, uint8_t key, uint8_t vel) {
    return key >= region->key_lo && key <= region->key_hi &&
           vel >= region->vel_lo && vel <= region->vel_hi;
}

// First of the region's mod_count modulators in the shared table.
static inline const FsModulator *fs_font_region_mods(const FsFont *font, const FsRegion *region) {
    return &font->mods[region->mod_first];
}
