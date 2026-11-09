#pragma once
//
// fs_synth.h - the synthesizer: 16 MIDI channels, a shared pool of voices,
// mixing and effects. Feed it MIDI messages and pull blocks of stereo
// audio. This is the core's public API.
//
// Typical use:
//   fs_font_init(&font, blob, size, true, false);
//   fs_synth_init(&synth, &font, 44100);
//   loop:
//     fs_synth_midi(&synth, msg, len);      // any number of messages
//     fs_synth_render(&synth, out);         // FS_BLOCK_SIZE stereo frames
//
// Threading: the synth is not thread safe. All calls must come from one
// thread (on the Pico, the render core), except fs_synth_render_part,
// which is designed to run concurrently on several cores (see below).
// MIDI is applied between blocks, so its timing is quantised to the block
// size (32 frames = 0.7 ms at 44.1 kHz).
//
// Memory: FsSynth is large (mainly the per-voice stream rings and the
// reverb), so make it static, never a local.
//
// Output level: voices mix at int16 sample scale; the master gain (default
// 0.2) and master volume scale the mix down to full scale = 1.0. Many loud
// voices can still clip; stats.clipped counts it.
//
#include <stdint.h>
#include <stdbool.h>
#include "fs_config.h"
#include "fs_font.h"
#include "fs_voice.h"
#include "fs_effects.h"

// Counters since fs_synth_init, for status reports and tuning. Plain
// fields: the caller may read or reset them between blocks.
typedef struct {
    uint32_t blocks;            // Blocks rendered
    uint32_t notes;             // Note-ons that found a preset
    uint32_t voices_started;
    uint32_t voices_stolen;
    uint32_t voices_cut;        // Steals that had to cut a voice (no spare slot to fade it)
    uint32_t stolen_by_channel[16];  // Steals of held (not releasing) voices per channel
    uint32_t notes_dropped;     // No preset/region for the note
    uint32_t peak_voices;       // Most voices active at once, including fading ones
    uint32_t clipped;           // Output samples that clipped
    float peak;                 // Largest output sample magnitude (1.0 = full scale); reset by the user
    // Profiling, per render part, CPU cycles (device only; 0 on the host)
    uint32_t part_cycles[FS_RENDER_PARTS];  // In render_part, including effects for part 0
    uint32_t loop_cycles[FS_RENDER_PARTS];  // In the voice sample loops
    uint32_t fill_cycles[FS_RENDER_PARTS];  // Copying samples into the stream rings
    uint32_t voice_blocks[FS_RENDER_PARTS]; // Voice blocks rendered
    uint32_t start_cycles;      // In voice_alloc + fs_voice_start
    uint32_t start_count;
} FsSynthStats;

// The whole synthesizer state. Treat it as opaque except stats and the
// documented setters; fields are public only so it can be allocated
// statically.
typedef struct {
    const FsFont *font;
    float sample_rate;
    float gain;                 // Master gain, see fs_synth_set_gain
    float master_volume;        // GM Master Volume SysEx, 0..1 (default 1)
    bool reverb_enabled;
    bool chorus_enabled;

    FsChannel channels[FS_MIDI_CHANNELS];
    const FsPreset *presets[FS_MIDI_CHANNELS];  // Current preset per channel, NULL if none

    FsVoice voices[FS_TOTAL_VOICES];
    // Indices into voices[] of the voices in use, in no particular order
    // (freeing swaps the last entry into the gap). Rendering walks this
    // list, so free voices cost nothing.
    uint8_t active[FS_TOTAL_VOICES];
    uint32_t active_count;
    uint32_t dying_count;               // Active voices fading out after being stolen

    // Declick: when a voice has to be cut off, its last output value is
    // added here and decays quickly, so the output has no step
    float declick_l;
    float declick_r;
    float declick_decay;                // Per-sample decay factor (1 ms time constant)
    uint32_t serial;                    // Last voice serial handed out

    // Key and velocity ranges and exclusive class of the font's regions,
    // in RAM (see FS_REGION_INDEX_SIZE)
    struct { uint8_t key_lo, key_hi, vel_lo, vel_hi, exclusive_class; } region_index[FS_REGION_INDEX_SIZE];

    // Active voices per channel and key, for the voice stealing policy
    uint8_t key_voices[FS_MIDI_CHANNELS][128];

    // Sample stream ring of each voice slot (FsStream.ring points here).
    // By far the largest member.
    int16_t rings[FS_TOTAL_VOICES][FS_STREAM_FRAMES + FS_STREAM_GUARD];

    // One set of mix buses per render part (core), summed in render_end
    float bus_l[FS_RENDER_PARTS][FS_BLOCK_SIZE];
    float bus_r[FS_RENDER_PARTS][FS_BLOCK_SIZE];
    float bus_reverb[FS_RENDER_PARTS][FS_BLOCK_SIZE];
    float bus_chorus[FS_RENDER_PARTS][FS_BLOCK_SIZE];
    uint8_t finished[FS_TOTAL_VOICES];  // Per active index, set while rendering
    // Effect sends summed over all parts. The effects process them one block
    // later, in part 0, so they run alongside voice rendering on other cores.
    float send_reverb[FS_BLOCK_SIZE];
    float send_chorus[FS_BLOCK_SIZE];

#if FS_ENABLE_REVERB
    FsReverb reverb;
#endif
#if FS_ENABLE_CHORUS
    FsChorus chorus;
#endif

    FsEq eq;                    // Master EQ (GS SysEx 40 02 0x)
    uint8_t eq_gs[4];           // GS EQ parameters: low freq, low gain, high freq, high gain

    FsSynthStats stats;
} FsSynth;

