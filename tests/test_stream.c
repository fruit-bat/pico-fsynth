//
// test_stream.c - unit tests for the per-voice sample streamer
// (fs_stream.c): loop unrolling, the leading zero frame, silence after the
// end, loop-until-release, and the guard frames after the ring end.
//
#include "test.h"
#include "fs_stream.h"

static int16_t ring[FS_STREAM_FRAMES + FS_STREAM_GUARD];
static int16_t data[4096];

static void init_data(void) {
    for (int i = 0; i < 4096; ++i) data[i] = (int16_t)(i + 1);  // Never 0
}

// Read virtual frames [from, to) one block at a time, the way a voice does
// (fill, then read), checking each against expect(vpos)
typedef int16_t (*Expect)(uint32_t vpos);

static int check_frames(FsStream *s, uint32_t from, uint32_t to, Expect expect) {
    int bad = 0;
    for (uint32_t pos = from; pos < to; pos += 32) {
        fs_stream_fill(s, pos, 64);
        CHECK(fs_stream_available(s, pos) >= 64);
        for (uint32_t k = 0; k < 32 && pos + k < to; ++k) {
            if (fs_stream_frame(s, pos + k) != expect(pos + k)) ++bad;
        }
    }
    return bad;
}

// Sample frames 100..299 (200 frames), loop 150..250
static int16_t expect_looped(uint32_t vpos) {
    if (vpos == 0) return 0;
    const uint32_t i = vpos - 1;            // Index into the unrolled sample
    if (i < 150) return data[100 + i];      // Up to the loop end (frame 250)
    return data[150 + (i - 150) % 100];     // Then the loop repeats
}

TEST(loop_unrolled) {
    init_data();
    FsStream s = { .ring = ring };
    fs_stream_start(&s, data, 100, 300, 150, 250, true);
    CHECK_EQ(fs_stream_frame(&s, 0), 0);
    // Many times round the ring and the loop
    CHECK_EQ(check_frames(&s, 1, 20000, expect_looped), 0);
    CHECK(!fs_stream_finished(&s, 20000));
}

static int16_t expect_oneshot(uint32_t vpos) {
    if (vpos == 0 || vpos > 200) return 0;
    return data[100 + vpos - 1];
}

TEST(one_shot_then_silence) {
    init_data();
    FsStream s = { .ring = ring };
    fs_stream_start(&s, data, 100, 300, 150, 250, false);
    CHECK_EQ(check_frames(&s, 1, 3000, expect_oneshot), 0);
    CHECK(s.src_done);
    CHECK_EQ(s.end_vpos, 201);
    CHECK(!fs_stream_finished(&s, 200));
    CHECK(fs_stream_finished(&s, 201));
}

// An empty loop is treated as no loop
TEST(empty_loop_is_one_shot) {
    init_data();
    FsStream s = { .ring = ring };
    fs_stream_start(&s, data, 100, 300, 150, 150, true);
    CHECK(!s.looping);
    CHECK_EQ(check_frames(&s, 1, 1000, expect_oneshot), 0);
}

// Loop until release: stop looping part way; the frames already in the
// ring are kept, then the sample plays on past the loop end to its end.
TEST(stop_looping) {
    init_data();
    FsStream s = { .ring = ring };
    fs_stream_start(&s, data, 100, 300, 150, 250, true);
    CHECK_EQ(check_frames(&s, 1, 500, expect_looped), 0);
    fs_stream_stop_looping(&s);
    // Read on until silence; the sample must reach frame 299 (value 300)
    // and nothing after the end may be non-zero
    bool saw_end = false, silent = false, bad = false;
    for (uint32_t pos = 500; pos < 5000; pos += 32) {
        fs_stream_fill(&s, pos, 64);
        for (uint32_t k = 0; k < 32; ++k) {
            const int16_t v = fs_stream_frame(&s, pos + k);
            if (v == data[299]) saw_end = true;
            if (saw_end && v == 0) silent = true;
            if (silent && v != 0) bad = true;
        }
    }
    CHECK(saw_end);
    CHECK(silent);
    CHECK(!bad);
    CHECK(s.src_done);
}

// The guard frames after the ring end mirror its first frames, so the
// cubic interpolator can read 4 frames from any position without wrapping
TEST(guard_frames) {
    init_data();
    FsStream s = { .ring = ring };
    fs_stream_start(&s, data, 0, 4096, 0, 4096, true);
    for (uint32_t pos = 1; pos < 3 * FS_STREAM_FRAMES; pos += 32) {
        fs_stream_fill(&s, pos, 64);
        for (int k = 0; k < FS_STREAM_GUARD - 1; ++k) {
            CHECK_EQ(ring[FS_STREAM_FRAMES + k], ring[k]);
        }
    }
}

int main(void) {
    RUN(loop_unrolled);
    RUN(one_shot_then_silence);
    RUN(empty_loop_is_one_shot);
    RUN(stop_looping);
    RUN(guard_frames);
    return test_summary();
}
