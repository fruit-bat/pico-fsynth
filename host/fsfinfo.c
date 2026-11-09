//
// fsfinfo.c - inspect a compiled .fsf font using the same reader as the synth.
//
// Loads the font with fs_font_init() with both CRC checks on, so a clean
// run also proves the file is intact and passes the synth's validation.
// Useful to check what sf2conv produced: which presets exist and, for one
// preset, exactly what each region contains (ranges, sample positions,
// loop mode, non-default generators and the modulator list).
//
// Usage: fsfinfo font.fsf                 list presets
//        fsfinfo font.fsf bank program    list the regions of one preset
//
// Modulators are printed as [src->destination amount amt amt_src tN]
// with src and amt_src as raw SF2 source words in hex (see fs_gen.h).
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs_font.h"

// SF2 spec name of generator g (index into FsRegion.gen), or "?" if out of
// range. Generator 59 is the synth's internal pitch destination and 61 is
// the padding slot that rounds FS_GEN_COUNT up.
static const char *gen_name(int g) {
    static const char *names[FS_GEN_COUNT] = {
        "startAddrsOffset", "endAddrsOffset", "startloopAddrsOffset", "endloopAddrsOffset",
        "startAddrsCoarseOffset", "modLfoToPitch", "vibLfoToPitch", "modEnvToPitch",
        "initialFilterFc", "initialFilterQ", "modLfoToFilterFc", "modEnvToFilterFc",
        "endAddrsCoarseOffset", "modLfoToVolume", "unused1", "chorusEffectsSend",
        "reverbEffectsSend", "pan", "unused2", "unused3", "unused4", "delayModLFO",
        "freqModLFO", "delayVibLFO", "freqVibLFO", "delayModEnv", "attackModEnv",
        "holdModEnv", "decayModEnv", "sustainModEnv", "releaseModEnv",
        "keynumToModEnvHold", "keynumToModEnvDecay", "delayVolEnv", "attackVolEnv",
        "holdVolEnv", "decayVolEnv", "sustainVolEnv", "releaseVolEnv",
        "keynumToVolEnvHold", "keynumToVolEnvDecay", "instrument", "reserved1",
        "keyRange", "velRange", "startloopAddrsCoarseOffset", "keynum", "velocity",
        "initialAttenuation", "reserved2", "endloopAddrsCoarseOffset", "coarseTune",
        "fineTune", "sampleID", "sampleModes", "reserved3", "scaleTuning",
        "exclusiveClass", "overridingRootKey", "pitch(internal)", "endOper", "pad",
    };
    return (g >= 0 && g < FS_GEN_COUNT) ? names[g] : "?";
}

// Generators already represented by dedicated region fields (or with no
// meaning in a compiled region), so not worth printing in the "gens" list.
// Returns 1 to skip.
static int gen_skip(int g) {
    switch (g) {
        case FS_GEN_START_ADDRS_OFFSET: case FS_GEN_END_ADDRS_OFFSET:
        case FS_GEN_STARTLOOP_ADDRS_OFFSET: case FS_GEN_ENDLOOP_ADDRS_OFFSET:
        case FS_GEN_START_ADDRS_COARSE_OFFSET: case FS_GEN_END_ADDRS_COARSE_OFFSET:
        case FS_GEN_STARTLOOP_ADDRS_COARSE_OFFSET: case FS_GEN_ENDLOOP_ADDRS_COARSE_OFFSET:
        case FS_GEN_KEY_RANGE: case FS_GEN_VEL_RANGE: case FS_GEN_SAMPLE_ID:
        case FS_GEN_SAMPLE_MODES: case FS_GEN_EXCLUSIVE_CLASS: case FS_GEN_OVERRIDING_ROOT_KEY:
        case FS_GEN_INSTRUMENT: case FS_GEN_END_OPER: case FS_GEN_COUNT - 1:
            return 1;
        default:
            return 0;
    }
}

// Load and validate the font, then list either all presets (one argument)
// or the regions of one bank:program (three arguments). Generators equal
// to their SF2 default are not printed. Returns 0 on success, 1 on a file,
// validation or lookup error, 2 on bad usage.
int main(int argc, char **argv) {
    if (argc != 2 && argc != 4) {
        fprintf(stderr, "usage: fsfinfo font.fsf [bank program]\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    rewind(f);
    void *blob = malloc((size_t)size);
    if (!blob || fread(blob, 1, (size_t)size, f) != (size_t)size) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    fclose(f);

    FsFont font;
    const FsFontResult res = fs_font_init(&font, blob, (size_t)size, true, true);
    if (res != FS_FONT_OK) { fprintf(stderr, "%s: %s\n", argv[1], fs_font_result_str(res)); return 1; }
    const FsFontHeader *h = font.header;

    if (argc == 2) {
        printf("%s: '%s' %u presets, %u regions, %u modulators, %u frames, %u bytes, CRCs ok\n",
               argv[1], h->name, h->preset_count, h->region_count, h->mod_count, h->data_frames, h->total_size);
        for (uint32_t i = 0; i < h->preset_count; ++i) {
            const FsPreset *p = &font.presets[i];
            printf("  %3u:%3u  %-20.20s  %u regions\n", p->bank, p->program, p->name, p->region_count);
        }
        return 0;
    }

    const FsPreset *p = fs_font_find_preset(&font, (uint16_t)atoi(argv[2]), (uint16_t)atoi(argv[3]));
    if (!p) { fprintf(stderr, "preset %s:%s not found\n", argv[2], argv[3]); return 1; }
    printf("%u:%u '%.20s' %u regions\n", p->bank, p->program, p->name, p->region_count);
    for (uint32_t i = 0; i < p->region_count; ++i) {
        const FsRegion *r = fs_font_preset_region(&font, p, i);
        printf("  region %u: keys %u-%u vel %u-%u root %u corr %d rate %u\n",
               i, r->key_lo, r->key_hi, r->vel_lo, r->vel_hi, r->root_key, r->pitch_correction, r->sample_rate);
        printf("    frames %u..%u loop %u..%u mode %u excl %u\n",
               r->start, r->end, r->loop_start, r->loop_end, r->loop_mode, r->exclusive_class);
        printf("    gens:");
        for (int g = 0; g < FS_GEN_COUNT; ++g) {
            if (gen_skip(g) || r->gen[g] == fs_gen_default[g]) continue;
            printf(" %s=%d", gen_name(g), r->gen[g]);
        }
        printf("\n    mods:");
        const FsModulator *m = fs_font_region_mods(&font, r);
        for (uint32_t j = 0; j < r->mod_count; ++j) {
            printf(" [%04x->%s %d amt %04x t%u]", m[j].src, gen_name(m[j].dst), m[j].amount, m[j].amt_src, m[j].transform);
        }
        printf("\n");
    }
    return 0;
}
