#pragma once
//
// audio_i2s.h - I2S audio output on the ESP32-P4 (ESP-IDF i2s_std driver).
//
// The renderer fills a buffer of AUDIO_FRAMES interleaved int16 stereo
// frames and passes it to audio_write(), which copies it into the driver's
// DMA ring and blocks while the ring is full. That blocking paces the
// render task to the output rate. The ring is AUDIO_DMA_BUFFERS buffers of
// AUDIO_FRAMES each, the same latency as the RP2350 firmware (8.7 ms at
// 44.1 kHz). If the renderer falls behind, the driver plays zeros and
// counts an underrun.
//
#include <stdint.h>
#include <stdbool.h>
#include "fs_config.h"

#define AUDIO_DMA_BUFFERS 3
#define AUDIO_FRAMES (FS_BLOCK_SIZE * 4)   // Frames per buffer (4 synth blocks)

// Start the output at sample_rate (Hz) on the pins set in menuconfig,
// playing silence. Returns false if the driver could not be set up.
bool audio_init(uint32_t sample_rate);

// Queue AUDIO_FRAMES stereo frames (L, R, L, R...). Blocks until there is
// room in the DMA ring.
void audio_write(const int16_t *frames);

// DMA buffers of silence played because no data was ready.
uint32_t audio_underruns(void);
