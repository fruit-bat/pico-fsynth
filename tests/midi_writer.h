#pragma once
//
// midi_writer.h - build Standard MIDI Files in memory for tests.
//
//   MwFile f; mw_begin(&f, 1, 480);       // format 1, 480 ticks per quarter
//   mw_track(&f);
//   mw_event(&f, 0, 3, 0x90, 60, 100);    // delta 0: note on
//   mw_event(&f, 480, 3, 0x80, 60, 0);    // one quarter later: note off
//   mw_end_track(&f);
//   ... f.data, f.size
//
// Fixed capacity, no allocation. Events are written as given (a status of
// 0 omits the status byte, for running status tests).
//
#include <stdint.h>
#include <string.h>
#include <stdarg.h>

typedef struct {
    uint8_t data[8192];
    uint32_t size;
    uint32_t track_start;   // Offset of the current MTrk length field
    uint16_t tracks;
} MwFile;

static inline void mw_byte(MwFile *f, uint8_t b) {
    if (f->size < sizeof(f->data)) f->data[f->size++] = b;
}

static inline void mw_be(MwFile *f, uint32_t v, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) mw_byte(f, (uint8_t)(v >> (8 * i)));
}

// Variable length quantity
static inline void mw_vlq(MwFile *f, uint32_t v) {
    uint8_t tmp[5];
    int n = 0;
    tmp[n++] = v & 0x7f;
    while (v >>= 7) tmp[n++] = (uint8_t)(0x80 | (v & 0x7f));
    while (n) mw_byte(f, tmp[--n]);
}

// MThd. The track count is filled in as tracks are added.
static inline void mw_begin(MwFile *f, uint16_t format, uint16_t division) {
    memset(f, 0, sizeof(*f));
    memcpy(f->data, "MThd", 4);
    f->size = 4;
    mw_be(f, 6, 4);
    mw_be(f, format, 2);
    mw_be(f, 0, 2);
    mw_be(f, division, 2);
}

static inline void mw_track(MwFile *f) {
    mw_byte(f, 'M'); mw_byte(f, 'T'); mw_byte(f, 'r'); mw_byte(f, 'k');
    f->track_start = f->size;
    mw_be(f, 0, 4);
    f->tracks++;
    f->data[10] = (uint8_t)(f->tracks >> 8);
    f->data[11] = (uint8_t)f->tracks;
}

// One event: delta time, then n bytes
static inline void mw_event(MwFile *f, uint32_t delta, int n, ...) {
    mw_vlq(f, delta);
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; ++i) mw_byte(f, (uint8_t)va_arg(ap, int));
    va_end(ap);
}

// Tempo meta event, microseconds per quarter note
static inline void mw_tempo(MwFile *f, uint32_t delta, uint32_t us) {
    mw_event(f, delta, 6, 0xff, 0x51, 3, (us >> 16) & 0xff, (us >> 8) & 0xff, us & 0xff);
}

// End of track meta event, and fix up the track length
static inline void mw_end_track(MwFile *f, uint32_t delta) {
    mw_event(f, delta, 3, 0xff, 0x2f, 0);
    const uint32_t len = f->size - f->track_start - 4;
    for (int i = 0; i < 4; ++i) f->data[f->track_start + i] = (uint8_t)(len >> (24 - 8 * i));
}
