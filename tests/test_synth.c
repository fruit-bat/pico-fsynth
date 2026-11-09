//
// test_synth.c - tests of the font reader and the synthesizer (fs_font.c,
// fs_voice.c, fs_synth.c, fs_effects.c) using small fonts built in memory
// (font_builder.h).
//
#include "test.h"
#include "font_builder.h"
#include "fs_synth.h"

#define RATE 44100
#define B FS_BLOCK_SIZE

// The test font. Presets:
//   0:0  one looping region, 0.25 s release
//   0:1  one one-shot region (the sample is 0.1 s long)
//   0:2  two layered regions on every key
//   128:0 drum kit: key 36, and keys 42 / 46 in exclusive class 1
static const TbRegion font_regions[] = {
    { .bank = 0, .program = 0, .release_tc = -2400 },
    { .bank = 0, .program = 1, .one_shot = true },
    { .bank = 0, .program = 2 },
    { .bank = 0, .program = 2 },
    { .bank = 128, .program = 0, .key_lo = 36, .key_hi = 36 },
    { .bank = 128, .program = 0, .key_lo = 42, .key_hi = 42, .exclusive_class = 1 },
    { .bank = 128, .program = 0, .key_lo = 46, .key_hi = 46, .exclusive_class = 1 },
};

static TbFont tb;
static FsFont font;
static FsSynth synth;       // Large: static
static FsSynth synth2;

static void setup(void) {
    fs_synth_init(&synth, &font, RATE);
    fs_synth_set_effects(&synth, false, false);
}

static void midi3(FsSynth *s, uint8_t a, uint8_t b, uint8_t c) {
    const uint8_t m[3] = { a, b, c };
    fs_synth_midi(s, m, 3);
}

// Render n blocks; returns the largest output magnitude
static float render(FsSynth *s, int blocks) {
    float l[B], r[B], peak = 0.0f;
    for (int i = 0; i < blocks; ++i) {
        fs_synth_render_float(s, l, r);
        for (int k = 0; k < B; ++k) peak = fmaxf(peak, fmaxf(fabsf(l[k]), fabsf(r[k])));
    }
    return peak;
}

// Frequency of the left output over about one second, from rising zero
// crossings
static double measure_hz(FsSynth *s) {
    float l[B], r[B], prev = 0.0f;
    int crossings = 0;
    const int blocks = RATE / B;
    for (int i = 0; i < blocks; ++i) {
        fs_synth_render_float(s, l, r);
        for (int k = 0; k < B; ++k) {
            if (prev < 0.0f && l[k] >= 0.0f) ++crossings;
            prev = l[k];
        }
    }
    return (double)crossings * RATE / (blocks * B);
}

static uint32_t voices_in_state(const FsSynth *s, FsVoiceState state) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < s->active_count; ++i) n += s->voices[s->active[i]].state == state;
    return n;
}

// ---------------------------------------------------------------------------
// Font reader
// ---------------------------------------------------------------------------

TEST(font_init_validation) {
    FsFont f;
    CHECK_EQ(fs_font_init(&f, tb.blob, tb.size, true, true), FS_FONT_OK);
    CHECK_EQ(fs_font_init(&f, tb.blob, tb.size - 1, true, true), FS_FONT_ERR_SIZE);

    uint8_t *copy = malloc(tb.size);
    memcpy(copy, tb.blob, tb.size);
    copy[0] ^= 1;
    CHECK_EQ(fs_font_init(&f, copy, tb.size, true, true), FS_FONT_ERR_MAGIC);

    memcpy(copy, tb.blob, tb.size);
    const FsFontHeader *h = (const FsFontHeader *)tb.blob;
    copy[h->region_offset + 3] ^= 1;
    CHECK_EQ(fs_font_init(&f, copy, tb.size, true, true), FS_FONT_ERR_TABLE_CRC);

    memcpy(copy, tb.blob, tb.size);
    copy[h->data_offset + 100] ^= 1;
    CHECK_EQ(fs_font_init(&f, copy, tb.size, true, true), FS_FONT_ERR_DATA_CRC);
    CHECK_EQ(fs_font_init(&f, copy, tb.size, true, false), FS_FONT_OK);

    // A region pointing past the sample data is refused even without CRCs
    memcpy(copy, tb.blob, tb.size);
    FsRegion *r = (FsRegion *)(copy + h->region_offset);
    r->end = h->data_frames + 1;
    CHECK_EQ(fs_font_init(&f, copy, tb.size, false, false), FS_FONT_ERR_REGION);
    free(copy);
}

TEST(find_preset) {
    const FsPreset *p = fs_font_find_preset(&font, 0, 2);
    CHECK(p != NULL);
    if (p) CHECK_EQ(p->region_count, 2);
    CHECK(fs_font_find_preset(&font, 128, 0) != NULL);
    CHECK(fs_font_find_preset(&font, 0, 3) == NULL);
    CHECK(fs_font_find_preset(&font, 1, 0) == NULL);
}

