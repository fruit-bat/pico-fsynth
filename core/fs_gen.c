#include "fs_gen.h"

const int16_t fs_gen_default[FS_GEN_COUNT] = {
    [FS_GEN_INITIAL_FILTER_FC] = 13500,
    [FS_GEN_DELAY_MOD_LFO] = -12000,
    [FS_GEN_DELAY_VIB_LFO] = -12000,
    [FS_GEN_DELAY_MOD_ENV] = -12000,
    [FS_GEN_ATTACK_MOD_ENV] = -12000,
    [FS_GEN_HOLD_MOD_ENV] = -12000,
    [FS_GEN_DECAY_MOD_ENV] = -12000,
    [FS_GEN_RELEASE_MOD_ENV] = -12000,
    [FS_GEN_DELAY_VOL_ENV] = -12000,
    [FS_GEN_ATTACK_VOL_ENV] = -12000,
    [FS_GEN_HOLD_VOL_ENV] = -12000,
    [FS_GEN_DECAY_VOL_ENV] = -12000,
    [FS_GEN_RELEASE_VOL_ENV] = -12000,
    [FS_GEN_KEY_RANGE] = 0x7f00,  // lo = 0, hi = 127
    [FS_GEN_VEL_RANGE] = 0x7f00,  // lo = 0, hi = 127
    [FS_GEN_KEYNUM] = -1,
    [FS_GEN_VELOCITY] = -1,
    [FS_GEN_SCALE_TUNING] = 100,
    [FS_GEN_OVERRIDING_ROOT_KEY] = -1,
};

#define SRC(index, cc, neg, bip, type) \
    ((index) | ((cc) ? FS_MOD_SRC_CC : 0) | ((neg) ? FS_MOD_SRC_NEGATIVE : 0) | \
     ((bip) ? FS_MOD_SRC_BIPOLAR : 0) | ((type) << FS_MOD_SRC_TYPE_SHIFT))

// SF2 2.04 section 8.4 default modulators
const FsModulator fs_default_modulators[FS_DEFAULT_MODULATOR_COUNT] = {
    // 8.4.1 MIDI note-on velocity to initial attenuation
    { SRC(FS_MOD_GC_VELOCITY, 0, 1, 0, FS_MOD_TYPE_CONCAVE), FS_GEN_INITIAL_ATTENUATION, 960, 0, 0, 0 },
    // 8.4.2 MIDI note-on velocity to filter cutoff.
    // The amount source is 0x0D02 (velocity, negative, switch) as in the SF2
    // 2.01 text: velocities below 64 lower the cutoff. Editors disable this
    // default by writing an identical modulator with amount 0 and that amount
    // source (TimGM6mb does so in 148 zones).
    // FluidSynth uses a positive switch instead (velocities of 64 and above
    // lower the cutoff). sf2conv --vel-filter selects spec, fluid or off and
    // defaults to fluid.
    { SRC(FS_MOD_GC_VELOCITY, 0, 1, 0, FS_MOD_TYPE_LINEAR), FS_GEN_INITIAL_FILTER_FC, -2400,
      SRC(FS_MOD_GC_VELOCITY, 0, 1, 0, FS_MOD_TYPE_SWITCH), 0, 0 },
    // 8.4.3 MIDI channel pressure to vibrato LFO pitch depth
    { SRC(FS_MOD_GC_CHAN_PRESSURE, 0, 0, 0, FS_MOD_TYPE_LINEAR), FS_GEN_VIB_LFO_TO_PITCH, 50, 0, 0, 0 },
    // 8.4.4 MIDI CC1 (mod wheel) to vibrato LFO pitch depth
    { SRC(1, 1, 0, 0, FS_MOD_TYPE_LINEAR), FS_GEN_VIB_LFO_TO_PITCH, 50, 0, 0, 0 },
    // 8.4.5 MIDI CC7 (volume) to initial attenuation
    { SRC(7, 1, 1, 0, FS_MOD_TYPE_CONCAVE), FS_GEN_INITIAL_ATTENUATION, 960, 0, 0, 0 },
    // 8.4.6 MIDI CC10 (pan) to pan
    { SRC(10, 1, 0, 1, FS_MOD_TYPE_LINEAR), FS_GEN_PAN, 1000, 0, 0, 0 },
    // 8.4.7 MIDI CC11 (expression) to initial attenuation
    { SRC(11, 1, 1, 0, FS_MOD_TYPE_CONCAVE), FS_GEN_INITIAL_ATTENUATION, 960, 0, 0, 0 },
    // 8.4.8 MIDI CC91 to reverb send
    { SRC(91, 1, 0, 0, FS_MOD_TYPE_LINEAR), FS_GEN_REVERB_EFFECTS_SEND, 200, 0, 0, 0 },
    // 8.4.9 MIDI CC93 to chorus send
    { SRC(93, 1, 0, 0, FS_MOD_TYPE_LINEAR), FS_GEN_CHORUS_EFFECTS_SEND, 200, 0, 0, 0 },
    // 8.4.10 MIDI pitch wheel to initial pitch, scaled by pitch wheel sensitivity
    { SRC(FS_MOD_GC_PITCH_WHEEL, 0, 0, 1, FS_MOD_TYPE_LINEAR), FS_GEN_PITCH, 12700,
      SRC(FS_MOD_GC_PITCH_WHEEL_SENSITIVITY, 0, 0, 0, FS_MOD_TYPE_LINEAR), 0, 0 },
};
