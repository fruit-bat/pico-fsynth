//
// test_math.c - unit tests for fs_math.h (unit conversions, sine, curves)
// and fs_crc32.
//
#include "test.h"
#include "fs_math.h"
#include "fs_crc32.h"

// The standard CRC-32 check value, whole and fed in pieces
TEST(crc32_check_value) {
    CHECK_EQ(fs_crc32(0, "123456789", 9), 0xcbf43926u);
    const uint32_t part = fs_crc32(0, "1234", 4);
    CHECK_EQ(fs_crc32(part, "56789", 5), 0xcbf43926u);
    CHECK_EQ(fs_crc32(0, "", 0), 0);
}

// fs_exp2f against libm over the range the synth uses
TEST(exp2f_accuracy) {
    double worst = 0.0;
    for (float x = -30.0f; x <= 30.0f; x += 0.01f) {
        const double ref = exp2((double)x);
        const double rel = fabs((double)fs_exp2f(x) - ref) / ref;
        if (rel > worst) worst = rel;
    }
    CHECK(worst < 3e-6);
    CHECK_EQ(fs_exp2f(-200.0f) == 0.0f, 1);
    CHECK_NEAR(fs_exp2f(0.0f), 1.0, 1e-7);
    CHECK_NEAR(fs_exp2f(10.0f), 1024.0, 1024.0 * 3e-6);
}

TEST(unit_conversions) {
    CHECK_NEAR(fs_cb_to_amp(0.0f), 1.0, 1e-6);
    CHECK_NEAR(fs_cb_to_amp(200.0f), 0.1, 1e-6);              // 20 dB
    CHECK_NEAR(fs_amp_to_cb(0.1f), 200.0, 1e-3);
    CHECK_NEAR(fs_amp_to_cb(0.0f), 1200.0, 0.0);              // Silence
    CHECK_NEAR(fs_tc_to_sec(0.0f), 1.0, 1e-6);
    CHECK_NEAR(fs_tc_to_sec(-1200.0f), 0.5, 1e-6);
    CHECK_NEAR(fs_tc_to_sec(-12000.0f), 0.0, 0.0);            // "Instant"
    CHECK_NEAR(fs_tc_to_sec(20000.0f), fs_tc_to_sec(8000.0f), 0.0);  // Clamped
    CHECK_NEAR(fs_abs_cents_to_hz(6900.0f), 440.0, 0.01);
    CHECK_NEAR(fs_cents_to_ratio(1200.0f), 2.0, 1e-5);
    CHECK_NEAR(fs_cents_to_ratio(-1200.0f), 0.5, 1e-6);
}

TEST(sine) {
    for (float w = 0.0f; w <= 3.14159f; w += 0.01f) {
        float sn, cs;
        fs_sincos(w, &sn, &cs);
        CHECK_NEAR(sn, sin(w), 2e-6);
        CHECK_NEAR(cs, cos(w), 2e-6);
    }
    for (float p = 0.0f; p < 1.0f; p += 0.001f) {
        CHECK_NEAR(fs_sin_turns(p), sin(2.0 * M_PI * p), 2e-6);
    }
}

TEST(frac32) {
    CHECK_NEAR(fs_frac32_to_float(0), 0.0, 0.0);
    CHECK_NEAR(fs_frac32_to_float(0x80000000u), 0.5, 0.0);
    CHECK(fs_frac32_to_float(0xffffffffu) <= 1.0f);
}

// Endpoints, monotonic, and convex mirrors concave
TEST(curves) {
    fs_math_init();
    CHECK_NEAR(fs_concave(0.0f), 0.0, 0.0);
    CHECK_NEAR(fs_concave(1.0f), 1.0, 0.0);
    CHECK_NEAR(fs_concave(-1.0f), 0.0, 0.0);
    CHECK_NEAR(fs_concave(2.0f), 1.0, 0.0);
    float prev = -1.0f;
    for (float x = 0.0f; x <= 1.0f; x += 0.001f) {
        const float y = fs_concave(x);
        CHECK(y >= prev);
        prev = y;
        CHECK_NEAR(fs_convex(x), 1.0f - fs_concave(1.0f - x), 1e-6);
    }
    // Concave starts slowly: half way it is well below the diagonal
    CHECK(fs_concave(0.5f) < 0.2f);
}

int main(void) {
    RUN(crc32_check_value);
    RUN(exp2f_accuracy);
    RUN(unit_conversions);
    RUN(sine);
    RUN(frac32);
    RUN(curves);
    return test_summary();
}