// ---------------------------------------------------------------------------
// Notes
// ---------------------------------------------------------------------------

TEST(pitch) {
    setup();
    midi3(&synth, 0x90, 69, 100);           // Root key: 440 Hz
    render(&synth, 10);
    CHECK_NEAR(measure_hz(&synth), 440.0, 2.0);
    midi3(&synth, 0x80, 69, 0);
    setup();
    midi3(&synth, 0x90, 81, 100);           // One octave up
    render(&synth, 10);
    CHECK_NEAR(measure_hz(&synth), 880.0, 2.0);
    setup();
    midi3(&synth, 0xe0, 0x7f, 0x7f);        // Full bend up: +2 semitones
    midi3(&synth, 0x90, 69, 100);
    render(&synth, 10);
    CHECK_NEAR(measure_hz(&synth), 440.0 * pow(2.0, 2.0 / 12.0), 2.0);
}

TEST(note_on_off) {
    setup();
    CHECK_NEAR(render(&synth, 4), 0.0, 0.0);
    midi3(&synth, 0x90, 60, 100);
    CHECK_EQ(fs_synth_active_voices(&synth), 1);
    CHECK(render(&synth, 20) > 0.01f);
    midi3(&synth, 0x80, 60, 0);
    CHECK_EQ(voices_in_state(&synth, FS_VOICE_RELEASED), 1);
    render(&synth, RATE / B);               // 1 s: well past the 0.25 s release
    CHECK_EQ(fs_synth_active_voices(&synth), 0);
    CHECK(render(&synth, 4) < 1e-4f);
}

TEST(velocity_zero_is_note_off) {
    setup();
    midi3(&synth, 0x90, 60, 100);
    render(&synth, 4);
    midi3(&synth, 0x90, 60, 0);
    CHECK_EQ(voices_in_state(&synth, FS_VOICE_RELEASED), 1);
}

TEST(sustain_pedal) {
    setup();
    midi3(&synth, 0xb0, 64, 127);
    midi3(&synth, 0x90, 60, 100);
    render(&synth, 4);
    midi3(&synth, 0x80, 60, 0);
    CHECK_EQ(voices_in_state(&synth, FS_VOICE_SUSTAINED), 1);
    render(&synth, RATE / B);
    CHECK_EQ(fs_synth_active_voices(&synth), 1);   // Still held by the pedal
    midi3(&synth, 0xb0, 64, 0);
    CHECK_EQ(voices_in_state(&synth, FS_VOICE_RELEASED), 1);
    render(&synth, RATE / B);
    CHECK_EQ(fs_synth_active_voices(&synth), 0);
}

TEST(one_shot_ends_by_itself) {
    setup();
    midi3(&synth, 0xc0, 1, 0);
    midi3(&synth, 0x90, 69, 100);
    render(&synth, RATE / B / 2);           // The sample lasts 0.1 s
    CHECK_EQ(fs_synth_active_voices(&synth), 0);
}

TEST(layers_and_restrike) {
    setup();
    midi3(&synth, 0xc0, 2, 0);
    midi3(&synth, 0x90, 60, 100);
    CHECK_EQ(fs_synth_active_voices(&synth), 2);
    render(&synth, 4);
    midi3(&synth, 0x90, 60, 100);           // Same key again
    CHECK_EQ(fs_synth_active_voices(&synth), 4);
    CHECK_EQ(voices_in_state(&synth, FS_VOICE_RELEASED), 2);
    CHECK_EQ(voices_in_state(&synth, FS_VOICE_ON), 2);
}

TEST(exclusive_class) {
    setup();
    midi3(&synth, 0x99, 46, 100);           // Open hi-hat
    midi3(&synth, 0x99, 36, 100);           // Kick: no class
    render(&synth, 4);
    midi3(&synth, 0x99, 42, 100);           // Closed hi-hat cuts the open one
    render(&synth, 20);
    CHECK_EQ(fs_synth_active_voices(&synth), 2);
    bool open = false;
    for (uint32_t i = 0; i < synth.active_count; ++i) open |= synth.voices[synth.active[i]].key == 46;
    CHECK(!open);
}

// Programs missing from the font fall back to bank 0, then to 0:0
TEST(program_fallback) {
    setup();
    midi3(&synth, 0xb0, 0, 5);              // Bank 5 (not in the font)
    midi3(&synth, 0xc0, 1, 0);
    CHECK(synth.presets[0] == fs_font_find_preset(&font, 0, 1));
    midi3(&synth, 0xc0, 50, 0);
    CHECK(synth.presets[0] == fs_font_find_preset(&font, 0, 0));
    midi3(&synth, 0xc9, 77, 0);             // Missing drum kit: kit 0
    CHECK(synth.presets[9] == fs_font_find_preset(&font, 128, 0));
}

TEST(all_sound_off) {
    setup();
    for (int k = 40; k < 60; ++k) midi3(&synth, 0x90, (uint8_t)k, 100);
    render(&synth, 10);
    fs_synth_all_sound_off(&synth);
    render(&synth, 20);                     // A few ms fade
    CHECK_EQ(fs_synth_active_voices(&synth), 0);
    CHECK(render(&synth, 2) < 1e-4f);
}

