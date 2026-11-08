#pragma once
//
// Standard MIDI File (format 0 and 1) player working on a file in memory.
// Tracks are merged on the fly; nothing is copied or allocated.
//
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifndef FS_SMF_MAX_TRACKS
#define FS_SMF_MAX_TRACKS 64
#endif

typedef struct {
    const uint8_t *pos;
    const uint8_t *end;
    uint32_t next_tick;     // Absolute tick of the next event
    uint8_t running_status;
    bool done;
} FsSmfTrack;

// Receives channel messages (status + 1 or 2 data bytes) and complete
// SysEx messages (starting 0xF0, including the terminating 0xF7).
typedef void (*FsSmfCallback)(void *ctx, const uint8_t *msg, uint32_t len);

typedef struct {
    FsSmfTrack tracks[FS_SMF_MAX_TRACKS];
    uint32_t track_count;
    uint16_t format;
    uint16_t division;      // Ticks per quarter note (SMPTE not supported)
    uint32_t tempo;         // Microseconds per quarter note
    uint32_t tick;          // Current tick
    double tick_frames;     // Frames at the current tick
    double frames_per_tick;
    double now;             // Current time in frames
    uint32_t sample_rate;
    bool finished;
} FsSmf;

// Returns false if the data is not a usable MIDI file.
bool fs_smf_init(FsSmf *smf, const uint8_t *data, size_t size, uint32_t sample_rate);

// Dispatch every event due within the next `frames` frames, then advance
// time. Returns false once all tracks have ended.
bool fs_smf_advance(FsSmf *smf, uint32_t frames, FsSmfCallback cb, void *ctx);
