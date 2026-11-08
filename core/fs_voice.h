#pragma once
//
// A single playing region: sample stream, envelopes, LFOs, filter and gains.
//
#include <stdint.h>
#include <stdbool.h>
#include "fs_config.h"
#include "fs_font.h"
#include "fs_stream.h"

typedef enum {
    FS_ENV_DELAY = 0,
    FS_ENV_ATTACK,
    FS_ENV_HOLD,
    FS_ENV_DECAY,
    FS_ENV_SUSTAIN,
    FS_ENV_RELEASE,
    FS_ENV_DONE
} FsEnvStage;

// DAHDSR envelope. For the volume envelope "level" is linear amplitude in
// the attack stage and a normalised dB scale (1 = 0 dB, 0 = -96 dB)
// elsewhere, as in SF2 / EMU hardware. The modulation envelope is linear.
typedef struct {
    uint8_t stage;
    uint32_t remaining;     // Samples left in timed stages
    float level;
    float rate;             // Level change per sample in attack/decay/release
    uint32_t delay;         // Stage lengths in samples
    uint32_t attack;
    uint32_t hold;
    uint32_t decay;
    uint32_t release;
    float sustain;          // Sustain level
} FsEnv;

typedef struct {
    uint32_t delay;         // Samples before the LFO starts
    float phase;            // [0, 1)
    float step;             // Phase increment per block
    float value;            // Current output [-1, 1]
} FsLfo;

typedef enum {
    FS_VOICE_FREE = 0,
    FS_VOICE_ON,            // Key held
    FS_VOICE_SUSTAINED,     // Key released while the sustain pedal is down
    FS_VOICE_RELEASED,      // In release
} FsVoiceState;

struct FsChannel;

typedef struct {
    uint8_t state;
    uint8_t channel;
    uint8_t key;                // MIDI key that started the voice
    uint8_t vel;
    uint8_t mkey;               // Key and velocity seen by modulators and
    uint8_t mvel;               // keynum scaling (keynum/velocity generators)
    uint8_t exclusive_class;
    uint8_t loop_mode;
    bool stolen;                // Fading out after being stolen
    uint32_t serial;            // Allocation order (for voice stealing)
    uint32_t chan_serial;       // Channel controller state the params reflect

    const FsRegion *region;
    const FsModulator *mods;

    FsStream stream;
    uint32_t pos;               // Playhead, integer part (virtual frame)
    uint32_t frac;              // Playhead, fractional part (0.32 fixed)

    FsEnv vol_env;
    FsEnv mod_env;
    FsLfo mod_lfo;
    FsLfo vib_lfo;

    // Static (note-on) pitch in cents relative to playing the sample at its
    // root key, plus the sample rate ratio
    float pitch_base;
    float rate_ratio;

    // Modulated parameters (recomputed when channel controllers change)
    float pitch_mod;            // Cents (pitch wheel etc.)
    float vib_lfo_to_pitch;     // Cents at full LFO swing
    float mod_lfo_to_pitch;
    float mod_env_to_pitch;
    float fc;                   // Filter cutoff, absolute cents
    float q;                    // Filter resonance, cB
    float mod_lfo_to_fc;
    float mod_env_to_fc;
    float attenuation;          // cB
    float mod_lfo_to_vol;       // cB
    float pan;                  // -500..500
    float reverb_send;          // 0..1
    float chorus_send;          // 0..1

    // Pan gains derived from pan
    float pan_l;
    float pan_r;

    // Filter state (biquad, transposed direct form II)
    bool filter_on;
    float filter_fc_hz;         // Cutoff the coefficients were computed for
    float filter_q_cb;
    float w_scale;          // 2 pi / sample rate
    float b0, b1, b2, a1, a2;
    float z1, z2;

    // Overall amplitude at the end of the previous block (for ramping)
    float amp;

    // Last output sample per side (for declicking if the voice is cut)
    float last_l;
    float last_r;
} FsVoice;

typedef struct FsChannel {
    uint8_t program;
    uint16_t bank;
    uint8_t cc[128];
    uint16_t pitch_bend;        // 0..16383, 8192 centre
    uint8_t pressure;
    uint8_t pitch_bend_range;   // Semitones (RPN 0)
    uint16_t rpn;               // Selected RPN (0x3fff = none)
    bool drums;
    uint32_t serial;            // Incremented on any controller change
} FsChannel;

// Output buses a voice mixes into
typedef struct {
    float *left;
    float *right;
    float *reverb;
    float *chorus;
    uint32_t *fill_cycles;      // Cycles spent copying samples are added here
    uint32_t *loop_cycles;      // Cycles spent in the sample loop are added here
} FsBuses;

void fs_voice_start(FsVoice *v, const FsFont *font, const FsRegion *region,
                    const FsChannel *ch, uint8_t channel, uint8_t key, uint8_t vel,
                    float sample_rate, uint32_t serial);

void fs_voice_release(FsVoice *v);

// Fade out quickly (exclusive class, voice stealing, all sound off)
void fs_voice_kill(FsVoice *v, float sample_rate);

// Render one block, adding into the buses. Returns false when the voice
// has finished and can be freed.
bool fs_voice_render(FsVoice *v, const FsChannel *ch, const FsBuses *buses, float sample_rate);

// Current amplitude, used to choose a voice to steal
static inline float fs_voice_amp(const FsVoice *v) {
    return v->amp;
}
