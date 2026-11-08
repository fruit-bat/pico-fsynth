//
// sf2conv - compile a SoundFont 2 (.sf2) file into a flat .fsf font image
// that pico-fsynth can read in place from flash.
//
// What it does:
//  - Parses the RIFF structure (bounds checked; the input is untrusted).
//  - Flattens presets -> preset zones -> instruments -> instrument zones
//    into a list of regions per preset. Global zones and defaults are
//    applied, preset generators are added to instrument generators and
//    key/velocity ranges are intersected (SF2 2.04 section 9.4).
//  - Builds each region's modulator list: default modulators, overridden
//    by instrument global then local zone modulators, followed by preset
//    modulators (which are additive). Identical lists are shared.
//  - Resolves sample address offset generators into absolute sample
//    positions and validates/repairs loop points.
//  - Copies only the sample data that is referenced.
//
// Usage: sf2conv [-v] input.sf2 output.fsf
//
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>

#include "fs_font.h"
#include "fs_crc32.h"

_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "sf2conv assumes a little-endian host");

static bool verbose = false;
static unsigned warning_count = 0;

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "sf2conv: error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

static void warn(const char *fmt, ...) {
    ++warning_count;
    if (!verbose) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "sf2conv: warning: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static void *xcalloc(size_t n, size_t size) {
    void *p = calloc(n ? n : 1, size);
    if (!p) die("out of memory");
    return p;
}

static void *xrealloc(void *p, size_t size) {
    p = realloc(p, size ? size : 1);
    if (!p) die("out of memory");
    return p;
}

