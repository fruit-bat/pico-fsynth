#pragma once
//
// fs_effects.h - reverb, chorus and master EQ.
//
// Reverb and chorus take a mono send bus (the sum of every voice's output
// times its reverb or chorus send, CC91 / CC93) and add their stereo wet
// output into the main left and right buses. The master EQ filters the
// final stereo mix in place.
//
// All processing works on blocks of up to FS_BLOCK_SIZE frames, in float,
// at the same scale as the voice buses (int16 sample units before the
// master gain). State is plain structs with no heap; buffers are sized at
// compile time for FS_MAX_SAMPLE_RATE. fs_synth.c owns one of each and
// calls them once per block (fs_synth_render_part, part 0).
//
#include <stdint.h>
#include <stdbool.h>
#include "fs_config.h"

// Freeverb style reverb: one mono bank of 8 parallel comb filters feeding
// separate left and right chains of 4 series allpass filters.
#define FS_REVERB_COMBS 8
#define FS_REVERB_ALLPASSES 4
// Delay lengths at 44.1 kHz, scaled for other rates (+1 for rounding)
#define FS_REVERB_SCALED(n) ((n) * FS_MAX_SAMPLE_RATE / 44100 + 1)
#define FS_REVERB_COMB_TOTAL FS_REVERB_SCALED(1116 + 1188 + 1277 + 1356 + 1422 + 1491 + 1557 + 1617)
#define FS_REVERB_STEREO_SPREAD 23
#define FS_REVERB_AP_TOTAL FS_REVERB_SCALED(2 * (556 + 441 + 341 + 225) + 4 * FS_REVERB_STEREO_SPREAD)

// A circular delay line inside one of the reverb's memory pools.
typedef struct {
    float *buf;
    uint32_t len;           // Delay in samples
    uint32_t pos;           // Read/write position, 0 .. len-1
    float store;            // Comb damping filter state
} FsDelayLine;

// Reverb state. comb_mem and ap_mem are the delay memory, shared out
// between the delay lines by fs_reverb_init.
typedef struct {
    float comb_mem[FS_REVERB_COMB_TOTAL];
    float ap_mem[FS_REVERB_AP_TOTAL];
    FsDelayLine comb[FS_REVERB_COMBS];
    FsDelayLine ap_l[FS_REVERB_ALLPASSES];
    FsDelayLine ap_r[FS_REVERB_ALLPASSES];
    float feedback;         // Comb feedback, from room size (decay time)
    float damp;             // Comb low pass coefficient (high frequency loss)
    float wet1;             // Gain to the same side
    float wet2;             // Gain to the opposite side (width)
    float input_gain;
} FsReverb;

// Default reverb settings, applied by fs_reverb_init. Chosen to be close
// to FluidSynth: RT60 about 3.5 s, wet about -11 dB below dry at full send.
#define FS_REVERB_DEFAULT_ROOM  0.88f
#define FS_REVERB_DEFAULT_DAMP  0.2f
#define FS_REVERB_DEFAULT_WIDTH 1.0f
#define FS_REVERB_DEFAULT_LEVEL 0.2f

// Clear the delay memory, set the delay lengths for sample_rate (at most
// FS_MAX_SAMPLE_RATE) and apply the default settings.
void fs_reverb_init(FsReverb *r, float sample_rate);
// Change the sound without clearing the tail. Inputs are not range checked
// here (fs_synth_set_reverb clamps them):
// room 0..1, damp 0..1, width 0..1, level 0..1
void fs_reverb_set(FsReverb *r, float room, float damp, float width, float level);
// Run n frames (at most FS_BLOCK_SIZE; more are ignored) of the mono send
// in[] through the reverb and add the wet result to left[] and right[].
void fs_reverb_process(FsReverb *r, const float *in, float *left, float *right, uint32_t n);

// Chorus: one delay line read by FS_CHORUS_TAPS modulated taps whose LFO
// phases are spread evenly over the cycle. Even taps go left, odd taps right.
#define FS_CHORUS_BUF 2048   // Power of 2, 42 ms at 48 kHz
#define FS_CHORUS_TAPS 4

// Chorus state. Delays are in samples (fractional); the taps glide
// linearly from their previous delay to the new one over each block.
typedef struct {
    float buf[FS_CHORUS_BUF];
    uint32_t pos;           // Next write position, kept < FS_CHORUS_BUF
    float phase;            // LFO phase [0, 1)
    float phase_step;       // Per block
    float base;             // Base delay in samples
    float depth;            // Modulation depth in samples
    float level;            // Gain per tap
    float delay[FS_CHORUS_TAPS];  // Tap delays at the end of the previous block
} FsChorus;

// Default chorus settings, applied by fs_chorus_init (about the same wet
// level as FluidSynth). Rate in Hz, delays in ms.
#define FS_CHORUS_DEFAULT_RATE  0.3f
#define FS_CHORUS_DEFAULT_BASE  12.0f
#define FS_CHORUS_DEFAULT_DEPTH 4.0f
#define FS_CHORUS_DEFAULT_LEVEL 0.9f

// Clear the delay buffer and apply the default settings.
void fs_chorus_init(FsChorus *c, float sample_rate);
// rate in Hz, base and depth in ms, level 0..
// Each tap's delay swings between base - depth and base + depth. The
// depth is reduced if needed to keep the delay inside the buffer and above
// 2 samples.
void fs_chorus_set(FsChorus *c, float sample_rate, float rate_hz, float base_ms, float depth_ms, float level);
// Run n frames (at most FS_BLOCK_SIZE) of the mono send in[] through the
// chorus and add the wet result to left[] and right[]. The LFO advances
// once per call, so call it once per block.
void fs_chorus_process(FsChorus *c, const float *in, float *left, float *right, uint32_t n);

// Master EQ: low and high shelving filters on the stereo output (as the
// Roland GS master EQ). Bypassed when both gains are 0 dB.
//
// Biquad coefficients, normalised so a0 = 1:
// y = b0 x + b1 x[-1] + b2 x[-2] - a1 y[-1] - a2 y[-2]
typedef struct {
    float b0, b1, b2, a1, a2;
} FsBiquad;

typedef struct {
    FsBiquad low, high;
    float z[2][4];          // Filter state per channel: low z1 z2, high z1 z2
    bool active;            // False when flat: processing is skipped
    float low_hz, low_db, high_hz, high_db;  // Current settings, for display
} FsEq;

// Clear the filter state and set the EQ flat (200 Hz / 3 kHz, 0 dB).
void fs_eq_init(FsEq *eq, float sample_rate);
// Shelf corner frequencies in Hz and gains in dB (+-12 is the GS range).
// Computes new coefficients (uses libm, so not for every block). The
// filter state is cleared when the EQ switches from flat to active.
void fs_eq_set(FsEq *eq, float sample_rate, float low_hz, float low_db, float high_hz, float high_db);
// Filter n frames of left[] and right[] in place. Does nothing when flat.
void fs_eq_process(FsEq *eq, float *left, float *right, uint32_t n);
