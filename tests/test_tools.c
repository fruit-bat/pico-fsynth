//
// test_tools.c - end-to-end tests of the host tools: SoundFonts written by
// sf2_writer.h are converted with the real sf2conv binary and the result
// is loaded with the synth's own font reader; fsrender is run on a small
// MIDI file.
//
// Usage (CTest passes these): test_tools path/to/sf2conv path/to/fsrender
// Scratch files are written to the current directory.
//
#include <stdlib.h>
#include <sys/wait.h>
#include "test.h"
#include "sf2_writer.h"
#include "midi_writer.h"
#include "fs_font.h"

static const char *sf2conv;
static const char *fsrender;

// Run a shell command quietly; returns its exit status, or -1 if it did not
// exit normally (e.g. crashed)
static int run(const char *fmt, const char *a, const char *b, const char *c) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), fmt, a, b, c);
    const int st = system(cmd);
    if (st == -1 || !WIFEXITED(st)) return -1;
    return WEXITSTATUS(st);
}

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    rewind(f);
    uint8_t *p = malloc(n > 0 ? (size_t)n : 1);
    if (fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); p = NULL; }
    fclose(f);
    *size = (size_t)n;
    return p;
}

// Convert presets/instruments to tt_<name>.fsf and load it. Returns the
// blob (free it) or NULL if conversion or loading failed.
static uint8_t *convert(const char *name, const SwPreset *p, int np, const SwInst *in, int ni, FsFont *font) {
    char sf2[256], fsf[256];
    snprintf(sf2, sizeof(sf2), "tt_%s.sf2", name);
    snprintf(fsf, sizeof(fsf), "tt_%s.fsf", name);
    size_t size;
    uint8_t *data = sw_build(p, np, in, ni, &size);
    CHECK_EQ(sw_write_file(sf2, data, size), 0);
    free(data);
    const int st = run("'%s' -v '%s' '%s' >/dev/null 2>&1", sf2conv, sf2, fsf);
    CHECK_EQ(st, 0);
    if (st != 0) return NULL;
    uint8_t *blob = read_file(fsf, &size);
    CHECK(blob != NULL);
    if (!blob) return NULL;
    const FsFontResult res = fs_font_init(font, blob, size, true, true);
    CHECK_EQ(res, FS_FONT_OK);
    if (res != FS_FONT_OK) { free(blob); return NULL; }
    return blob;
}

TEST(sf2conv_basic) {
    const SwInst insts[] = { { 0, 127, 0 } };
    const SwPreset presets[] = { { "Drums", 128, 0, 0 }, { "Piano", 0, 0, 0 } };
    FsFont font;
    uint8_t *blob = convert("basic", presets, 2, insts, 1, &font);
    if (!blob) return;
    CHECK_EQ(font.header->preset_count, 2);
    CHECK_EQ(font.header->region_count, 2);
    // Sorted by bank: piano first
    CHECK_EQ(font.presets[0].bank, 0);
    CHECK_EQ(font.presets[1].bank, 128);
    const FsRegion *r = &font.regions[0];
    CHECK_EQ(r->root_key, 69);
    CHECK_EQ(r->sample_rate, 44100);
    CHECK_EQ(r->loop_mode, FS_LOOP_CONTINUOUS);
    CHECK_EQ(r->loop_end - r->loop_start, 1000);
    CHECK_EQ(r->mod_count, FS_DEFAULT_MODULATOR_COUNT);
    free(blob);
}

// Regression: with duplicate bank/program the first preset in the file is
// kept, and the regions of the dropped one do not stay in the output.
TEST(sf2conv_duplicate_presets) {
    const SwInst insts[] = { { 0, 63, 0 }, { 64, 127, 0 } };
    // Several duplicates, so an unstable sort would be likely to reorder them
    const SwPreset presets[] = {
        { "Second", 0, 1, 0 },
        { "First", 0, 0, 0 },
        { "Dup A", 0, 0, 1 }, { "Dup B", 0, 0, 1 }, { "Dup C", 0, 0, 1 },
        { "Dup D", 0, 0, 1 }, { "Dup E", 0, 0, 1 }, { "Dup F", 0, 0, 1 },
    };
    FsFont font;
    uint8_t *blob = convert("dup", presets, 8, insts, 2, &font);
    if (!blob) return;
    CHECK_EQ(font.header->preset_count, 2);
    CHECK_EQ(font.header->region_count, 2);        // No dead regions
    const FsPreset *p = fs_font_find_preset(&font, 0, 0);
    CHECK(p != NULL);
    if (p) {
        CHECK(strncmp(p->name, "First", 5) == 0);
        CHECK_EQ(p->region_count, 1);
        CHECK_EQ(fs_font_preset_region(&font, p, 0)->key_hi, 63);
    }
    free(blob);
}

