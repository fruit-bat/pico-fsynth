//
// test_smf.c - unit tests for the Standard MIDI File player (fs_smf.c):
// event timing, track merging, tempo changes, running status, SysEx, and
// robustness against bad and truncated files.
//
#include <stdlib.h>
#include "test.h"
#include "midi_writer.h"
#include "fs_smf.h"

#define RATE 48000      // 120 bpm, 480 ppq: one tick = 50 frames

// Events as delivered, with the player time at delivery
typedef struct {
    uint8_t msg[256];
    uint32_t len;
    double at;          // smf->now when delivered (start of the advance call)
} Ev;

static Ev events[256];
static int event_count;
static const FsSmf *current;

static void collect(void *ctx, const uint8_t *msg, uint32_t len) {
    (void)ctx;
    if (event_count < 256) {
        Ev *e = &events[event_count++];
        memcpy(e->msg, msg, len < sizeof(e->msg) ? len : sizeof(e->msg));
        e->len = len;
        e->at = current->now;
    }
}

// Play the whole file in steps of `frames`; returns the total frames
static double play(FsSmf *smf, uint32_t frames) {
    event_count = 0;
    current = smf;
    int guard = 0;
    while (fs_smf_advance(smf, frames, collect, NULL) && ++guard < 1000000) {}
    return smf->now;
}

TEST(single_track_timing) {
    MwFile f;
    mw_begin(&f, 0, 480);
    mw_track(&f);
    mw_event(&f, 0, 3, 0x90, 60, 100);
    mw_event(&f, 480, 3, 0x80, 60, 0);     // One quarter = 0.5 s = 24000 frames
    mw_end_track(&f, 0);
    static FsSmf smf;
    CHECK(fs_smf_init(&smf, f.data, f.size, RATE));
    play(&smf, 100);
    CHECK_EQ(event_count, 2);
    CHECK_EQ(events[0].msg[0], 0x90);
    CHECK_NEAR(events[0].at, 0.0, 0.0);
    CHECK_EQ(events[1].msg[0], 0x80);
    // Delivered in the 100-frame step containing frame 24000
    CHECK_NEAR(events[1].at, 24000.0, 100.0);
    CHECK(smf.finished);
}

// Format 1: events from two tracks come out in time order
TEST(tracks_merge_in_time_order) {
    MwFile f;
    mw_begin(&f, 1, 480);
    mw_track(&f);
    mw_event(&f, 0, 3, 0x90, 1, 1);
    mw_event(&f, 200, 3, 0x90, 3, 1);
    mw_end_track(&f, 0);
    mw_track(&f);
    mw_event(&f, 100, 3, 0x91, 2, 1);
    mw_event(&f, 200, 3, 0x91, 4, 1);
    mw_end_track(&f, 0);
    static FsSmf smf;
    CHECK(fs_smf_init(&smf, f.data, f.size, RATE));
    CHECK_EQ(smf.track_count, 2);
    play(&smf, 32);
    CHECK_EQ(event_count, 4);
    for (int i = 0; i < event_count && i < 4; ++i) CHECK_EQ(events[i].msg[1], i + 1);
}

// A tempo change applies from its tick onwards
TEST(tempo_change) {
    MwFile f;
    mw_begin(&f, 0, 480);
    mw_track(&f);
    mw_tempo(&f, 480, 250000);              // After 1 quarter at 120 bpm: 240 bpm
    mw_event(&f, 480, 3, 0x90, 60, 100);    // 1 more quarter at 240 bpm = 0.25 s
    mw_end_track(&f, 0);
    static FsSmf smf;
    CHECK(fs_smf_init(&smf, f.data, f.size, RATE));
    play(&smf, 1);
    CHECK_EQ(event_count, 1);
    CHECK_NEAR(events[0].at, 0.75 * RATE, 1.0);
}

