#pragma once
//
// audio_out.h - audio output interface: a DMA-fed ring of stereo buffers.
//
// Exactly one implementation is linked in, chosen by the CMake option
// FS_AUDIO_OUT: audio_i2s.c (PIO I2S to an external DAC) or audio_pwm.c
// (two PWM pins with RC filters). Both share the AudioRing below and the
// helpers in audio_ring.h.
//
// Data flow: the render loop on core 1 calls audio_out_acquire() to get
// the next free buffer, fills it with AUDIO_OUT_FRAMES interleaved int16
// stereo frames (L, R, L, R...) and hands it back with audio_out_submit().
// A DMA channel streams submitted buffers to the hardware; its completion
// interrupt (DMA_IRQ_1) chains the next buffer, or a buffer of silence if
// the producer has fallen behind (an "underrun"). The interrupt runs on
// the core that called audio_out_init(), which in this firmware is core 1.
//
// Latency is up to AUDIO_OUT_BUFFERS buffers: 3 x 128 frames = 8.7 ms at
// 44.1 kHz with the default block size.
//
#include <stdint.h>
#include <stdbool.h>
#include "fs_config.h"

#define AUDIO_OUT_BUFFERS 3                     // Buffers in the ring
#define AUDIO_OUT_FRAMES (FS_BLOCK_SIZE * 4)   // Frames per buffer (4 synth blocks)

// Claim the hardware (PIO state machine or PWM slice, a DMA channel and
// DMA_IRQ_1), set the output clock as close to sample_rate (Hz) as the
// dividers allow and start playing silence. Returns false if the hardware
// could not be claimed or the pins/rate are unusable. Call once, on the
// core that should service the DMA interrupt.
bool audio_out_init(uint32_t sample_rate);

// Wait (busy loop) for a free buffer and return it: AUDIO_OUT_FRAMES
// stereo frames of int16 (L, R, L, R...). Producer core only. Must be
// followed by audio_out_submit() before the next acquire.
int16_t *audio_out_acquire(void);

// Queue the buffer returned by the last acquire for playback. The PWM
// implementation converts the samples to compare values in place here.
void audio_out_submit(void);

// Number of times the DMA had to play silence because no buffer was ready.
// Includes the silence played at start up before the first submit.
uint32_t audio_out_underruns(void);

// Actual output rate in Hz after clock divider rounding (exact for I2S
// with a fractional divider; PWM rounds to whole system clock counts).
float audio_out_rate(void);

// ---------------------------------------------------------------------------
// Shared buffer ring used by both implementations
// ---------------------------------------------------------------------------

// read and write are free-running counts (they wrap at 2^32, which the
// unsigned arithmetic in audio_ring.h tolerates). Buffer n lives at
// buf[n % AUDIO_OUT_BUFFERS]. Buffers read .. write-1 are queued; the one at
// read is the one the DMA is playing, unless playing_silence is set.
typedef struct {
    uint32_t buf[AUDIO_OUT_BUFFERS][AUDIO_OUT_FRAMES];  // One word per stereo frame: L low half, R high half
    uint32_t silence[AUDIO_OUT_FRAMES];                 // Played on underrun (zeros, or PWM mid level)
    volatile uint32_t read;        // Buffers consumed by DMA (count); written by the ISR
    volatile uint32_t write;       // Buffers submitted (count); written by the producer
    volatile uint32_t underruns;   // Silence buffers played
    volatile bool playing_silence; // DMA is currently playing `silence`, not buf[read]
} AudioRing;
