//
// test_stream.c - unit tests for the per-voice sample streamer
// (fs_stream.c): loop unrolling, the leading zero frame, silence after the
// end, loop-until-release, and the guard frames after the ring end. Each
// runs three times: on plain PCM samples and on the same samples
// compressed in each mode (fs_codec.h). Then tests of the codec itself: round trips of awkward
// signals, decoding in odd chunk sizes, and seeking to every block start.
//
#include <stdlib.h>
#include <string.h>
#include "test.h"
#include "fs_stream.h"

static int16_t ring[FS_STREAM_FRAMES + FS_STREAM_GUARD];
static int16_t data[4096];

static FsFont pcm_font, fast_font, small_font;
static const FsFont *font;     // The one the stream tests use
static uint8_t *fast_data, *small_data;

// The seek points the stream tests use: region starts 100 and 0, loop
// starts 150 and 0
static const uint32_t cuts[] = { 0, 100, 150 };
static FsSeekPoint fast_seek[3], small_seek[3];

static void init_data(void) {
    for (int i = 0; i < 4096; ++i) data[i] = (int16_t)(i + 1);  // Never 0
}

// A compressed font over data[] (only the sample fields are set)
static FsFont compressed_font(FsCodecMode mode, uint8_t **enc, FsSeekPoint *seek) {
    uint32_t offs[3];
    size_t size = 0;
    *enc = fs_encode(data, 4096, cuts, 3, offs, &size, mode);
    for (int i = 0; i < 3; ++i) seek[i] = (FsSeekPoint){ cuts[i], offs[i] };
    return (FsFont){ .cdata = *enc, .data_format = FS_DATA_RICE, .data_size = (uint32_t)size,
                     .seek = seek, .seek_count = 3 };
}

// Plain and compressed (both modes) fonts over data[]
static void init_fonts(void) {
    init_data();
    pcm_font = (FsFont){ .data = data, .data_format = FS_DATA_PCM16, .data_size = sizeof(data) };
    fast_font = compressed_font(FS_CODEC_FAST, &fast_data, fast_seek);
    small_font = compressed_font(FS_CODEC_SMALL, &small_data, small_seek);
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
    FsStream s = { .ring = ring };
    fs_stream_start(&s, font, 100, 300, 150, 250, true);
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
    FsStream s = { .ring = ring };
    fs_stream_start(&s, font, 100, 300, 150, 250, false);
    CHECK_EQ(check_frames(&s, 1, 3000, expect_oneshot), 0);
    CHECK(s.src_done);
    CHECK_EQ(s.end_vpos, 201);
    CHECK(!fs_stream_finished(&s, 200));
    CHECK(fs_stream_finished(&s, 201));
}

// An empty loop is treated as no loop
TEST(empty_loop_is_one_shot) {
    FsStream s = { .ring = ring };
    fs_stream_start(&s, font, 100, 300, 150, 150, true);
    CHECK(!s.looping);
    CHECK_EQ(check_frames(&s, 1, 1000, expect_oneshot), 0);
}

// Loop until release: stop looping part way; the frames already in the
// ring are kept, then the sample plays on past the loop end to its end.
TEST(stop_looping) {
    FsStream s = { .ring = ring };
    fs_stream_start(&s, font, 100, 300, 150, 250, true);
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
    FsStream s = { .ring = ring };
    fs_stream_start(&s, font, 0, 4096, 0, 4096, true);
    for (uint32_t pos = 1; pos < 3 * FS_STREAM_FRAMES; pos += 32) {
        fs_stream_fill(&s, pos, 64);
        for (int k = 0; k < FS_STREAM_GUARD - 1; ++k) {
            CHECK_EQ(ring[FS_STREAM_FRAMES + k], ring[k]);
        }
    }
}

// ---------------------------------------------------------------------------
// Codec
// ---------------------------------------------------------------------------

