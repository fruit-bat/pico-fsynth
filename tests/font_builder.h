#pragma once
//
// font_builder.h - build small compiled fonts (.fsf images) in memory for
// tests, without going through sf2conv.
//
// Every region plays the same sample: a 440 Hz sine recorded at 44 kHz
// (exactly 100 frames per cycle), root key 69, looped over whole cycles,
// so a note's output frequency is easy to check. Regions get the SF2
// generator defaults and default modulators, as sf2conv would give them,
// plus the few settings a test asks for.
//
//   TbRegion r[] = { { .bank = 0, .program = 0 }, ... };   // sorted by bank, program
//   TbFont f;  tb_build(&f, r, n);   ... f.blob, f.size ...
//
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include "fs_font.h"
#include "fs_crc32.h"

#define TB_SAMPLE_FRAMES 4400
#define TB_SAMPLE_RATE 44000

// What a test can set per region; zero means the default
typedef struct {
    uint16_t bank, program;         // Regions with the same pair form one preset
    uint8_t key_lo, key_hi;         // 0, 0 = all keys
    uint8_t exclusive_class;
    bool one_shot;                  // Default: loop continuously
    int16_t release_tc;             // Volume envelope release; 0 = SF2 default (instant)
} TbRegion;

typedef struct {
    uint8_t *blob;
    size_t size;
} TbFont;

static inline void tb_build(TbFont *f, const TbRegion *in, uint32_t n) {
    // Count presets (runs of equal bank/program)
    uint32_t np = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (i == 0 || in[i].bank != in[i - 1].bank || in[i].program != in[i - 1].program) ++np;
    }
    FsFontHeader h;
    memset(&h, 0, sizeof(h));
    h.magic = FS_FONT_MAGIC;
    h.version = FS_FONT_VERSION;
    h.header_size = sizeof(FsFontHeader);
    h.preset_count = np;
    h.preset_offset = sizeof(FsFontHeader);
    h.region_count = n;
    h.region_offset = h.preset_offset + np * sizeof(FsPreset);
    h.mod_count = FS_DEFAULT_MODULATOR_COUNT;
    h.mod_offset = h.region_offset + n * sizeof(FsRegion);
    const uint32_t tables_end = h.mod_offset + FS_DEFAULT_MODULATOR_COUNT * sizeof(FsModulator);
    h.data_offset = (tables_end + FS_FONT_DATA_ALIGN - 1) & ~(uint32_t)(FS_FONT_DATA_ALIGN - 1);
    h.data_frames = TB_SAMPLE_FRAMES;
    h.total_size = h.data_offset + TB_SAMPLE_FRAMES * 2;
    strcpy(h.name, "test font");

    uint8_t *b = calloc(1, h.total_size);
    FsPreset *presets = (FsPreset *)(b + h.preset_offset);
    FsRegion *regions = (FsRegion *)(b + h.region_offset);
    uint32_t p = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (i == 0 || in[i].bank != in[i - 1].bank || in[i].program != in[i - 1].program) {
            FsPreset *pr = &presets[p++];
            snprintf(pr->name, sizeof(pr->name), "preset %u:%u", in[i].bank, in[i].program);
            pr->bank = in[i].bank;
            pr->program = in[i].program;
            pr->region_first = i;
        }
        presets[p - 1].region_count++;

        FsRegion *r = &regions[i];
        for (int g = 0; g < FS_GEN_COUNT; ++g) r->gen[g] = fs_gen_default[g];
        r->start = 0;
        r->end = TB_SAMPLE_FRAMES;
        r->loop_start = 1000;
        r->loop_end = 4000;                 // 30 whole cycles
        r->loop_mode = in[i].one_shot ? FS_LOOP_NONE : FS_LOOP_CONTINUOUS;
        r->sample_rate = TB_SAMPLE_RATE;
        r->root_key = 69;
        r->key_lo = in[i].key_lo;
        r->key_hi = in[i].key_hi ? in[i].key_hi : 127;
        r->vel_lo = 0;
        r->vel_hi = 127;
        r->exclusive_class = in[i].exclusive_class;
        if (in[i].release_tc) r->gen[FS_GEN_RELEASE_VOL_ENV] = in[i].release_tc;
        r->mod_first = 0;
        r->mod_count = FS_DEFAULT_MODULATOR_COUNT;
    }
    memcpy(b + h.mod_offset, fs_default_modulators, sizeof(fs_default_modulators));
    int16_t *data = (int16_t *)(b + h.data_offset);
    for (int i = 0; i < TB_SAMPLE_FRAMES; ++i) {
        data[i] = (int16_t)(16000.0 * sin(2.0 * M_PI * (double)i / 100.0));
    }
    h.table_crc = fs_crc32(0, b + h.header_size, h.data_offset - h.header_size);
    h.data_crc = fs_crc32(0, b + h.data_offset, TB_SAMPLE_FRAMES * 2);
    memcpy(b, &h, sizeof(h));
    f->blob = b;
    f->size = h.total_size;
}

static inline void tb_free(TbFont *f) {
    free(f->blob);
    f->blob = NULL;
}