// Regression: exclusive classes outside 0..127 are ignored, not wrapped
// onto a real class
TEST(sf2conv_exclusive_class_range) {
    const SwInst insts[] = { { 0, 127, 5 }, { 0, 127, 133 } };
    const SwPreset presets[] = { { "A", 0, 0, 0 }, { "B", 0, 1, 1 } };
    FsFont font;
    uint8_t *blob = convert("excl", presets, 2, insts, 2, &font);
    if (!blob) return;
    CHECK_EQ(fs_font_preset_region(&font, fs_font_find_preset(&font, 0, 0), 0)->exclusive_class, 5);
    CHECK_EQ(fs_font_preset_region(&font, fs_font_find_preset(&font, 0, 1), 0)->exclusive_class, 0);
    free(blob);
}

// Regression: a RIFF size below 4 used to wrap around and read past the
// file. Bad input must give a clean error (exit status 1), never a crash.
TEST(sf2conv_bad_input) {
    uint8_t bad[76] = { 'R', 'I', 'F', 'F', 2, 0, 0, 0, 's', 'f', 'b', 'k' };
    for (size_t i = 12; i < sizeof(bad); ++i) bad[i] = (uint8_t)(i * 37);
    CHECK_EQ(sw_write_file("tt_tiny.sf2", bad, sizeof(bad)), 0);
    CHECK_EQ(run("'%s' '%s' '%s' >/dev/null 2>&1", sf2conv, "tt_tiny.sf2", "tt_tiny.fsf"), 1);

    // Every truncation of a valid file
    const SwInst insts[] = { { 0, 127, 0 } };
    const SwPreset presets[] = { { "P", 0, 0, 0 } };
    size_t size;
    uint8_t *good = sw_build(presets, 1, insts, 1, &size);
    int crashes = 0;
    for (size_t n = 0; n < size; n += (n < 200 ? 1 : 97)) {
        sw_write_file("tt_trunc.sf2", good, n);
        const int st = run("'%s' '%s' '%s' >/dev/null 2>&1", sf2conv, "tt_trunc.sf2", "tt_trunc.fsf");
        if (st != 0 && st != 1) ++crashes;
    }
    CHECK_EQ(crashes, 0);
    free(good);
}

// Regression: fsrender -R / -C with only some values used to set the rest
// to 0; they must take the defaults, so "-R 0.5" equals the full form.
TEST(fsrender_partial_effect_options) {
    const SwInst insts[] = { { 0, 127, 0 } };
    const SwPreset presets[] = { { "P", 0, 0, 0 } };
    FsFont font;
    uint8_t *blob = convert("render", presets, 1, insts, 1, &font);
    if (!blob) return;
    free(blob);
    MwFile f;
    mw_begin(&f, 0, 96);
    mw_track(&f);
    mw_event(&f, 0, 3, 0xb0, 91, 127);
    mw_event(&f, 0, 3, 0xb0, 93, 127);
    mw_event(&f, 0, 3, 0x90, 60, 100);
    mw_event(&f, 96, 3, 0x80, 60, 0);
    mw_end_track(&f, 0);
    CHECK_EQ(sw_write_file("tt_render.mid", f.data, f.size), 0);

    const char *fmt = "'%s' %s tt_render.fsf tt_render.mid %s >/dev/null 2>&1";
    CHECK_EQ(run(fmt, fsrender, "-R 0.5 -C 0.6", "tt_short.wav"), 0);
    CHECK_EQ(run(fmt, fsrender, "-R 0.5,0.2,1,0.2 -C 0.6,4,0.9", "tt_full.wav"), 0);
    CHECK_EQ(run(fmt, fsrender, "", "tt_default.wav"), 0);
    size_t na, nb, nc;
    uint8_t *a = read_file("tt_short.wav", &na), *b = read_file("tt_full.wav", &nb), *c = read_file("tt_default.wav", &nc);
    CHECK(a && b && c);
    if (a && b && c) {
        CHECK(na > 44 && na == nb);
        CHECK(na == nb && memcmp(a, b, na) == 0);
        // And the room size did change something
        CHECK(na != nc || memcmp(a, c, na) != 0);
    }
    free(a); free(b); free(c);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: test_tools sf2conv fsrender\n");
        return 2;
    }
    sf2conv = argv[1];
    fsrender = argv[2];
    RUN(sf2conv_basic);
    RUN(sf2conv_duplicate_presets);
    RUN(sf2conv_exclusive_class_range);
    RUN(sf2conv_bad_input);
    RUN(fsrender_partial_effect_options);
    return test_summary();
}
