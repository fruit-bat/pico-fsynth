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
}
