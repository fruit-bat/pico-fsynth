#include "fs_math.h"

float fs_concave_table[129];

void fs_math_init(void) {
    // SF2 2.04 section 9.5.3: concave(x) = -20/96 * log10((1 - x)^2),
    // which reaches 1 when the attenuation hits 96 dB.
    for (int i = 0; i <= 128; ++i) {
        const float x = (float)i / 128.0f;
        float y;
        if (i == 128) {
            y = 1.0f;
        } else {
            y = -(20.0f / 96.0f) * log10f((1.0f - x) * (1.0f - x));
            if (y > 1.0f) y = 1.0f;
        }
        fs_concave_table[i] = y;
    }
}
