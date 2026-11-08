#pragma once
//
// Buffer ring logic shared by the audio output implementations.
//
#include "pico.h"
#include "hardware/sync.h"
#include "audio_out.h"

// Called from the DMA completion interrupt: returns the next buffer to play,
// or the silence buffer if the producer has not kept up.
static inline const uint32_t *audio_ring_next(AudioRing *r) {
    if (!r->playing_silence) r->read = r->read + 1; // Finished the buffer it was playing
    if (r->read != r->write) {
        r->playing_silence = false;
        return r->buf[r->read % AUDIO_OUT_BUFFERS];
    }
    r->underruns = r->underruns + 1;
    r->playing_silence = true;
    return r->silence;
}

// Wait until a buffer is free and return it
static inline uint32_t *audio_ring_acquire(AudioRing *r) {
    // Buffers read..write-1 are queued (the first one is playing). Buffer
    // write % N is free unless all N are queued.
    while (r->write - r->read >= AUDIO_OUT_BUFFERS) {
        tight_loop_contents();
    }
    return r->buf[r->write % AUDIO_OUT_BUFFERS];
}

static inline void audio_ring_submit(AudioRing *r) {
    __dmb(); // Buffer contents visible before the count changes
    r->write = r->write + 1;
}
