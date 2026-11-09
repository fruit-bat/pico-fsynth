#pragma once
//
// fs_smf.h - Standard MIDI File (format 0 and 1) player working on a file
// in memory. Tracks are merged on the fly; nothing is copied or allocated.
//
// Time is measured in audio frames at the sample rate given to
// fs_smf_init, so the player can be driven straight from the render loop:
// call fs_smf_advance(FS_BLOCK_SIZE) before rendering each block and the
// events land on the right block. Used by fsrender on the host and by the
// firmware's built-in demo song.
//
// Supported: tempo changes, running status, SysEx (passed on), multiple
// tracks. Not supported: SMPTE time division, format 2. Truncated or
// malformed tracks end early instead of failing.
//
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifndef FS_SMF_MAX_TRACKS
#define FS_SMF_MAX_TRACKS 64
#endif

// Read position in one MTrk chunk.
typedef struct {
    const uint8_t *pos;     // Next byte to read (after the pending delta time)
    const uint8_t *end;     // End of the track data
    uint32_t next_tick;     // Absolute tick of the next event
    uint8_t running_status;
    bool done;              // End of track reached (or the data was bad)
} FsSmfTrack;

// Receives channel messages (status + 1 or 2 data bytes) and complete
// SysEx messages (starting 0xF0, including the terminating 0xF7).
// msg is only valid during the call. Meta events are not passed on.
typedef void (*FsSmfCallback)(void *ctx, const uint8_t *msg, uint32_t len);

// Player state. Large-ish (FS_SMF_MAX_TRACKS tracks), so keep it static.
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

// Prepare to play a MIDI file from the start. data must stay valid while
// playing. sample_rate sets the unit of time for fs_smf_advance. Tempo
// starts at 120 bpm until a tempo event says otherwise. Calling it again
// restarts the song. Returns false if the data is not a usable MIDI file.
bool fs_smf_init(FsSmf *smf, const uint8_t *data, size_t size, uint32_t sample_rate);

// Dispatch every event due within the next `frames` frames, then advance
// time. Returns false once all tracks have ended.
// Events are delivered in time order across all tracks (ties go to the
// lower track number), through cb with ctx passed back. Timing is to the
// call, not to the frame: an event due part way through the period is
// delivered at its start.
bool fs_smf_advance(FsSmf *smf, uint32_t frames, FsSmfCallback cb, void *ctx);
