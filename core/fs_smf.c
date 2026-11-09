//
// fs_smf.c - Standard MIDI File parser and player. See fs_smf.h.
//
// Each track keeps a read pointer and the absolute tick of its next event.
// fs_smf_advance repeatedly picks the track whose next event is earliest,
// converts that tick to a frame time with the current tempo, and plays the
// event if it falls inside the requested period. Tempo changes take effect
// from the tick where they occur (tick_frames is the frame time of the
// last event, the base for the next conversion).
//
// The file is untrusted: every read is bounds checked, and a track that
// runs off its end or contains nonsense is simply marked done.
//
#include <string.h>
#include "fs_smf.h"

// Big-endian 32-bit read (MIDI files are big-endian).
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

// Big-endian 16-bit read.
static uint16_t be16(const uint8_t *p) {
    return (uint16_t)(p[0] << 8 | p[1]);
}

// Read a variable length quantity (7 bits per byte, high bit set on all
// but the last, at most 4 bytes) into *value and advance t->pos.
// Returns false on overrun or if it is longer than 4 bytes.
static bool read_vlq(FsSmfTrack *t, uint32_t *value) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        if (t->pos >= t->end) return false;
        const uint8_t b = *t->pos++;
        v = (v << 7) | (b & 0x7f);
        if (!(b & 0x80)) {
            *value = v;
            return true;
        }
    }
    return false;
}

// Read the delta time before a track's next event and add it to
// next_tick. Marks the track done if there is none.
static void track_read_delta(FsSmfTrack *t) {
    uint32_t delta;
    if (t->done || !read_vlq(t, &delta)) {
        t->done = true;
        return;
    }
    t->next_tick += delta;
}

// Recompute frames_per_tick after a tempo change:
// (microseconds per quarter note / ticks per quarter note) * frames per us.
static void update_frames_per_tick(FsSmf *smf) {
    smf->frames_per_tick = (double)smf->tempo * (double)smf->sample_rate / (1e6 * (double)smf->division);
}

// See fs_smf.h. Reads the MThd header, then walks the chunks and sets up
// a track for each MTrk (other chunk types are skipped). A truncated last
// track is used up to the end of the data.
bool fs_smf_init(FsSmf *smf, const uint8_t *data, size_t size, uint32_t sample_rate) {
    memset(smf, 0, sizeof(*smf));
    if (size < 14 || memcmp(data, "MThd", 4) != 0) return false;
    const uint32_t hlen = be32(data + 4);
    if (hlen < 6 || hlen > size - 8) return false;
    smf->format = be16(data + 8);
    const uint16_t ntracks = be16(data + 10);
    smf->division = be16(data + 12);
    if (smf->division == 0 || (smf->division & 0x8000)) return false; // SMPTE time unsupported
    if (smf->format > 1) return false;

    size_t o = 8 + hlen;
    while (o + 8 <= size && smf->track_count < ntracks && smf->track_count < FS_SMF_MAX_TRACKS) {
        const uint32_t len = be32(data + o + 4);
        const uint8_t *body = data + o + 8;
        size_t body_len = len;
        if (body_len > size - o - 8) body_len = size - o - 8; // Truncated file
        if (memcmp(data + o, "MTrk", 4) == 0) {
            FsSmfTrack *t = &smf->tracks[smf->track_count++];
            t->pos = body;
            t->end = body + body_len;
            track_read_delta(t);
        }
        if (len > size - o - 8) break;
        o += 8 + len;
    }
    if (smf->track_count == 0) return false;

    smf->sample_rate = sample_rate;
    smf->tempo = 500000; // 120 bpm
    update_frames_per_tick(smf);
    return true;
}

// Process one event of a track. The delta before it has already been read.
// Channel messages and F0 SysEx (up to 255 bytes) go to cb; tempo meta
// events update the tempo; end-of-track and anything malformed mark the
// track done. Then reads the delta time of the following event.
static void track_event(FsSmf *smf, FsSmfTrack *t, FsSmfCallback cb, void *ctx) {
    if (t->pos >= t->end) {
        t->done = true;
        return;
    }
    uint8_t status = *t->pos;
    if (status & 0x80) {
        ++t->pos;
    } else {
        status = t->running_status; // Running status
        if (!(status & 0x80)) { t->done = true; return; }
    }

    if (status == 0xff) {
        // Meta event
        if (t->pos >= t->end) { t->done = true; return; }
        const uint8_t type = *t->pos++;
        uint32_t len;
        if (!read_vlq(t, &len) || len > (uint32_t)(t->end - t->pos)) { t->done = true; return; }
        if (type == 0x51 && len == 3) {
            smf->tempo = (uint32_t)t->pos[0] << 16 | (uint32_t)t->pos[1] << 8 | t->pos[2];
            if (smf->tempo == 0) smf->tempo = 1;
            update_frames_per_tick(smf);
        } else if (type == 0x2f) {
            t->done = true;
            return;
        }
        t->pos += len;
    } else if (status == 0xf0 || status == 0xf7) {
        // SysEx: pass F0 messages on whole (F0 + body), skip F7 escapes
        uint32_t len;
        if (!read_vlq(t, &len) || len > (uint32_t)(t->end - t->pos)) { t->done = true; return; }
        if (status == 0xf0 && len <= 254) {
            uint8_t msg[256];
            msg[0] = 0xf0;
            memcpy(msg + 1, t->pos, len);
            cb(ctx, msg, len + 1);
        }
        t->pos += len;
        t->running_status = 0;
    } else if (status >= 0xf0) {
        // Other system messages do not belong in files; stop to be safe
        t->done = true;
        return;
    } else {
        t->running_status = status;
        const uint32_t n = ((status & 0xe0) == 0xc0) ? 1 : 2; // Cx and Dx have one data byte
        if ((uint32_t)(t->end - t->pos) < n) { t->done = true; return; }
        uint8_t msg[3] = { status, t->pos[0], n == 2 ? t->pos[1] : 0 };
        t->pos += n;
        cb(ctx, msg, n + 1);
    }
    track_read_delta(t);
}

// See fs_smf.h. A linear scan for the earliest track per event; fine for
// the small track counts of real files.
bool fs_smf_advance(FsSmf *smf, uint32_t frames, FsSmfCallback cb, void *ctx) {
    const double until = smf->now + frames;
    for (;;) {
        // Find the track with the earliest pending event
        FsSmfTrack *next = NULL;
        for (uint32_t i = 0; i < smf->track_count; ++i) {
            FsSmfTrack *t = &smf->tracks[i];
            if (!t->done && (!next || t->next_tick < next->next_tick)) next = t;
        }
        if (!next) {
            smf->finished = true;
            break;
        }
        // Time of that event, using the tempo in force since the last event
        const double at = smf->tick_frames + (double)(next->next_tick - smf->tick) * smf->frames_per_tick;
        if (at >= until) break;
        smf->tick_frames = at;
        smf->tick = next->next_tick;
        track_event(smf, next, cb, ctx);
    }
    smf->now = until;
    return !smf->finished;
}
