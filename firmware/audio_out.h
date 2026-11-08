#pragma once
//
// Audio output: a small ring of DMA buffers of interleaved 16-bit stereo.
// One implementation is linked in: audio_i2s.c or audio_pwm.c.
//
// The producer (the render loop) calls audio_out_acquire() to get the next
// free buffer, fills it and hands it back with audio_out_submit(). The DMA
// completion interrupt runs on the core that called audio_out_init().
//
#include <stdint.h>
#include <stdbool.h>
#include "fs_config.h"

#define AUDIO_OUT_BUFFERS 3
#define AUDIO_OUT_FRAMES (FS_BLOCK_SIZE * 4)   // Frames per buffer

bool audio_out_init(uint32_t sample_rate);

// Wait for a free buffer: AUDIO_OUT_FRAMES stereo frames (L, R, L, R...)
int16_t *audio_out_acquire(void);

// Queue the buffer returned by the last acquire for playback
void audio_out_submit(void);

// Number of times the DMA had to play silence because no buffer was ready
uint32_t audio_out_underruns(void);

// Actual output rate (PWM rates are rounded to whole clock counts)
float audio_out_rate(void);

// ---------------------------------------------------------------------------
// Shared buffer ring used by both implementations
// ---------------------------------------------------------------------------

typedef struct {
    uint32_t buf[AUDIO_OUT_BUFFERS][AUDIO_OUT_FRAMES];  // One word per stereo frame
    uint32_t silence[AUDIO_OUT_FRAMES];
    volatile uint32_t read;        // Buffers consumed by DMA (count)
    volatile uint32_t write;       // Buffers submitted (count)
    volatile uint32_t underruns;
    volatile bool playing_silence;
} AudioRing;
