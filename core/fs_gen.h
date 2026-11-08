#pragma once
//
// SoundFont 2 generator and modulator definitions shared by the
// host-side converter and the synth core.
//
#include <stdint.h>

typedef enum {
    FS_GEN_START_ADDRS_OFFSET = 0,
    FS_GEN_END_ADDRS_OFFSET = 1,
    FS_GEN_STARTLOOP_ADDRS_OFFSET = 2,
    FS_GEN_ENDLOOP_ADDRS_OFFSET = 3,
    FS_GEN_START_ADDRS_COARSE_OFFSET = 4,
    FS_GEN_MOD_LFO_TO_PITCH = 5,
    FS_GEN_VIB_LFO_TO_PITCH = 6,
    FS_GEN_MOD_ENV_TO_PITCH = 7,
    FS_GEN_INITIAL_FILTER_FC = 8,
    FS_GEN_INITIAL_FILTER_Q = 9,
    FS_GEN_MOD_LFO_TO_FILTER_FC = 10,
    FS_GEN_MOD_ENV_TO_FILTER_FC = 11,
    FS_GEN_END_ADDRS_COARSE_OFFSET = 12,
    FS_GEN_MOD_LFO_TO_VOLUME = 13,
    FS_GEN_UNUSED1 = 14,
    FS_GEN_CHORUS_EFFECTS_SEND = 15,
    FS_GEN_REVERB_EFFECTS_SEND = 16,
    FS_GEN_PAN = 17,
    FS_GEN_UNUSED2 = 18,
    FS_GEN_UNUSED3 = 19,
    FS_GEN_UNUSED4 = 20,
    FS_GEN_DELAY_MOD_LFO = 21,
    FS_GEN_FREQ_MOD_LFO = 22,
    FS_GEN_DELAY_VIB_LFO = 23,
    FS_GEN_FREQ_VIB_LFO = 24,
    FS_GEN_DELAY_MOD_ENV = 25,
    FS_GEN_ATTACK_MOD_ENV = 26,
    FS_GEN_HOLD_MOD_ENV = 27,
    FS_GEN_DECAY_MOD_ENV = 28,
    FS_GEN_SUSTAIN_MOD_ENV = 29,
    FS_GEN_RELEASE_MOD_ENV = 30,
    FS_GEN_KEYNUM_TO_MOD_ENV_HOLD = 31,
    FS_GEN_KEYNUM_TO_MOD_ENV_DECAY = 32,
    FS_GEN_DELAY_VOL_ENV = 33,
    FS_GEN_ATTACK_VOL_ENV = 34,
    FS_GEN_HOLD_VOL_ENV = 35,
    FS_GEN_DECAY_VOL_ENV = 36,
    FS_GEN_SUSTAIN_VOL_ENV = 37,
    FS_GEN_RELEASE_VOL_ENV = 38,
    FS_GEN_KEYNUM_TO_VOL_ENV_HOLD = 39,
    FS_GEN_KEYNUM_TO_VOL_ENV_DECAY = 40,
    FS_GEN_INSTRUMENT = 41,
    FS_GEN_RESERVED1 = 42,
    FS_GEN_KEY_RANGE = 43,
    FS_GEN_VEL_RANGE = 44,
    FS_GEN_STARTLOOP_ADDRS_COARSE_OFFSET = 45,
    FS_GEN_KEYNUM = 46,
    FS_GEN_VELOCITY = 47,
    FS_GEN_INITIAL_ATTENUATION = 48,
    FS_GEN_RESERVED2 = 49,
    FS_GEN_ENDLOOP_ADDRS_COARSE_OFFSET = 50,
    FS_GEN_COARSE_TUNE = 51,
    FS_GEN_FINE_TUNE = 52,
    FS_GEN_SAMPLE_ID = 53,
    FS_GEN_SAMPLE_MODES = 54,
    FS_GEN_RESERVED3 = 55,
    FS_GEN_SCALE_TUNING = 56,
    FS_GEN_EXCLUSIVE_CLASS = 57,
    FS_GEN_OVERRIDING_ROOT_KEY = 58,
    // Generator 59 is "unused5" in the SF2 spec. Like FluidSynth, we use it
    // internally as the destination for the default pitch wheel modulator
    // (pitch offset in cents). It is never read from a SoundFont.
    FS_GEN_PITCH = 59,
    FS_GEN_END_OPER = 60,
    FS_GEN_COUNT = 62 // Rounded up to keep FsRegion 4-byte aligned
} FsGen;

// SF2 sample modes (generator 54)
typedef enum {
    FS_LOOP_NONE = 0,
    FS_LOOP_CONTINUOUS = 1,
    FS_LOOP_UNUSED = 2, // Treated as FS_LOOP_NONE
    FS_LOOP_UNTIL_RELEASE = 3
} FsLoopMode;

// Default values for every generator (SF2 2.04 section 8.1.3)
extern const int16_t fs_gen_default[FS_GEN_COUNT];

// Modulator source encoding (SF2 2.04 section 8.2.1)
#define FS_MOD_SRC_INDEX_MASK   0x007f
#define FS_MOD_SRC_CC           0x0080
#define FS_MOD_SRC_NEGATIVE     0x0100
#define FS_MOD_SRC_BIPOLAR      0x0200
#define FS_MOD_SRC_TYPE_SHIFT   10

// General controller indices (when FS_MOD_SRC_CC is clear)
#define FS_MOD_GC_NONE          0
#define FS_MOD_GC_VELOCITY      2
#define FS_MOD_GC_KEY           3
#define FS_MOD_GC_POLY_PRESSURE 10
#define FS_MOD_GC_CHAN_PRESSURE 13
#define FS_MOD_GC_PITCH_WHEEL   14
#define FS_MOD_GC_PITCH_WHEEL_SENSITIVITY 16
#define FS_MOD_GC_LINK          127

// Modulator source curve types
#define FS_MOD_TYPE_LINEAR  0
#define FS_MOD_TYPE_CONCAVE 1
#define FS_MOD_TYPE_CONVEX  2
#define FS_MOD_TYPE_SWITCH  3

// Modulator transforms
#define FS_MOD_TRANSFORM_LINEAR   0
#define FS_MOD_TRANSFORM_ABSOLUTE 2

// Modulator record as stored in a compiled font (12 bytes)
typedef struct {
    uint16_t src;       // Primary source
    uint16_t dst;       // Destination generator
    int16_t  amount;    // Amount
    uint16_t amt_src;   // Secondary (amount) source
    uint16_t transform; // Transform
    uint16_t pad;
} FsModulator;

#define FS_DEFAULT_MODULATOR_COUNT 10
extern const FsModulator fs_default_modulators[FS_DEFAULT_MODULATOR_COUNT];