TEST(running_status) {
    MwFile f;
    mw_begin(&f, 0, 96);
    mw_track(&f);
    mw_event(&f, 0, 3, 0x92, 60, 100);
    mw_event(&f, 10, 2, 61, 101);           // Running status: no status byte
    mw_event(&f, 10, 2, 0xc2, 5);           // Program change, 1 data byte
    mw_event(&f, 10, 1, 6);                 // Running status program change
    mw_end_track(&f, 0);
    static FsSmf smf;
    CHECK(fs_smf_init(&smf, f.data, f.size, RATE));
    play(&smf, 64);
    CHECK_EQ(event_count, 4);
    CHECK_EQ(events[1].len, 3);
    CHECK_EQ(events[1].msg[0], 0x92);
    CHECK_EQ(events[1].msg[1], 61);
    CHECK_EQ(events[3].len, 2);
    CHECK_EQ(events[3].msg[0], 0xc2);
    CHECK_EQ(events[3].msg[1], 6);
}

// F0 SysEx is passed on whole, starting with F0
TEST(sysex_passed_on) {
    MwFile f;
    mw_begin(&f, 0, 96);
    mw_track(&f);
    mw_event(&f, 0, 7, 0xf0, 5, 0x7e, 0x7f, 0x09, 0x01, 0xf7);  // GM on
    mw_end_track(&f, 0);
    static FsSmf smf;
    CHECK(fs_smf_init(&smf, f.data, f.size, RATE));
    play(&smf, 64);
    CHECK_EQ(event_count, 1);
    CHECK_EQ(events[0].len, 6);
    CHECK_EQ(events[0].msg[0], 0xf0);
    CHECK_EQ(events[0].msg[5], 0xf7);
}

TEST(rejects_bad_files) {
    static FsSmf smf;
    const uint8_t junk[32] = { 'R', 'I', 'F', 'F' };
    CHECK(!fs_smf_init(&smf, junk, sizeof(junk), RATE));
    MwFile f;
    mw_begin(&f, 0, 0x8000 | 25);           // SMPTE division: unsupported
    mw_track(&f);
    mw_end_track(&f, 0);
    CHECK(!fs_smf_init(&smf, f.data, f.size, RATE));
    mw_begin(&f, 2, 96);                    // Format 2: unsupported
    mw_track(&f);
    mw_end_track(&f, 0);
    CHECK(!fs_smf_init(&smf, f.data, f.size, RATE));
    mw_begin(&f, 0, 96);                    // No tracks
    CHECK(!fs_smf_init(&smf, f.data, f.size, RATE));
}

// Every truncation of a valid file either fails to load or plays to the
// end without reading past the data (run under ASan to catch overreads:
// each truncated copy is in a heap block of exactly that size).
TEST(truncated_files_are_safe) {
    MwFile f;
    mw_begin(&f, 1, 96);
    mw_track(&f);
    mw_tempo(&f, 0, 400000);
    mw_event(&f, 0, 3, 0x90, 60, 100);
    mw_event(&f, 10, 2, 62, 100);
    mw_event(&f, 10, 6, 0xf0, 4, 1, 2, 3, 0xf7);
    mw_event(&f, 0x0fffff, 3, 0x80, 60, 0); // Long delta: multi-byte VLQ
    mw_end_track(&f, 0);
    mw_track(&f);
    mw_event(&f, 5, 3, 0x91, 40, 90);
    mw_end_track(&f, 0);
    for (uint32_t n = 0; n <= f.size; ++n) {
        uint8_t *copy = malloc(n ? n : 1);
        memcpy(copy, f.data, n);
        static FsSmf smf;
        if (fs_smf_init(&smf, copy, n, RATE)) {
            event_count = 0;
            current = &smf;
            int steps = 0;
            while (fs_smf_advance(&smf, 1 << 20, collect, NULL) && ++steps < 100000) {}
            CHECK(smf.finished);
        }
        free(copy);
    }
}

int main(void) {
    RUN(single_track_timing);
    RUN(tracks_merge_in_time_order);
    RUN(tempo_change);
    RUN(running_status);
    RUN(sysex_passed_on);
    RUN(rejects_bad_files);
    RUN(truncated_files_are_safe);
    return test_summary();
}