// Encode x[0, n) with the given cuts, decode it all back in chunks of
// varying size, and check it matches; then decode from every cut.
static int round_trip_mode(const int16_t *x, uint32_t n, const uint32_t *c, uint32_t nc, FsCodecMode mode) {
    uint32_t *offs = malloc((nc ? nc : 1) * sizeof(uint32_t));
    size_t size = 0;
    uint8_t *enc = fs_encode(x, n, c, nc, offs, &size, mode);
    int16_t *y = malloc(n * sizeof(int16_t));
    int bad = 0;
    FsDecoder d;
    fs_decoder_seek(&d, enc, (uint32_t)size, 0);
    for (uint32_t pos = 0, step = 1; pos < n; pos += step, step = step % 157 + 3) {
        const uint32_t m = n - pos < step ? n - pos : step;
        fs_decoder_read(&d, y + pos, m);
    }
    for (uint32_t i = 0; i < n; ++i) bad += y[i] != x[i];
    for (uint32_t i = 0; i < nc; ++i) {
        fs_decoder_seek(&d, enc, (uint32_t)size, offs[i]);
        const uint32_t m = n - c[i] < 300 ? n - c[i] : 300;
        fs_decoder_read(&d, y, m);
        for (uint32_t j = 0; j < m; ++j) bad += y[j] != x[c[i] + j];
    }
    free(enc); free(y); free(offs);
    return bad;
}

static int round_trip(const int16_t *x, uint32_t n, const uint32_t *c, uint32_t nc) {
    return round_trip_mode(x, n, c, nc, FS_CODEC_FAST) + round_trip_mode(x, n, c, nc, FS_CODEC_SMALL);
}

TEST(codec_round_trips) {
    enum { N = 20000 };
    static int16_t x[N];
    uint32_t c[] = { 0, 1, 2, 3, 127, 128, 129, 1000, 1001, 5000, 19999 };
    const uint32_t nc = sizeof(c) / sizeof(c[0]);
    srand(1);
    // White noise, full scale
    for (int i = 0; i < N; ++i) x[i] = (int16_t)(rand() & 0xffff);
    CHECK_EQ(round_trip(x, N, c, nc), 0);
    // Worst case for the predictors: full scale alternating extremes
    for (int i = 0; i < N; ++i) x[i] = (i & 1) ? 32767 : -32768;
    CHECK_EQ(round_trip(x, N, c, nc), 0);
    // A quiet sine, which compresses well
    for (int i = 0; i < N; ++i) x[i] = (int16_t)(1000.0 * __builtin_sin(i * 0.01));
    CHECK_EQ(round_trip(x, N, c, nc), 0);
    // Silence, and a single frame
    memset(x, 0, sizeof(x));
    CHECK_EQ(round_trip(x, N, c, nc), 0);
    x[0] = -12345;
    CHECK_EQ(round_trip(x, 1, c, 1), 0);
}

// Reads past the end of corrupt or truncated data must not hang or read
// outside the buffer (ASan checks the latter)
TEST(codec_truncated_input) {
    int16_t x[600], y[600];
    for (int i = 0; i < 600; ++i) x[i] = (int16_t)(i * 37);
    uint32_t c = 0, off;
    size_t size = 0;
    for (int mode = 0; mode < 2; ++mode) {
        uint8_t *enc = fs_encode(x, 600, &c, 1, &off, &size, (FsCodecMode)mode);
        FsDecoder d;
        fs_decoder_seek(&d, enc, (uint32_t)(size / 2), 0);
        fs_decoder_read(&d, y, 600);   // Garbage after the cut, but returns
        fs_decoder_seek(&d, enc, 0, 0);
        fs_decoder_read(&d, y, 600);
        free(enc);
    }
    CHECK(1);
}

int main(void) {
    init_fonts();
    const FsFont *fonts[] = { &pcm_font, &fast_font, &small_font };
    for (int pass = 0; pass < 3; ++pass) {
        font = fonts[pass];
        RUN(loop_unrolled);
        RUN(one_shot_then_silence);
        RUN(empty_loop_is_one_shot);
        RUN(stop_looping);
        RUN(guard_frames);
    }
    RUN(codec_round_trips);
    RUN(codec_truncated_input);
    free(fast_data);
    free(small_data);
    return test_summary();
}