TEST(gm_reset_sysex) {
    setup();
    midi3(&synth, 0xc0, 2, 0);
    midi3(&synth, 0xb0, 7, 10);
    midi3(&synth, 0x90, 60, 100);
    const uint8_t gm_on[] = { 0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7 };
    fs_synth_midi(&synth, gm_on, sizeof(gm_on));
    CHECK_EQ(synth.channels[0].program, 0);
    CHECK_EQ(synth.channels[0].cc[7], 100);
    render(&synth, 20);
    CHECK_EQ(fs_synth_active_voices(&synth), 0);
}

// Far more notes than voices: the pool limits hold, the bookkeeping stays
// consistent, and the most recent note always sounds
TEST(voice_stealing) {
    setup();
    for (int n = 0; n < 300; ++n) {
        const uint8_t ch = (uint8_t)(n % 16 == 9 ? 0 : n % 16);
        const uint8_t key = (uint8_t)(30 + n % 70);
        midi3(&synth, (uint8_t)(0x90 | ch), key, 100);
        CHECK(synth.active_count <= FS_TOTAL_VOICES);
        CHECK(synth.active_count - synth.dying_count <= FS_MAX_VOICES);
        bool newest = false;
        for (uint32_t i = 0; i < synth.active_count; ++i) {
            const FsVoice *v = &synth.voices[synth.active[i]];
            newest |= v->channel == ch && v->key == key && v->state == FS_VOICE_ON && !v->stolen;
        }
        CHECK(newest);
        if (n % 3 == 0) render(&synth, 1);
    }
    CHECK(synth.stats.voices_stolen > 0);
    // key_voices counts exactly the live (not stolen) voices
    uint32_t counted = 0;
    for (int c = 0; c < 16; ++c) for (int k = 0; k < 128; ++k) counted += synth.key_voices[c][k];
    CHECK_EQ(counted, synth.active_count - synth.dying_count);
    fs_synth_all_sound_off(&synth);
    render(&synth, 50);
    CHECK_EQ(fs_synth_active_voices(&synth), 0);
    counted = 0;
    for (int c = 0; c < 16; ++c) for (int k = 0; k < 128; ++k) counted += synth.key_voices[c][k];
    CHECK_EQ(counted, 0);
}

// Rendering in parts, in any order, gives exactly the same output as
// fs_synth_render (so the parts really share no state)
TEST(split_render_matches) {
    FsSynth *a = &synth, *b = &synth2;
    fs_synth_init(a, &font, RATE);
    fs_synth_init(b, &font, RATE);
    for (int c = 0; c < 4; ++c) {
        midi3(a, (uint8_t)(0xb0 | c), 91, 100);
        midi3(b, (uint8_t)(0xb0 | c), 91, 100);
        midi3(a, (uint8_t)(0xb0 | c), 93, 60);
        midi3(b, (uint8_t)(0xb0 | c), 93, 60);
        for (int k = 0; k < 6; ++k) {
            midi3(a, (uint8_t)(0x90 | c), (uint8_t)(48 + 7 * k + c), 100);
            midi3(b, (uint8_t)(0x90 | c), (uint8_t)(48 + 7 * k + c), 100);
        }
    }
    int16_t oa[B * 2], ob[B * 2];
    int diffs = 0;
    for (int blk = 0; blk < 400; ++blk) {
        if (blk == 200) {
            midi3(a, 0xb0, 64, 0);
            midi3(b, 0xb0, 64, 0);
            midi3(a, 0x80, 48, 0);
            midi3(b, 0x80, 48, 0);
        }
        fs_synth_render(a, oa);
        fs_synth_render_begin(b);
        for (int p = FS_RENDER_PARTS - 1; p >= 0; --p) fs_synth_render_part(b, (uint32_t)p);
        fs_synth_render_end(b, ob);
        diffs += memcmp(oa, ob, sizeof(oa)) != 0;
    }
    CHECK_EQ(diffs, 0);
    CHECK(fs_synth_active_voices(a) > 0);
}

int main(void) {
    tb_build(&tb, font_regions, sizeof(font_regions) / sizeof(font_regions[0]));
    if (fs_font_init(&font, tb.blob, tb.size, true, true) != FS_FONT_OK) {
        fprintf(stderr, "test font did not load\n");
        return 1;
    }
    RUN(font_init_validation);
    RUN(find_preset);
    RUN(pitch);
    RUN(note_on_off);
    RUN(velocity_zero_is_note_off);
    RUN(sustain_pedal);
    RUN(one_shot_ends_by_itself);
    RUN(layers_and_restrike);
    RUN(exclusive_class);
    RUN(program_fallback);
    RUN(all_sound_off);
    RUN(gm_reset_sysex);
    RUN(voice_stealing);
    RUN(split_render_matches);
    tb_free(&tb);
    return test_summary();
}
