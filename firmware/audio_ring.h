#pragma once
//
// audio_ring.h - buffer ring logic shared by audio_i2s.c and audio_pwm.c.
//
// A single-producer single-consumer ring of AUDIO_OUT_BUFFERS buffers (see
// AudioRing in audio_out.h). The producer is the render loop; the consumer
// is the DMA completion interrupt. Only the producer writes `write` and only
// the interrupt writes `read`, so no lock is needed: a data memory barrier
// before publishing `write` is enough.
//
#include "pico.h"
#include "hardware/sync.h"
#include "audio_out.h"

// Called from the DMA completion interrupt when the buffer it was playing
// has finished. Retires that buffer (unless it was the silence buffer) and
// returns the next one to play: the oldest queued buffer, or the silence
// buffer (counting an underrun) if the producer has not kept up.
// Runs in interrupt context: keep it short, RAM only (it is inlined into
// RAM-resident handlers).
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

// Busy-wait until a buffer is free and return it. Producer only.
static inline uint32_t *audio_ring_acquire(AudioRing *r) {
    // Buffers read..write-1 are queued (the first one is playing). Buffer
    // write % N is free unless all N are queued.
    while (r->write - r->read >= AUDIO_OUT_BUFFERS) {
        tight_loop_contents();
    }
    return r->buf[r->write % AUDIO_OUT_BUFFERS];
}

// Publish the buffer returned by the last acquire to the DMA interrupt.
// Producer only.
static inline void audio_ring_submit(AudioRing *r) {
    __dmb(); // Buffer contents visible before the count changes
    r->write = r->write + 1;
}
