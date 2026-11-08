#include <string.h>
#include "fs_stream.h"

void FS_RAM_FUNC(fs_stream_start)(FsStream *s, const int16_t *data, uint32_t start, uint32_t end,
                                  uint32_t loop_start, uint32_t loop_end, bool looping) {
    s->data = data;
    s->src = start;
    s->end = end;
    s->loop_start = loop_start;
    s->loop_end = loop_end;
    s->looping = looping && loop_end > loop_start;
    s->src_done = false;
    s->end_vpos = 0;
    s->ring[0] = 0; // Virtual frame 0
    s->ring[FS_STREAM_FRAMES] = 0;
    s->filled = 1;
}

void FS_RAM_FUNC(fs_stream_fill)(FsStream *s, uint32_t playhead, uint32_t frames) {
    if (frames > FS_STREAM_FRAMES) frames = FS_STREAM_FRAMES;
    const uint32_t target = playhead - 1 + frames;
    if ((int32_t)(target - s->filled) <= 0) return;
    while ((int32_t)(target - s->filled) > 0) {
        const uint32_t ring_pos = s->filled & FS_STREAM_MASK;
        uint32_t chunk = target - s->filled;
        const uint32_t to_wrap = FS_STREAM_FRAMES - ring_pos;
        if (chunk > to_wrap) chunk = to_wrap;

        if (s->src_done) {
            // Silence after the end. Only a few frames are ever read, but
            // filling keeps the logic simple and the voice ends soon.
            memset(&s->ring[ring_pos], 0, chunk * sizeof(int16_t));
            s->filled += chunk;
            continue;
        }

        const uint32_t run_end = s->looping ? s->loop_end : s->end;
        // If looping was switched off after src passed the loop end there is
        // nothing special to do: run_end is now the sample end.
        const uint32_t avail = run_end - s->src;
        if (chunk > avail) chunk = avail;

        memcpy(&s->ring[ring_pos], &s->data[s->src], chunk * sizeof(int16_t));
        s->src += chunk;
        s->filled += chunk;

        if (s->src >= run_end) {
            if (s->looping) {
                s->src = s->loop_start;
            } else {
                s->src_done = true;
                s->end_vpos = s->filled;
            }
        }
    }
    // Refresh the guard copy of the first frames
    for (uint32_t k = 0; k < FS_STREAM_GUARD - 1; ++k) s->ring[FS_STREAM_FRAMES + k] = s->ring[k];
}
