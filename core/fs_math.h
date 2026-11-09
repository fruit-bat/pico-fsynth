#pragma once
//
// fs_math.h - fast float helpers: SoundFont unit conversions, sine, and
// the SF2 concave/convex modulator curves.
//
// SoundFonts express almost everything in logarithmic units, and the synth
// converts them at control rate (once per block per voice), so these sit
// on the hot path. They are written to compile to straight-line single
// precision code on the Cortex-M33 FPU: no libm calls, no doubles.
//
// SF2 units used throughout the synth:
//   cents         1/100 semitone; 1200 cents = one octave (a factor of 2).
//   absolute cents  pitch or frequency: 0 = 8.176 Hz (MIDI note 0), so
//                 6900 = 440 Hz. Used for filter cutoff and LFO rates.
//   centibels (cB)  1/10 dB of attenuation; 0 = full level, 960 = -96 dB.
//   timecents     time as 1200 * log2(seconds): 0 = 1 s, -1200 = 0.5 s,
//                 -12000 or less = "instant" (0 s).
//
// fs_math_init() must be called once (fs_synth_init does it) before
// fs_concave() or fs_convex() are used.
//
#include <stdint.h>
#include <math.h>

// 2^x. Relative error < 3e-6 (0.005 cents) for |x| < 126; returns 0 below
// -126 and saturates at 2^126 above.
// Splits x into a nearest integer n and a fraction f in [-0.5, 0.5],
// evaluates 2^f with a polynomial and adds n straight into the float's
// exponent bits.
static inline float fs_exp2f(float x) {
    if (x < -126.0f) return 0.0f;
    if (x > 126.0f) x = 126.0f;
    const float xr = floorf(x + 0.5f);
    const float f = x - xr; // [-0.5, 0.5]
    // Taylor series of 2^f around 0, degree 6
    float p = 1.540353e-4f;
    p = p * f + 1.333355e-3f;
    p = p * f + 9.618129e-3f;
    p = p * f + 5.550411e-2f;
    p = p * f + 2.402265e-1f;
    p = p * f + 6.931472e-1f;
    p = p * f + 1.0f;
    union { float f; int32_t i; } u = { p };
    u.i += (int32_t)xr * (1 << 23); // Add to the exponent field
    return u.f;
}

// Centibels of attenuation to linear amplitude: 10^(-cb/200).
// 0 cB -> 1.0, 60 cB (6 dB) -> about 0.5, 960 cB -> about 1.6e-5.
// Computed as 2^(cb * -log2(10)/200).
static inline float fs_cb_to_amp(float cb) {
    return fs_exp2f(cb * -0.016609640f);
}

// Linear amplitude to centibels of attenuation (inverse of fs_cb_to_amp).
// Amplitudes at or below 1e-6 return 1200 cB (120 dB down, i.e. silent).
// Uses log10f, so keep it off the per-sample path.
static inline float fs_amp_to_cb(float amp) {
    if (amp <= 1e-6f) return 1200.0f;
    return -200.0f * log10f(amp);
}

// Timecents to seconds: 2^(tc/1200). Values at or below -12000 mean
// "instant" and return exactly 0. Clamped at 8000 timecents (about 100 s),
// the longest time the SF2 spec allows for envelope stages.
static inline float fs_tc_to_sec(float tc) {
    if (tc <= -12000.0f) return 0.0f;
    if (tc > 8000.0f) tc = 8000.0f;
    return fs_exp2f(tc * (1.0f / 1200.0f));
}

// Absolute cents to Hz: 0 cents = 8.176 Hz (MIDI note 0), 6900 = 440 Hz.
static inline float fs_abs_cents_to_hz(float cents) {
    return 8.175799f * fs_exp2f(cents * (1.0f / 1200.0f));
}

// Relative cents to a frequency ratio: +1200 -> 2.0, -1200 -> 0.5.
static inline float fs_cents_to_ratio(float cents) {
    return fs_exp2f(cents * (1.0f / 1200.0f));
}

// sin(x) for x in [-pi/2, pi/2] only, error < 1e-7 (Taylor series,
// degree 11). The building block for the wider-range functions below.
static inline float fs_sin_half_pi(float x) {
    const float x2 = x * x;
    float p = -2.5052108e-8f;
    p = p * x2 + 2.7557319e-6f;
    p = p * x2 - 1.9841270e-4f;
    p = p * x2 + 8.3333333e-3f;
    p = p * x2 - 1.6666667e-1f;
    return x + x * x2 * p;
}

// sin and cos of w in [0, pi] without library calls, written to *sn and
// *cs. Used for filter coefficients, where w = 2 pi fc / sample rate is
// always in that range.
static inline void fs_sincos(float w, float *sn, float *cs) {
    const float half_pi = 1.5707963f;
    *sn = fs_sin_half_pi(w > half_pi ? 3.1415927f - w : w);
    *cs = fs_sin_half_pi(half_pi - w);
}

// sin(2 pi p) for a phase p in turns, p in [0, 1). Used by the chorus LFO.
static inline float fs_sin_turns(float p) {
    float w = 6.2831853f * (p - 0.5f);   // [-pi, pi), sin(w) = -sin(2 pi p)
    if (w > 1.5707963f) w = 3.1415927f - w;
    else if (w < -1.5707963f) w = -3.1415927f - w;
    return -fs_sin_half_pi(w);
}

// 0.32 fixed point fraction to float in [0, 1): frac / 2^32. The voice
// playhead keeps its fractional part this way (see FsVoice.frac).
// One VCVT on the M33 FPU instead of a convert and a multiply; portable
// C elsewhere.
static inline float fs_frac32_to_float(uint32_t frac) {
#if defined(__ARM_FP) && defined(__thumb2__)
    union { uint32_t u; float f; } c = { frac };
    __asm__("vcvt.f32.u32 %0, %0, #32" : "+t"(c.f));
    return c.f;
#else
    return (float)frac * (1.0f / 4294967296.0f);
#endif
}

// x limited to [lo, hi].
static inline float fs_clampf(float x, float lo, float hi) {
    return x < lo ? lo : x > hi ? hi : x;
}

// SF2 concave curve sampled at 129 points over [0, 1], see fs_math.c
extern float fs_concave_table[129];

// Build fs_concave_table. Call once before any voice starts.
void fs_math_init(void);

// SF2 modulator curves, mapping [0, 1] onto [0, 1]. Inputs outside the
// range are clamped.
//
// Concave: starts flat and rises steeply at the end. Applied to an
// attenuation it gives a natural loudness response (this is how velocity
// and CC7/CC11 become centibels of attenuation). Linear interpolation of
// fs_concave_table.
static inline float fs_concave(float x) {
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    const float p = x * 128.0f;
    const int i = (int)p;
    const float f = p - (float)i;
    return fs_concave_table[i] + f * (fs_concave_table[i + 1] - fs_concave_table[i]);
}

// Convex: the concave curve mirrored, rising steeply at the start and
// flattening out.
static inline float fs_convex(float x) {
    return 1.0f - fs_concave(1.0f - x);
}