// Set up the synth for a font (which must stay valid) at a sample rate of
// at most FS_MAX_SAMPLE_RATE. All channels get their GM defaults, no voices
// play, effects are on, EQ flat, gain 0.2, master volume full. Copies the
// region key/velocity ranges into RAM (region_index). Call once; use
// fs_synth_reset to return to defaults later.
void fs_synth_init(FsSynth *s, const FsFont *font, uint32_t sample_rate);

// Master gain (linear). Default 0.2, as FluidSynth.
static inline void fs_synth_set_gain(FsSynth *s, float gain) { s->gain = gain; }

// Master volume, as set by the GM Master Volume SysEx (F0 7F dev 04 01 lsb msb
// F7): 0..16383, linear amplitude, 16383 (the default) = full. Multiplies the
// gain. Not changed by a GM/GS reset.
static inline void fs_synth_set_master_volume(FsSynth *s, uint16_t value) {
    s->master_volume = (float)(value > 16383 ? 16383 : value) * (1.0f / 16383.0f);
}

// Master EQ, as the Roland GS master EQ: low shelf 200 or 400 Hz, high shelf
// 3 or 6 kHz, gains -12..+12 dB. Flat by default; not changed by resets.
void fs_synth_set_eq(FsSynth *s, float low_hz, float low_db, float high_hz, float high_db);
// GS EQ parameter (address 40 02 0p): p = 0 low freq (0/1), 1 low gain
// (0x34..0x4c, 0x40 = 0 dB), 2 high freq (0/1), 3 high gain
void fs_synth_set_eq_gs(FsSynth *s, uint8_t param, uint8_t value);

// Switch effects at runtime (when compiled in). Default on.
static inline void fs_synth_set_effects(FsSynth *s, bool reverb, bool chorus) {
    s->reverb_enabled = reverb;
    s->chorus_enabled = chorus;
}

// Effect settings (no effect if compiled out).
// Reverb: room size 0..1, damping 0..1 (high frequency loss), width 0..1, level 0..1
void fs_synth_set_reverb(FsSynth *s, float room, float damp, float width, float level);
// Chorus: LFO rate in Hz, modulation depth in ms, level 0..
// Values are clamped to rate 0.01..10 Hz, depth 0..20 ms, level 0..4.
void fs_synth_set_chorus(FsSynth *s, float rate_hz, float depth_ms, float level);

// MIDI channel voice messages. chan is 0..15; out of range channels and
// keys are ignored. These take effect at the next rendered block.
//
// Note on: starts a voice for every matching region of the channel's
// preset, stealing voices if the pool is full. Velocity 0 is a note off.
// Re-striking a sounding key releases its previous voices; a note in an
// exclusive class (e.g. open/closed hi-hat) quickly cuts the channel's
// other voices in that class.
void fs_synth_note_on(FsSynth *s, uint8_t chan, uint8_t key, uint8_t vel);
// Note off: releases the key's voices, or marks them sustained while the
// sustain pedal (CC64) is down.
void fs_synth_note_off(FsSynth *s, uint8_t chan, uint8_t key);
// Control change: stores the value for the modulators, and handles bank
// select (CC0), RPN 0 pitch bend range (CC101/100 then CC6), sustain
// (CC64), all sound off (CC120), reset all controllers (CC121) and all
// notes off (CC123-127).
void fs_synth_control_change(FsSynth *s, uint8_t chan, uint8_t cc, uint8_t value);
// Program change: selects the preset for the channel's current bank,
// falling back to bank 0 and then to preset 0:0 (drum channels to kit 0)
// if the font lacks it. Sounding notes keep their old preset.
void fs_synth_program_change(FsSynth *s, uint8_t chan, uint8_t program);
void fs_synth_pitch_bend(FsSynth *s, uint8_t chan, uint16_t value);  // 0..16383, 8192 = centre
void fs_synth_channel_pressure(FsSynth *s, uint8_t chan, uint8_t value);  // 0..127
// Fade out every voice on every channel within a few ms.
void fs_synth_all_sound_off(FsSynth *s);
void fs_synth_reset(FsSynth *s);    // GM reset: controllers, programs, sound off

// Dispatch a complete MIDI message (status byte first, no running status)
// to the functions above. Also understands these SysEx messages (msg
// starts F0, ends F7): GM System On, GM2 System On and GS Reset (all
// fs_synth_reset), GM Master Volume and GS master EQ. Others are ignored.
void fs_synth_midi(FsSynth *s, const uint8_t *msg, uint32_t len);

// Render FS_BLOCK_SIZE stereo frames as interleaved int16 (L, R, L, R...).
// Out of range samples are clipped and counted in stats.clipped.
void fs_synth_render(FsSynth *s, int16_t *out);

// Same, as float in [-1, 1] (unclipped)
void fs_synth_render_float(FsSynth *s, float *left, float *right);

// Rendering split across cores. Per block:
//   fs_synth_render_begin(s);                  owner core
//   fs_synth_render_part(s, p);                p = 0 .. FS_RENDER_PARTS-1, any core,
//                                              parts may run concurrently
//   fs_synth_render_end(s, out);               owner core, after all parts finish
// No other synth call may happen between begin and end. Each part renders a
// contiguous range of the active voices into its own buses, so parts share
// no writable state. Part 0 also runs reverb and chorus on the previous
// block's sends (one block of extra effects latency) and so gets
// FS_RENDER_EFFECTS_VOICES fewer voices when effects are on.
// fs_synth_render() does the same on one core.
void fs_synth_render_begin(FsSynth *s);
void fs_synth_render_part(FsSynth *s, uint32_t part);
void fs_synth_render_end(FsSynth *s, int16_t *out);
void fs_synth_render_end_float(FsSynth *s, float *left, float *right);

// Voices currently in use, including ones fading out after a steal.
static inline uint32_t fs_synth_active_voices(const FsSynth *s) { return s->active_count; }
