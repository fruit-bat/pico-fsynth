//
// fs_math.c - lookup table behind fs_concave() / fs_convex().
//
// Everything else in fs_math.h is inline. The table is computed once at
// start-up (fs_synth_init calls fs_math_init) instead of being stored as a
// constant, so it lands in SRAM and is not read from flash while a voice
// is starting.
//
#include "fs_math.h"

// SF2 concave curve sampled at x = i / 128, i = 0..128. Filled in by
// fs_math_init(); read by fs_concave().
float fs_concave_table[129];
#if FS_INTERP_TABLE_BITS
float fs_interp_weights[(1u << FS_INTERP_TABLE_BITS) + 1][4];
#endif

// Fill fs_concave_table. Safe to call more than once.
void fs_math_init(void) {
    // SF2 2.04 section 9.5.3: concave(x) = -20/96 * log10((1 - x)^2),
    // which reaches 1 when the attenuation hits 96 dB.
    for (int i = 0; i <= 128; ++i) {
        const float x = (float)i / 128.0f;
        float y;
        if (i == 128) {
            y = 1.0f;   // log10(0) is -infinity: use the curve's limit
        } else {
            y = -(20.0f / 96.0f) * log10f((1.0f - x) * (1.0f - x));
            if (y > 1.0f) y = 1.0f;
        }
        fs_concave_table[i] = y;
    }
#if FS_INTERP_TABLE_BITS
    // The polynomial in fs_voice.c's interpolate(), written as weights:
    // s = w0 x[-1] + w1 x[0] + w2 x[1] + w3 x[2]
    for (uint32_t k = 0; k <= (1u << FS_INTERP_TABLE_BITS); ++k) {
        const float f = (float)k / (float)(1u << FS_INTERP_TABLE_BITS);
        const float f2 = f * f, f3 = f2 * f;
        fs_interp_weights[k][0] = -0.5f * f3 + f2 - 0.5f * f;
        fs_interp_weights[k][1] = 1.5f * f3 - 2.5f * f2 + 1.0f;
        fs_interp_weights[k][2] = -1.5f * f3 + 2.0f * f2 + 0.5f * f;
        fs_interp_weights[k][3] = 0.5f * f3 - 0.5f * f2;
    }
#endif
}
