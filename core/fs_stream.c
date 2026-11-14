//
// fs_stream.c - copies a voice's sample from the font into its ring
// buffer, unrolling loops. See fs_stream.h for the model.
//
// Both functions run on the render path (note-on and every block), so
// they are placed in RAM on the Pico.
//
#include <string.h>
#include "fs_stream.h"

// See fs_stream.h. Writes virtual frame 0 (a zero frame) and its guard
// copy, so the first cubic interpolation reads a 0 before the sample.
void FS_RAM_FUNC(fs_stream_start)(FsStream *s, const FsFont *font, uint32_t start, uint32_t end,
                                  uint32_t loop_start, uint32_t loop_end, bool looping) {
    s->font = font;
    s->src = start;
    s->end = end;
    s->loop_start = loop_start;
    s->loop_end = loop_end;
    s->looping = looping && loop_end > loop_start;
    s->src_done = false;
    s->end_vpos = 0;
    if (font->data_format == FS_DATA_RICE) {
        uint32_t off = 0;
        fs_font_seek(font, start, &off);
        fs_decoder_seek(&s->dec, font->cdata, font->data_size, off);
        s->loop_offset = 0;
        if (s->looping && !fs_font_seek(font, loop_start, &s->loop_offset)) s->looping = false;
    }
    s->ring[0] = 0; // Virtual frame 0
    s->ring[FS_STREAM_FRAMES] = 0;
    s->filled = 1;
}

// See fs_stream.h. Copies in chunks that stop at whichever comes first:
// the target, the end of the ring (wrap), or the loop end / sample end in
// the font. At the loop end it jumps back to loop_start; at the sample
// end it records end_vpos and pads with zeros from then on.
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

        if (s->font->data_format == FS_DATA_RICE) fs_decoder_read(&s->dec, &s->ring[ring_pos], chunk);
        else memcpy(&s->ring[ring_pos], &s->font->data[s->src], chunk * sizeof(int16_t));
        s->src += chunk;
        s->filled += chunk;

        if (s->src >= run_end) {
            if (s->looping) {
                s->src = s->loop_start;
                if (s->font->data_format == FS_DATA_RICE) {
                    fs_decoder_seek(&s->dec, s->font->cdata, s->font->data_size, s->loop_offset);
                }
            } else {
                s->src_done = true;
                s->end_vpos = s->filled;
            }
        }
    }
    // Refresh the guard copy of the first frames
    for (uint32_t k = 0; k < FS_STREAM_GUARD - 1; ++k) s->ring[FS_STREAM_FRAMES + k] = s->ring[k];
}