static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static int16_t rds16(const uint8_t *p) { int16_t v; memcpy(&v, p, 2); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

// ---------------------------------------------------------------------------
// RIFF parsing
// ---------------------------------------------------------------------------

typedef struct {
    const uint8_t *data;
    uint32_t size;
} Chunk;

typedef struct {
    Chunk inam;
    Chunk smpl;
    Chunk phdr, pbag, pmod, pgen;
    Chunk inst, ibag, imod, igen;
    Chunk shdr;
} Sf2Chunks;

static bool id_is(const uint8_t *p, const char *id) {
    return memcmp(p, id, 4) == 0;
}

static void parse_list(const uint8_t *p, uint32_t size, Sf2Chunks *c) {
    uint32_t o = 0;
    while (o + 8 <= size) {
        const uint8_t *id = p + o;
        const uint32_t sz = rd32(p + o + 4);
        if (sz > size - o - 8) die("chunk %.4s overruns its list", (const char *)id);
        const Chunk ch = { p + o + 8, sz };
        if (id_is(id, "INAM")) c->inam = ch;
        else if (id_is(id, "smpl")) c->smpl = ch;
        else if (id_is(id, "phdr")) c->phdr = ch;
        else if (id_is(id, "pbag")) c->pbag = ch;
        else if (id_is(id, "pmod")) c->pmod = ch;
        else if (id_is(id, "pgen")) c->pgen = ch;
        else if (id_is(id, "inst")) c->inst = ch;
        else if (id_is(id, "ibag")) c->ibag = ch;
        else if (id_is(id, "imod")) c->imod = ch;
        else if (id_is(id, "igen")) c->igen = ch;
        else if (id_is(id, "shdr")) c->shdr = ch;
        o += 8 + sz + (sz & 1);
    }
}

static void parse_riff(const uint8_t *file, size_t size, Sf2Chunks *c) {
    memset(c, 0, sizeof(*c));
    if (size < 12 || !id_is(file, "RIFF") || !id_is(file + 8, "sfbk")) die("not a SoundFont 2 file");
    uint32_t riff_size = rd32(file + 4);
    if (riff_size > size - 8) {
        warn("RIFF size %u larger than file, truncating", riff_size);
        riff_size = (uint32_t)(size - 8);
    }
    const uint8_t *p = file + 12;
    const uint32_t end = riff_size - 4;
    uint32_t o = 0;
    while (o + 8 <= end) {
        const uint32_t sz = rd32(p + o + 4);
        if (sz > end - o - 8) die("top level chunk overruns file");
        if (id_is(p + o, "LIST") && sz >= 4) {
            parse_list(p + o + 12, sz - 4, c);
        }
        o += 8 + sz + (sz & 1);
    }
}

// Record counts, checked against the record sizes from the spec.
static uint32_t record_count(const Chunk *c, uint32_t rec, const char *name, uint32_t min) {
    if (c->size % rec) die("%s chunk size %u is not a multiple of %u", name, c->size, rec);
    const uint32_t n = c->size / rec;
    if (n < min) die("%s chunk has %u records, need at least %u", name, n, min);
    return n;
}

// ---------------------------------------------------------------------------
// SoundFont model
// ---------------------------------------------------------------------------

typedef struct {
    char name[21];
    uint32_t start, end, loop_start, loop_end, rate;
    uint8_t original_pitch;
    int8_t correction;
    uint16_t type;
} Sf2Sample;

typedef struct {
    int32_t gen[FS_GEN_COUNT];
    bool set[FS_GEN_COUNT];
    FsModulator *mods;
    uint32_t mod_count;
} Zone;

typedef struct {
    const uint8_t *bag;  // bag records
    uint32_t bag_count;
    const uint8_t *gen;  // gen records
    uint32_t gen_count;
    const uint8_t *mod;  // mod records
    uint32_t mod_count;
    uint16_t terminal;   // FS_GEN_INSTRUMENT or FS_GEN_SAMPLE_ID
} ZoneSource;

static bool mod_identical(const FsModulator *a, const FsModulator *b) {
    return a->src == b->src && a->dst == b->dst && a->amt_src == b->amt_src;
}

// Add or replace (if identical) a modulator in a list.
static void mod_list_apply(FsModulator **list, uint32_t *count, const FsModulator *m) {
    for (uint32_t i = 0; i < *count; ++i) {
        if (mod_identical(&(*list)[i], m)) {
            (*list)[i] = *m;
            return;
        }
    }
    *list = xrealloc(*list, sizeof(FsModulator) * (*count + 1));
    (*list)[(*count)++] = *m;
}

// Read the generators and modulators of one bag into a zone. Values
// overwrite whatever is already in the zone (global zone, defaults).
// Returns true if the zone ends with the terminal generator.
static bool read_zone(const ZoneSource *zs, uint32_t bag, Zone *z, const char *what) {
    const uint32_t g0 = rd16(zs->bag + bag * 4), g1 = rd16(zs->bag + (bag + 1) * 4);
    const uint32_t m0 = rd16(zs->bag + bag * 4 + 2), m1 = rd16(zs->bag + (bag + 1) * 4 + 2);
    if (g0 > g1 || g1 > zs->gen_count) die("%s bag %u: bad generator index", what, bag);
    if (m0 > m1 || m1 > zs->mod_count) die("%s bag %u: bad modulator index", what, bag);

    bool terminal = false;
    for (uint32_t g = g0; g < g1 && !terminal; ++g) {
        const uint16_t oper = rd16(zs->gen + g * 4);
        const uint8_t *amt = zs->gen + g * 4 + 2;
        if (oper >= FS_GEN_END_OPER || oper == FS_GEN_PITCH) {
            continue; // Unknown or unused generators are ignored
        }
        if (oper == FS_GEN_KEY_RANGE || oper == FS_GEN_VEL_RANGE) {
            z->gen[oper] = amt[0] | (amt[1] << 8); // lo, hi bytes
        } else if (oper == FS_GEN_INSTRUMENT || oper == FS_GEN_SAMPLE_ID) {
            z->gen[oper] = rd16(amt);
        } else {
            z->gen[oper] = rds16(amt);
        }
        z->set[oper] = true;
        if (oper == zs->terminal) terminal = true;
    }

    for (uint32_t m = m0; m < m1; ++m) {
        const uint8_t *r = zs->mod + m * 10;
        FsModulator mod = { rd16(r), rd16(r + 2), rds16(r + 4), rd16(r + 6), rd16(r + 8), 0 };
        if ((mod.src & FS_MOD_SRC_INDEX_MASK) == FS_MOD_GC_LINK && !(mod.src & FS_MOD_SRC_CC)) {
            warn("%s bag %u: linked modulators are not supported, ignored", what, bag);
            continue;
        }
        if (mod.dst & 0x8000) {
            warn("%s bag %u: modulator linked to another modulator, ignored", what, bag);
            continue;
        }
        if (mod.dst >= FS_GEN_END_OPER) {
            warn("%s bag %u: modulator destination %u invalid, ignored", what, bag, mod.dst);
            continue;
        }
        mod_list_apply(&z->mods, &z->mod_count, &mod);
    }
    return terminal;
}

static void zone_copy(Zone *dst, const Zone *src) {
    memcpy(dst->gen, src->gen, sizeof(dst->gen));
    memcpy(dst->set, src->set, sizeof(dst->set));
    dst->mods = NULL;
    dst->mod_count = 0;
    for (uint32_t i = 0; i < src->mod_count; ++i) mod_list_apply(&dst->mods, &dst->mod_count, &src->mods[i]);
}

static void zone_free(Zone *z) {
    free(z->mods);
    z->mods = NULL;
    z->mod_count = 0;
}

// Resolved list of zones for an instrument or preset
typedef struct {
    Zone *zones;
    uint32_t count;
} ZoneList;

// Build the local zones of an instrument or preset with global zone values
// (and the supplied base values) already applied.
static ZoneList build_zones(const ZoneSource *zs, uint32_t bag_first, uint32_t bag_last,
                            const Zone *base, const char *what) {
    ZoneList list = { NULL, 0 };
    Zone global;
    zone_copy(&global, base);

    for (uint32_t b = bag_first; b < bag_last; ++b) {
        Zone z;
        zone_copy(&z, &global);
        // Read into a scratch zone first to find out whether this is a global zone
        Zone probe = { 0 };
        const bool terminal = read_zone(zs, b, &probe, what);
        if (!terminal) {
            if (b == bag_first) {
                // Global zone: apply on top of the base
                zone_free(&z);
                zone_free(&global);
                zone_copy(&global, base);
                for (int g = 0; g < FS_GEN_COUNT; ++g) {
                    if (probe.set[g]) { global.gen[g] = probe.gen[g]; global.set[g] = true; }
                }
                for (uint32_t i = 0; i < probe.mod_count; ++i) mod_list_apply(&global.mods, &global.mod_count, &probe.mods[i]);
            } else {
                warn("%s bag %u: zone without %s ignored", what, b,
                     zs->terminal == FS_GEN_SAMPLE_ID ? "sampleID" : "instrument");
                zone_free(&z);
            }
            zone_free(&probe);
            continue;
        }
        for (int g = 0; g < FS_GEN_COUNT; ++g) {
            if (probe.set[g]) { z.gen[g] = probe.gen[g]; z.set[g] = true; }
        }
        for (uint32_t i = 0; i < probe.mod_count; ++i) mod_list_apply(&z.mods, &z.mod_count, &probe.mods[i]);
        zone_free(&probe);

        list.zones = xrealloc(list.zones, sizeof(Zone) * (list.count + 1));
        list.zones[list.count++] = z;
    }
    zone_free(&global);
    return list;
}

static void zone_list_free(ZoneList *l) {
    for (uint32_t i = 0; i < l->count; ++i) zone_free(&l->zones[i]);
    free(l->zones);
    l->zones = NULL;
    l->count = 0;
}

// Generators that are only meaningful at instrument level
static bool gen_instrument_only(int g) {
    switch (g) {
        case FS_GEN_START_ADDRS_OFFSET:
        case FS_GEN_END_ADDRS_OFFSET:
        case FS_GEN_STARTLOOP_ADDRS_OFFSET:
        case FS_GEN_ENDLOOP_ADDRS_OFFSET:
        case FS_GEN_START_ADDRS_COARSE_OFFSET:
        case FS_GEN_END_ADDRS_COARSE_OFFSET:
        case FS_GEN_STARTLOOP_ADDRS_COARSE_OFFSET:
        case FS_GEN_ENDLOOP_ADDRS_COARSE_OFFSET:
        case FS_GEN_KEYNUM:
        case FS_GEN_VELOCITY:
        case FS_GEN_SAMPLE_MODES:
        case FS_GEN_EXCLUSIVE_CLASS:
        case FS_GEN_OVERRIDING_ROOT_KEY:
        case FS_GEN_SAMPLE_ID:
        case FS_GEN_INSTRUMENT:
        case FS_GEN_KEY_RANGE:
        case FS_GEN_VEL_RANGE:
            return true;
        default:
            return false;
    }
}

static int16_t sat16(int32_t v) {
    return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
}

// ---------------------------------------------------------------------------
// Output building
// ---------------------------------------------------------------------------

typedef struct {
    FsRegion region;
    uint32_t sample_index;   // Index into the SF2 sample headers
} PendingRegion;

typedef struct {
    FsPreset preset;
} PendingPreset;

static FsModulator *out_mods = NULL;
static uint32_t out_mod_count = 0;

// Find an identical modulator list in the output table or append it.
static uint16_t intern_mod_list(const FsModulator *mods, uint32_t count) {
    if (count == 0) return 0;
    for (uint32_t i = 0; i + count <= out_mod_count; ++i) {
        if (memcmp(&out_mods[i], mods, sizeof(FsModulator) * count) == 0) return (uint16_t)i;
    }
    if (out_mod_count + count > 65535) die("too many distinct modulators");
    out_mods = xrealloc(out_mods, sizeof(FsModulator) * (out_mod_count + count));
    memcpy(&out_mods[out_mod_count], mods, sizeof(FsModulator) * count);
    const uint16_t first = (uint16_t)out_mod_count;
    out_mod_count += count;
    return first;
}

static int compare_presets(const void *a, const void *b) {
    const FsPreset *pa = &((const PendingPreset *)a)->preset;
    const FsPreset *pb = &((const PendingPreset *)b)->preset;
    const uint32_t ka = (uint32_t)pa->bank << 16 | pa->program;
    const uint32_t kb = (uint32_t)pb->bank << 16 | pb->program;
    return ka < kb ? -1 : ka > kb ? 1 : 0;
}

static void copy_name(char *dst, const uint8_t *src, size_t n) {
    memcpy(dst, src, n);
    dst[n] = 0;
    // Strip trailing spaces and non-printables
    for (size_t i = 0; i < n; ++i) if (dst[i] && (dst[i] < 32 || dst[i] > 126)) dst[i] = '?';
}

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open %s", path);
    if (fseek(f, 0, SEEK_END) != 0) die("cannot seek %s", path);
    const long sz = ftell(f);
    if (sz < 0) die("cannot size %s", path);
    rewind(f);
    uint8_t *buf = xcalloc((size_t)sz, 1);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) die("cannot read %s", path);
    fclose(f);
    *size = (size_t)sz;
    return buf;
}

