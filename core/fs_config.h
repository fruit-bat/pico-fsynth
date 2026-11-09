#pragma once
//
// fs_config.h - compile time configuration of the synth core.
//
// Every core file includes this. Override any #ifndef'd value with -D on
// the command line (or target_compile_definitions in CMake); the firmware
// and the host tools may use different values. The rest are derived
// values and checks.
//
// Memory: the large per-voice cost is the stream ring,
// (FS_STREAM_FRAMES + 4) * 2 bytes per voice slot (FS_TOTAL_VOICES slots),
// plus the reverb and chorus buffers if enabled. On the Pico everything
// lives in SRAM; see FsSynth in fs_synth.h.
//

// Maximum simultaneous voices
#ifndef FS_MAX_VOICES
#define FS_MAX_VOICES 80
#endif

// Frames rendered per block. Control-rate values (envelopes, LFOs, pitch,
// filter coefficients, modulators) are updated once per block.
#ifndef FS_BLOCK_SIZE
#define FS_BLOCK_SIZE 32
#endif

// Per-voice sample stream ring buffer in frames (power of 2).
#ifndef FS_STREAM_FRAMES
#define FS_STREAM_FRAMES 1024
#endif

// The stream ring is topped up when the next block would run short, by
// this many frames beyond what the block needs, so copies from flash are
// spread over time instead of filling the whole ring at note on.
#ifndef FS_STREAM_REFILL
#define FS_STREAM_REFILL 128
#endif

// Regions whose key/velocity ranges are indexed in RAM, so a note on does
// not scan region records in flash. Fonts with more regions still work,
// reading flash for the rest. 5 bytes each.
#ifndef FS_REGION_INDEX_SIZE
#define FS_REGION_INDEX_SIZE 4096
#endif

// Interpolation: 1 = 4-point cubic (Catmull-Rom), 0 = linear
#ifndef FS_INTERP_CUBIC
#define FS_INTERP_CUBIC 1
#endif

// Highest sample rate the effects buffers are sized for
#ifndef FS_MAX_SAMPLE_RATE
#define FS_MAX_SAMPLE_RATE 48000
#endif

// Reverb and chorus can be compiled out to save RAM and CPU
#ifndef FS_ENABLE_REVERB
#define FS_ENABLE_REVERB 1
#endif
#ifndef FS_ENABLE_CHORUS
#define FS_ENABLE_CHORUS 1
#endif

// Scale applied to the font's initialAttenuation generator (not to
// modulators). EMU hardware uses 0.4; the SF2 spec implies 1.0.
#ifndef FS_ATTENUATION_FACTOR
#define FS_ATTENUATION_FACTOR 0.4f
#endif

// Voice stealing (see steal_score in fs_synth.c): a voice whose key has
// other voices sounding (e.g. one layer of a layered preset) counts this
// much less when choosing a voice to steal.
#ifndef FS_STEAL_LAYER_FACTOR
#define FS_STEAL_LAYER_FACTOR 0.25f
#endif

// Extra voice slots used only to fade out stolen voices (a few ms each), so
// a steal does not cut a voice off. At most FS_MAX_VOICES voices are ever
// playing normally; the reserve only adds short fade-outs.
#ifndef FS_STEAL_RESERVE
#define FS_STEAL_RESERVE 8
#endif

#define FS_TOTAL_VOICES (FS_MAX_VOICES + FS_STEAL_RESERVE)

// Voice rendering can be split into this many parts, one per core
#ifndef FS_RENDER_PARTS
#define FS_RENDER_PARTS 2
#endif

// Part 0 also runs the effects and the final mix (render_end), so it is
// given this many fewer voices than the other parts when effects are on.
// Roughly (effects cycles per sample) / (voice cycles per sample).
#ifndef FS_RENDER_EFFECTS_VOICES
#define FS_RENDER_EFFECTS_VOICES 4
#endif

// Place time critical functions in RAM on the Pico. Wrap the function name
// in a definition: void FS_RAM_FUNC(name)(args) { ... }. Code in RAM does
// not stall on flash cache misses, and keeps running while flash is being
// written. A no-op on the host.
#ifdef PICO_ON_DEVICE
#include "pico.h"
#define FS_RAM_FUNC(f) __not_in_flash_func(f)
#else
#define FS_RAM_FUNC(f) f
#endif

// CPU cycle counter for profiling (the DWT counter on the M33, which the
// firmware enables on each core). Always 0 on the host, so the profiling
// counters in FsSynthStats stay at 0 there.
#ifndef FS_CYCLE_COUNT
#if defined(PICO_ON_DEVICE) && PICO_RP2350
#include "hardware/structs/m33.h"
#define FS_CYCLE_COUNT() (m33_hw->dwt_cyccnt)
#else
#define FS_CYCLE_COUNT() 0u
#endif
#endif

#define FS_MIDI_CHANNELS 16

// A block must never consume more than half the stream ring, which bounds
// the playback rate ratio (about +3.9 octaves at equal sample rates). The
// voice clamps its playback increment (source frames per output frame) to
// this; with the defaults it is 15.
#define FS_MAX_INCREMENT ((float)((FS_STREAM_FRAMES / 2 - 4) / FS_BLOCK_SIZE))

_Static_assert((FS_STREAM_FRAMES & (FS_STREAM_FRAMES - 1)) == 0, "FS_STREAM_FRAMES must be a power of 2");
_Static_assert(FS_STREAM_FRAMES / 2 >= FS_BLOCK_SIZE * 2 + 4, "FS_STREAM_FRAMES too small for FS_BLOCK_SIZE");
_Static_assert(FS_MAX_VOICES + FS_STEAL_RESERVE <= 256, "voice indices are 8 bit");
