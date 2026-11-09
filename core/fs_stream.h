#pragma once
//
// fs_stream.h - per-voice sample streamer.
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
// Positions: a "virtual" position counts frames of that unrolled stream
// from the start of the note, and maps to ring slot (vpos & FS_STREAM_MASK).
// The ring holds the most recent FS_STREAM_FRAMES of them. The voice's
// playhead (FsVoice.pos) is a virtual position.
//
// Usage per voice: fs_stream_start at note-on, then before each block
// fs_stream_fill enough frames for the block, then read with
// fs_stream_frame or straight from s->ring.
//
#include <stdint.h>
#include <stdbool.h>
#include "fs_config.h"

#define FS_STREAM_MASK (FS_STREAM_FRAMES - 1)
// The first frames of the ring are repeated after its end so the 4 frames
// used by cubic interpolation are always contiguous: ring[(pos - 1) & mask]
// to ring[((pos - 1) & mask) + 3] with a single wrap per output sample.
#define FS_STREAM_GUARD 4

// Streamer state, one per voice. Font positions (src, end, loop_*) are
// frame indices into the font's sample data; filled and end_vpos are
// virtual positions.
typedef struct {
    int16_t *ring;          // FS_STREAM_FRAMES + FS_STREAM_GUARD frames, owned by FsSynth
    const int16_t *data;    // Font sample data
    uint32_t filled;        // Virtual frames written so far
    uint32_t src;           // Next font frame to copy
    uint32_t end;           // One past the last font frame
    uint32_t loop_start;
    uint32_t loop_end;
    uint32_t end_vpos;      // Virtual position of the sample end (when src_done)
    bool looping;           // Jump back to loop_start on reaching loop_end
    bool src_done;          // The whole sample has been copied; only silence follows
} FsStream;

// Start streaming a sample from font frame start. The ring is not filled
// yet; call fs_stream_fill before reading.
//
// data        the font's sample data (FsFont.data)
// start, end  frames to play, end exclusive
// loop_start, loop_end  loop, end exclusive; ignored unless looping
// looping     whether to loop; also forced off if the loop is empty
void fs_stream_start(FsStream *s, const int16_t *data, uint32_t start, uint32_t end,
                     uint32_t loop_start, uint32_t loop_end, bool looping);

// Stop looping (SF2 loop mode 3 on note release). Already buffered frames
// still play, then the sample continues to its end.
static inline void fs_stream_stop_looping(FsStream *s) {
    s->looping = false;
}

// Fill the ring so frames [playhead - 1, playhead - 1 + frames) are
// available (frames <= FS_STREAM_FRAMES). Does nothing if they already
// are. Copies from the font (flash on the Pico), so this is where a voice
// pays for sample reads; the voice asks for a little more than it needs
// (FS_STREAM_REFILL) so refills are spread out.
void fs_stream_fill(FsStream *s, uint32_t playhead, uint32_t frames);

// Frames available from playhead - 1 onwards
static inline uint32_t fs_stream_available(const FsStream *s, uint32_t playhead) {
    return s->filled - (playhead - 1);
}

// The frame at virtual position vpos. Only valid for positions that are
// filled and not yet overwritten (the last FS_STREAM_FRAMES).
static inline int16_t fs_stream_frame(const FsStream *s, uint32_t vpos) {
    return s->ring[vpos & FS_STREAM_MASK];
}

// True once the playhead has passed the end of a non-looping sample
static inline bool fs_stream_finished(const FsStream *s, uint32_t playhead) {
    return s->src_done && playhead >= s->end_vpos;
}