static void usage(void) {
    fprintf(stderr,
        "usage: sf2conv [-v] [--vel-filter=spec|fluid|off] input.sf2 output.fsf\n"
        "  -v            list warnings about the source font\n"
        "  --vel-filter  default 'velocity to filter cutoff' modulator:\n"
        "                fluid as FluidSynth: velocities of 64 and above lower the cutoff (default)\n"
        "                spec  as SF2 2.01: velocities below 64 lower the cutoff\n"
        "                off   no default velocity to filter modulator\n");
    exit(2);
}

int main(int argc, char **argv) {
    // Default modulators, adjustable from the command line
    FsModulator default_mods[FS_DEFAULT_MODULATOR_COUNT];
    memcpy(default_mods, fs_default_modulators, sizeof(default_mods));
    FsModulator *vel_filter = NULL;
    for (int i = 0; i < FS_DEFAULT_MODULATOR_COUNT; ++i) {
        if (default_mods[i].dst == FS_GEN_INITIAL_FILTER_FC) vel_filter = &default_mods[i];
    }
    const char *vel_filter_mode = "fluid";

    int argi = 1;
    while (argi < argc && argv[argi][0] == '-') {
        if (strcmp(argv[argi], "-v") == 0) verbose = true;
        else if (strncmp(argv[argi], "--vel-filter=", 13) == 0) vel_filter_mode = argv[argi] + 13;
        else usage();
        ++argi;
    }
    if (argc - argi != 2) usage();
    if (strcmp(vel_filter_mode, "spec") == 0) {
        // As defined in fs_default_modulators
    } else if (strcmp(vel_filter_mode, "fluid") == 0) {
        vel_filter->amt_src &= (uint16_t)~FS_MOD_SRC_NEGATIVE; // Positive switch
    } else if (strcmp(vel_filter_mode, "off") == 0) {
        vel_filter->amount = 0; // Zero amount modulators are dropped
    } else {
        usage();
    }
    const char *in_path = argv[argi];
    const char *out_path = argv[argi + 1];

    size_t file_size;
    uint8_t *file = read_file(in_path, &file_size);

    Sf2Chunks c;
    parse_riff(file, file_size, &c);
    if (!c.smpl.data) die("no smpl chunk");
    if (!c.phdr.data || !c.pbag.data || !c.pgen.data || !c.inst.data ||
        !c.ibag.data || !c.igen.data || !c.shdr.data) die("missing pdta sub-chunk");

    const uint32_t n_phdr = record_count(&c.phdr, 38, "phdr", 2);
    const uint32_t n_pbag = record_count(&c.pbag, 4, "pbag", 1);
    const uint32_t n_pmod = c.pmod.data ? record_count(&c.pmod, 10, "pmod", 0) : 0;
    const uint32_t n_pgen = record_count(&c.pgen, 4, "pgen", 1);
    const uint32_t n_inst = record_count(&c.inst, 22, "inst", 2);
    const uint32_t n_ibag = record_count(&c.ibag, 4, "ibag", 1);
    const uint32_t n_imod = c.imod.data ? record_count(&c.imod, 10, "imod", 0) : 0;
    const uint32_t n_igen = record_count(&c.igen, 4, "igen", 1);
    const uint32_t n_shdr = record_count(&c.shdr, 46, "shdr", 1);
    const uint32_t smpl_frames = c.smpl.size / 2;

    // Sample headers (last one is the terminal record)
    const uint32_t sample_count = n_shdr - 1;
    Sf2Sample *samples = xcalloc(sample_count, sizeof(Sf2Sample));
    for (uint32_t i = 0; i < sample_count; ++i) {
        const uint8_t *r = c.shdr.data + i * 46;
        Sf2Sample *s = &samples[i];
        copy_name(s->name, r, 20);
        s->start = rd32(r + 20);
        s->end = rd32(r + 24);
        s->loop_start = rd32(r + 28);
        s->loop_end = rd32(r + 32);
        s->rate = rd32(r + 36);
        s->original_pitch = r[40];
        s->correction = (int8_t)r[41];
        s->type = rd16(r + 44);
    }

    // Zone sources. The terminal bag record of each list gives the end index.
    const ZoneSource inst_src = { c.ibag.data, n_ibag - 1, c.igen.data, n_igen, c.imod.data, n_imod, FS_GEN_SAMPLE_ID };
    const ZoneSource preset_src = { c.pbag.data, n_pbag - 1, c.pgen.data, n_pgen, c.pmod.data, n_pmod, FS_GEN_INSTRUMENT };

    // Instrument level base: generator defaults plus default modulators
    Zone inst_base = { 0 };
    for (int g = 0; g < FS_GEN_COUNT; ++g) inst_base.gen[g] = fs_gen_default[g];
    for (int i = 0; i < FS_DEFAULT_MODULATOR_COUNT; ++i) mod_list_apply(&inst_base.mods, &inst_base.mod_count, &default_mods[i]);

    // Preset level base: zero offsets, full ranges, no modulators
    Zone preset_base = { 0 };
    preset_base.gen[FS_GEN_KEY_RANGE] = 0x7f00;
    preset_base.gen[FS_GEN_VEL_RANGE] = 0x7f00;

    // Resolve all instruments up front
    const uint32_t inst_count = n_inst - 1;
    ZoneList *instruments = xcalloc(inst_count, sizeof(ZoneList));
    for (uint32_t i = 0; i < inst_count; ++i) {
        const uint32_t b0 = rd16(c.inst.data + i * 22 + 20);
        const uint32_t b1 = rd16(c.inst.data + (i + 1) * 22 + 20);
        if (b0 > b1 || b1 > n_ibag - 1) die("instrument %u: bad bag index", i);
        char what[48];
        snprintf(what, sizeof(what), "instrument %u", i);
        instruments[i] = build_zones(&inst_src, b0, b1, &inst_base, what);
    }

    PendingPreset *presets = NULL;
    uint32_t preset_count = 0;
    PendingRegion *regions = NULL;
    uint32_t region_count = 0;
    bool *sample_used = xcalloc(sample_count, sizeof(bool));
    uint32_t loops_repaired = 0, loops_disabled = 0;

    for (uint32_t p = 0; p < n_phdr - 1; ++p) {
        const uint8_t *r = c.phdr.data + p * 38;
        const uint32_t b0 = rd16(r + 24);
        const uint32_t b1 = rd16(r + 38 + 24);
        if (b0 > b1 || b1 > n_pbag - 1) die("preset %u: bad bag index", p);

        PendingPreset pp;
        memset(&pp, 0, sizeof(pp));
        char name[21];
        copy_name(name, r, 20);
        memcpy(pp.preset.name, name, 20);
        pp.preset.program = rd16(r + 20);
        pp.preset.bank = rd16(r + 22);
        pp.preset.region_first = region_count;

        if (pp.preset.program > 127 || pp.preset.bank > 128) {
            warn("preset '%s' has bank %u program %u, skipped", name, pp.preset.bank, pp.preset.program);
            continue;
        }

        char what[48];
        snprintf(what, sizeof(what), "preset %u", p);
        ZoneList pzones = build_zones(&preset_src, b0, b1, &preset_base, what);

        for (uint32_t pz = 0; pz < pzones.count; ++pz) {
            const Zone *pzone = &pzones.zones[pz];
            const uint32_t inst_index = (uint32_t)pzone->gen[FS_GEN_INSTRUMENT];
            if (inst_index >= inst_count) {
                warn("preset '%s': instrument index %u out of range", name, inst_index);
                continue;
            }
            const ZoneList *izones = &instruments[inst_index];
            for (uint32_t iz = 0; iz < izones->count; ++iz) {
                const Zone *izone = &izones->zones[iz];

                // Intersect key and velocity ranges
                const int pkl = pzone->gen[FS_GEN_KEY_RANGE] & 0xff, pkh = (pzone->gen[FS_GEN_KEY_RANGE] >> 8) & 0xff;
                const int pvl = pzone->gen[FS_GEN_VEL_RANGE] & 0xff, pvh = (pzone->gen[FS_GEN_VEL_RANGE] >> 8) & 0xff;
                const int ikl = izone->gen[FS_GEN_KEY_RANGE] & 0xff, ikh = (izone->gen[FS_GEN_KEY_RANGE] >> 8) & 0xff;
                const int ivl = izone->gen[FS_GEN_VEL_RANGE] & 0xff, ivh = (izone->gen[FS_GEN_VEL_RANGE] >> 8) & 0xff;
                int kl = pkl > ikl ? pkl : ikl, kh = pkh < ikh ? pkh : ikh;
                int vl = pvl > ivl ? pvl : ivl, vh = pvh < ivh ? pvh : ivh;
                if (kh > 127) kh = 127;
                if (vh > 127) vh = 127;
                if (kl > kh || vl > vh) continue;

                const uint32_t si = (uint32_t)izone->gen[FS_GEN_SAMPLE_ID];
                if (si >= sample_count) {
                    warn("preset '%s': sample index %u out of range", name, si);
                    continue;
                }
                const Sf2Sample *s = &samples[si];
                if (s->type & 0x8000) {
                    warn("preset '%s': ROM sample '%s' not supported", name, s->name);
                    continue;
                }
                if (!(s->start < s->end && s->end <= smpl_frames) || s->rate == 0) {
                    warn("preset '%s': sample '%s' has invalid bounds/rate", name, s->name);
                    continue;
                }

                PendingRegion pr;
                memset(&pr, 0, sizeof(pr));
                FsRegion *reg = &pr.region;
                pr.sample_index = si;

                for (int g = 0; g < FS_GEN_COUNT; ++g) {
                    int32_t v = izone->gen[g];
                    if (!gen_instrument_only(g)) v += pzone->gen[g];
                    reg->gen[g] = sat16(v);
                }
                reg->key_lo = (uint8_t)kl; reg->key_hi = (uint8_t)kh;
                reg->vel_lo = (uint8_t)vl; reg->vel_hi = (uint8_t)vh;
                reg->gen[FS_GEN_KEY_RANGE] = (int16_t)(kl | kh << 8);
                reg->gen[FS_GEN_VEL_RANGE] = (int16_t)(vl | vh << 8);

                // Sample positions in the original smpl chunk (clamped to the sample)
                const int32_t *ig = izone->gen;
                int64_t start = (int64_t)s->start + ig[FS_GEN_START_ADDRS_OFFSET] + 32768LL * ig[FS_GEN_START_ADDRS_COARSE_OFFSET];
                int64_t end = (int64_t)s->end + ig[FS_GEN_END_ADDRS_OFFSET] + 32768LL * ig[FS_GEN_END_ADDRS_COARSE_OFFSET];
                int64_t ls = (int64_t)s->loop_start + ig[FS_GEN_STARTLOOP_ADDRS_OFFSET] + 32768LL * ig[FS_GEN_STARTLOOP_ADDRS_COARSE_OFFSET];
                int64_t le = (int64_t)s->loop_end + ig[FS_GEN_ENDLOOP_ADDRS_OFFSET] + 32768LL * ig[FS_GEN_ENDLOOP_ADDRS_COARSE_OFFSET];
                if (start < s->start) start = s->start;
                if (end > s->end) end = s->end;
                if (start >= end) {
                    warn("preset '%s': sample '%s' offsets leave no data", name, s->name);
                    continue;
                }

                int loop_mode = ig[FS_GEN_SAMPLE_MODES] & 3;
                if (loop_mode == FS_LOOP_UNUSED) loop_mode = FS_LOOP_NONE;
                if (loop_mode != FS_LOOP_NONE) {
                    int64_t ls2 = ls < start ? start : ls > end ? end : ls;
                    int64_t le2 = le < start ? start : le > end ? end : le;
                    if (ls2 != ls || le2 != le) {
                        ++loops_repaired;
                        warn("preset '%s': sample '%s' loop clamped to sample", name, s->name);
                    }
                    if (le2 - ls2 < 2) {
                        ++loops_disabled;
                        warn("preset '%s': sample '%s' loop too short, disabled", name, s->name);
                        loop_mode = FS_LOOP_NONE;
                        ls2 = start; le2 = end;
                    }
                    ls = ls2; le = le2;
                } else {
                    ls = start; le = end;
                }

                // Stored relative to the sample start for now; relocated below
                reg->start = (uint32_t)(start - s->start);
                reg->end = (uint32_t)(end - s->start);
                reg->loop_start = (uint32_t)(ls - s->start);
                reg->loop_end = (uint32_t)(le - s->start);
                reg->sample_rate = s->rate;
                reg->loop_mode = (uint8_t)loop_mode;
                reg->exclusive_class = (uint8_t)(ig[FS_GEN_EXCLUSIVE_CLASS] & 0x7f);

                const int32_t ork = ig[FS_GEN_OVERRIDING_ROOT_KEY];
                if (ork >= 0 && ork <= 127) reg->root_key = (uint8_t)ork;
                else if (s->original_pitch <= 127) reg->root_key = s->original_pitch;
                else reg->root_key = 60; // 255 = unpitched
                reg->pitch_correction = s->correction;

                // Modulators: instrument (with defaults) then preset, dropping zero amounts
                FsModulator *mods = NULL;
                uint32_t mod_count = 0;
                for (uint32_t i = 0; i < izone->mod_count; ++i) {
                    if (izone->mods[i].amount == 0) continue;
                    mods = xrealloc(mods, sizeof(FsModulator) * (mod_count + 1));
                    mods[mod_count++] = izone->mods[i];
                }
                for (uint32_t i = 0; i < pzone->mod_count; ++i) {
                    if (pzone->mods[i].amount == 0) continue;
                    mods = xrealloc(mods, sizeof(FsModulator) * (mod_count + 1));
                    mods[mod_count++] = pzone->mods[i];
                }
                reg->mod_first = intern_mod_list(mods, mod_count);
                reg->mod_count = (uint16_t)mod_count;
                free(mods);

                sample_used[si] = true;
                regions = xrealloc(regions, sizeof(PendingRegion) * (region_count + 1));
                regions[region_count++] = pr;
            }
        }
        zone_list_free(&pzones);

        pp.preset.region_count = region_count - pp.preset.region_first;
        if (pp.preset.region_count == 0) warn("preset '%s' has no playable regions", name);
        presets = xrealloc(presets, sizeof(PendingPreset) * (preset_count + 1));
        presets[preset_count++] = pp;
    }

    // Sort presets by (bank, program) and drop duplicates
    qsort(presets, preset_count, sizeof(PendingPreset), compare_presets);
    uint32_t unique = 0;
    for (uint32_t i = 0; i < preset_count; ++i) {
        if (unique > 0 && compare_presets(&presets[unique - 1], &presets[i]) == 0) {
            warn("duplicate preset bank %u program %u, keeping first", presets[i].preset.bank, presets[i].preset.program);
            continue;
        }
        presets[unique++] = presets[i];
    }
    preset_count = unique;

    // Lay out the referenced sample data. Each sample is padded to an even
    // number of frames so every sample starts 4-byte aligned.
    uint32_t *sample_base = xcalloc(sample_count, sizeof(uint32_t));
    uint32_t data_frames = 0;
    uint32_t used_samples = 0;
    for (uint32_t i = 0; i < sample_count; ++i) {
        if (!sample_used[i]) continue;
        sample_base[i] = data_frames;
        data_frames += (samples[i].end - samples[i].start + 1) & ~1u;
        ++used_samples;
    }

    // Relocate region positions
    for (uint32_t i = 0; i < region_count; ++i) {
        FsRegion *reg = &regions[i].region;
        const uint32_t base = sample_base[regions[i].sample_index];
        reg->start += base;
        reg->end += base;
        reg->loop_start += base;
        reg->loop_end += base;
    }

    // Assemble the output image
    FsFontHeader h;
    memset(&h, 0, sizeof(h));
    h.magic = FS_FONT_MAGIC;
    h.version = FS_FONT_VERSION;
    h.header_size = sizeof(FsFontHeader);
    h.preset_count = preset_count;
    h.preset_offset = sizeof(FsFontHeader);
    h.region_count = region_count;
    h.region_offset = h.preset_offset + preset_count * sizeof(FsPreset);
    h.mod_count = out_mod_count;
    h.mod_offset = h.region_offset + region_count * sizeof(FsRegion);
    const uint32_t tables_end = h.mod_offset + out_mod_count * sizeof(FsModulator);
    h.data_offset = (tables_end + FS_FONT_DATA_ALIGN - 1) & ~(uint32_t)(FS_FONT_DATA_ALIGN - 1);
    h.data_frames = data_frames;
    h.total_size = h.data_offset + data_frames * 2;
    if (c.inam.data) {
        const size_t n = c.inam.size < sizeof(h.name) - 1 ? c.inam.size : sizeof(h.name) - 1;
        memcpy(h.name, c.inam.data, n);
    }

    uint8_t *out = xcalloc(h.total_size, 1);
    for (uint32_t i = 0; i < preset_count; ++i) {
        memcpy(out + h.preset_offset + i * sizeof(FsPreset), &presets[i].preset, sizeof(FsPreset));
    }
    for (uint32_t i = 0; i < region_count; ++i) {
        memcpy(out + h.region_offset + i * sizeof(FsRegion), &regions[i].region, sizeof(FsRegion));
    }
    if (out_mod_count) memcpy(out + h.mod_offset, out_mods, out_mod_count * sizeof(FsModulator));
    for (uint32_t i = 0; i < sample_count; ++i) {
        if (!sample_used[i]) continue;
        memcpy(out + h.data_offset + sample_base[i] * 2,
               c.smpl.data + samples[i].start * 2,
               (samples[i].end - samples[i].start) * 2);
    }
    h.table_crc = fs_crc32(0, out + h.header_size, h.data_offset - h.header_size);
    h.data_crc = fs_crc32(0, out + h.data_offset, (size_t)data_frames * 2);
    memcpy(out, &h, sizeof(h));

    // Check our own output with the same reader the synth uses
    FsFont font;
    const FsFontResult res = fs_font_init(&font, out, h.total_size, true, true);
    if (res != FS_FONT_OK) die("internal error, output failed validation: %s", fs_font_result_str(res));

    FILE *f = fopen(out_path, "wb");
    if (!f) die("cannot create %s", out_path);
    if (fwrite(out, 1, h.total_size, f) != h.total_size) die("cannot write %s", out_path);
    if (fclose(f) != 0) die("cannot write %s", out_path);

    printf("%s -> %s\n", in_path, out_path);
    printf("  name:       %s\n", h.name);
    printf("  vel-filter: %s\n", vel_filter_mode);
    printf("  presets:    %u\n", preset_count);
    printf("  regions:    %u (%u bytes)\n", region_count, region_count * (unsigned)sizeof(FsRegion));
    printf("  modulators: %u (%u bytes)\n", out_mod_count, out_mod_count * (unsigned)sizeof(FsModulator));
    printf("  samples:    %u of %u used, %u frames (%.2f MB)\n", used_samples, sample_count, data_frames, data_frames * 2 / 1e6);
    printf("  loops:      %u clamped, %u disabled\n", loops_repaired, loops_disabled);
    printf("  total:      %u bytes (%.2f MB)\n", h.total_size, h.total_size / 1e6);
    if (warning_count) printf("  warnings:   %u%s\n", warning_count, verbose ? "" : " (use -v to list)");

    for (uint32_t i = 0; i < inst_count; ++i) zone_list_free(&instruments[i]);
    free(instruments);
    zone_free(&inst_base);
    free(presets); free(regions); free(samples); free(sample_used); free(sample_base);
    free(out_mods); free(out); free(file);
    return 0;
}
