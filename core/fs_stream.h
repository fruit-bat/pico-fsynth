#pragma once
//
// Per-voice sample streamer.
//
// A voice never reads font sample data directly. Instead the streamer
// copies the sample into a small ring buffer as a continuous "virtual"
// stream with loops unrolled and silence appended after the end. The
// interpolator can then read any frame from playhead-1 to playhead+2
// without worrying about loop points or sample boundaries.
//
// Virtual frame 0 is a zero frame (so cubic interpolation has a frame
// before the start); the sample starts at virtual frame 1.
//
// On the host, and as a first step on the Pico, refills are a memcpy from
// the font. The copy is isolated in fs_stream_fill so it can later become
// an asynchronous DMA transfer from flash.
//
#include <stdint.h>
#include <stdbool.h>
#include "fs_config.h"

#define FS_STREAM_MASK (FS_STREAM_FRAMES - 1)
// The first frames of the ring are repeated after its end so the 4 frames
// used by cubic interpolation are always contiguous: ring[(pos - 1) & mask]
// to ring[((pos - 1) & mask) + 3] with a single wrap per output sample.
#define FS_STREAM_GUARD 4

typedef struct {
    int16_t *ring;          // FS_STREAM_FRAMES + FS_STREAM_GUARD frames
    const int16_t *data;    // Font sample data
    uint32_t filled;        // Virtual frames written so far
    uint32_t src;           // Next font frame to copy
    uint32_t end;           // One past the last font frame
    uint32_t loop_start;
    uint32_t loop_end;
    uint32_t end_vpos;      // Virtual position of the sample end (when src_done)
    bool looping;
    bool src_done;
} FsStream;

void fs_stream_start(FsStream *s, const int16_t *data, uint32_t start, uint32_t end,
                     uint32_t loop_start, uint32_t loop_end, bool looping);

// Stop looping (SF2 loop mode 3 on note release). Already buffered frames
// still play, then the sample continues to its end.
static inline void fs_stream_stop_looping(FsStream *s) {
    s->looping = false;
}

// Fill the ring so frames [playhead - 1, playhead - 1 + frames) are
// available (frames <= FS_STREAM_FRAMES).
void fs_stream_fill(FsStream *s, uint32_t playhead, uint32_t frames);

// Frames available from playhead - 1 onwards
static inline uint32_t fs_stream_available(const FsStream *s, uint32_t playhead) {
    return s->filled - (playhead - 1);
}

static inline int16_t fs_stream_frame(const FsStream *s, uint32_t vpos) {
    return s->ring[vpos & FS_STREAM_MASK];
}

// True once the playhead has passed the end of a non-looping sample
static inline bool fs_stream_finished(const FsStream *s, uint32_t playhead) {
    return s->src_done && playhead >= s->end_vpos;
}
