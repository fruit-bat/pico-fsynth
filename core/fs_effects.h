#pragma once
//
// Reverb and chorus. Both take a mono send bus and add stereo output.
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

typedef struct {
    float *buf;
    uint32_t len;
    uint32_t pos;
    float store;            // Comb damping filter state
} FsDelayLine;

typedef struct {
    float comb_mem[FS_REVERB_COMB_TOTAL];
    float ap_mem[FS_REVERB_AP_TOTAL];
    FsDelayLine comb[FS_REVERB_COMBS];
    FsDelayLine ap_l[FS_REVERB_ALLPASSES];
    FsDelayLine ap_r[FS_REVERB_ALLPASSES];
    float feedback;
    float damp;
    float wet1;             // Gain to the same side
    float wet2;             // Gain to the opposite side (width)
    float input_gain;
} FsReverb;

void fs_reverb_init(FsReverb *r, float sample_rate);
// room 0..1, damp 0..1, width 0..1, level 0..1
void fs_reverb_set(FsReverb *r, float room, float damp, float width, float level);
void fs_reverb_process(FsReverb *r, const float *in, float *left, float *right, uint32_t n);

// Chorus: one delay line read by FS_CHORUS_TAPS modulated taps whose LFO
// phases are spread evenly over the cycle. Even taps go left, odd taps right.
#define FS_CHORUS_BUF 2048   // Power of 2, 42 ms at 48 kHz
#define FS_CHORUS_TAPS 4

typedef struct {
    float buf[FS_CHORUS_BUF];
    uint32_t pos;
    float phase;            // LFO phase [0, 1)
    float phase_step;       // Per block
    float base;             // Base delay in samples
    float depth;            // Modulation depth in samples
    float level;            // Gain per tap
    float delay[FS_CHORUS_TAPS];  // Tap delays at the end of the previous block
} FsChorus;

void fs_chorus_init(FsChorus *c, float sample_rate);
// rate in Hz, base and depth in ms, level 0..
void fs_chorus_set(FsChorus *c, float sample_rate, float rate_hz, float base_ms, float depth_ms, float level);
void fs_chorus_process(FsChorus *c, const float *in, float *left, float *right, uint32_t n);

// Master EQ: low and high shelving filters on the stereo output (as the
// Roland GS master EQ). Bypassed when both gains are 0 dB.
typedef struct {
    float b0, b1, b2, a1, a2;
} FsBiquad;

typedef struct {
    FsBiquad low, high;
    float z[2][4];          // Filter state per channel: low z1 z2, high z1 z2
    bool active;
    float low_hz, low_db, high_hz, high_db;
} FsEq;

void fs_eq_init(FsEq *eq, float sample_rate);
// Shelf corner frequencies in Hz and gains in dB (+-12 is the GS range)
void fs_eq_set(FsEq *eq, float sample_rate, float low_hz, float low_db, float high_hz, float high_db);
void fs_eq_process(FsEq *eq, float *left, float *right, uint32_t n);
