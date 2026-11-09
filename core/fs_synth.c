//
// fs_synth.c - MIDI handling, voice allocation and stealing, and the
// block mixer. See fs_synth.h for the API.
//
// Per block (fs_synth_render or the begin/part/end split):
//   begin  clear the per-part mix buses
//   part   render a slice of the active voices into that part's buses;
//          part 0 also runs reverb and chorus on last block's sends
//   end    free finished voices, sum the parts, add the declick tail,
//          master EQ, gain, convert to the output format
//
// Voice pool: FS_MAX_VOICES voices may play normally, plus
// FS_STEAL_RESERVE slots used only to fade out stolen voices (see
// voice_alloc). The active[] list keeps the voices in use so rendering
// and MIDI handling never scan free slots.
//
// MIDI handlers and render functions are placed in RAM on the Pico;
// one-off setup (init, effect settings) stays in flash.
//
#include <string.h>
#include "fs_synth.h"
#include "fs_math.h"

#define B FS_BLOCK_SIZE
#define FS_DRUM_CHANNEL 9           // MIDI channel 10, numbered from 0
#define FS_DRUM_BANK 128            // SF2 bank of GM percussion kits
#define FS_RPN_NONE 0x3fff          // "Null" RPN: data entry is ignored

// ---------------------------------------------------------------------------
// Channels and presets
// ---------------------------------------------------------------------------

// Preset for a channel's bank and program, with GM-style fallbacks so a
// small font still plays something: drums try 128:program then 128:0;
// melodic channels try bank:program, 0:program, then 0:0. NULL only if
// none of those exist.
static const FsPreset *FS_RAM_FUNC(find_preset)(const FsFont *font, bool drums, uint16_t bank, uint8_t program) {
    const FsPreset *p;
    if (drums) {
        p = fs_font_find_preset(font, FS_DRUM_BANK, program);
        if (!p) p = fs_font_find_preset(font, FS_DRUM_BANK, 0);
        return p;
    }
    p = fs_font_find_preset(font, bank, program);
    if (!p && bank != 0) p = fs_font_find_preset(font, 0, program);
    if (!p) p = fs_font_find_preset(font, 0, 0);
    return p;
}

// Reset the controllers that CC121 resets (per the MIDI recommended
// practice: volume, pan and effect sends are kept). Does not release
// sustained notes; the caller does that.
static void FS_RAM_FUNC(channel_reset_controllers)(FsChannel *ch) {
    // Controllers reset by "reset all controllers" (CC121)
    ch->cc[1] = 0;      // Mod wheel
    ch->cc[11] = 127;   // Expression
    ch->cc[64] = 0;     // Sustain
    ch->cc[65] = 0;
    ch->cc[66] = 0;
    ch->cc[67] = 0;
    ch->pitch_bend = 8192;
    ch->pressure = 0;
    ch->rpn = FS_RPN_NONE;
    ch->serial++;
}

// Put a channel in its power-on (GM reset) state: every controller at its
// default, program 0, bank 0 (or the drum bank on channel 10), pitch bend
// range 2 semitones. Does not touch its voices.
static void FS_RAM_FUNC(channel_init)(FsSynth *s, uint8_t chan) {
    FsChannel *ch = &s->channels[chan];
    memset(ch, 0, sizeof(*ch));
    ch->drums = chan == FS_DRUM_CHANNEL;
    ch->bank = ch->drums ? FS_DRUM_BANK : 0;
    ch->program = 0;
    ch->cc[7] = 100;    // Volume
    ch->cc[10] = 64;    // Pan
    ch->cc[91] = 0;     // Reverb send (GM2 suggests 40; 0 matches FluidSynth)
    ch->cc[93] = 0;     // Chorus send
    ch->pitch_bend_range = 2;
    channel_reset_controllers(ch);
    s->presets[chan] = find_preset(s->font, ch->drums, ch->bank, ch->program);
}

// ---------------------------------------------------------------------------
// Voice pool
// ---------------------------------------------------------------------------

// Maintain key_voices, the count of live (not stolen) voices per channel
// and key, which steal_score uses to spot layered notes.
static void FS_RAM_FUNC(voice_count_add)(FsSynth *s, const FsVoice *v) {
    s->key_voices[v->channel][v->key]++;
}

static void FS_RAM_FUNC(voice_count_remove)(FsSynth *s, const FsVoice *v) {
    s->key_voices[v->channel][v->key]--;
}

// Return the voice at position active_index of the active list to the
// pool. The last active entry moves into its place, so callers looping
// over the list must go backwards (or re-check index i).
static void FS_RAM_FUNC(voice_free)(FsSynth *s, uint32_t active_index) {
    FsVoice *v = &s->voices[s->active[active_index]];
    if (v->stolen) s->dying_count--;   // Already removed from the key counts
    else voice_count_remove(s, v);
    v->state = FS_VOICE_FREE;
    v->stolen = false;
    s->active[active_index] = s->active[--s->active_count];
}

// Priority for stealing: the lowest score is stolen first.
//  - Released voices go first, then voices only held by the sustain pedal.
//  - Quieter voices before louder ones. Voices that have not rendered yet
//    count as full volume.
//  - A voice whose key has other voices sounding (layers, or a re-struck
//    note) is a better victim than the only voice of a note: losing one
//    layer of a note is less noticeable than losing the note.
// (Weighting by how busy a channel is was tried and made dense passages
// worse: a chord arrives channel by channel, so the first channel looks
// busy part way through the burst and loses whole notes.)
static float FS_RAM_FUNC(steal_score)(const FsSynth *s, const FsVoice *v) {
    float a = v->amp;
    if (v->vol_env.stage <= FS_ENV_ATTACK) a = 1.0f;
    switch (v->state) {
        case FS_VOICE_RELEASED: a *= 0.1f; break;
        case FS_VOICE_SUSTAINED: a *= 0.5f; break;
        default: break;
    }
    if (s->key_voices[v->channel][v->key] > 1) a *= FS_STEAL_LAYER_FACTOR;
    return a;
}

// Get a voice for a new note.
//
// At most FS_MAX_VOICES voices play normally. When that many are playing, the
// least important one (steal_score) is stolen: it fades out over a few ms
// in one of the FS_STEAL_RESERVE spare slots while the new note starts in a
// free slot. Only if every spare slot is busy fading is a voice cut off,
// with its last output value handed to the declicker.
//
// note_serial is the serial the note's first voice will get: voices with
// that serial or later belong to the note being started (its other layers)
// and are never stolen for it. Returns a voice for the caller to start
// with fs_voice_start (already in the active list), or NULL if nothing can
// be stolen.
static FsVoice *FS_RAM_FUNC(voice_alloc)(FsSynth *s, uint32_t note_serial) {
    if (s->active_count - s->dying_count >= FS_MAX_VOICES) {
        // Choose a victim, never a voice of the note being started or one
        // that is already fading out
        int best = -1;
        float best_score = 0.0f;
        for (uint32_t i = 0; i < s->active_count; ++i) {
            const FsVoice *v = &s->voices[s->active[i]];
            if (v->stolen || v->serial >= note_serial) continue;
            const float score = steal_score(s, v);
            if (best < 0 || score < best_score || (score == best_score && v->serial < s->voices[s->active[best]].serial)) {
                best = (int)i;
                best_score = score;
            }
        }
        if (best < 0) return NULL;
        FsVoice *victim = &s->voices[s->active[best]];
        s->stats.voices_stolen++;
        if (victim->state != FS_VOICE_RELEASED) s->stats.stolen_by_channel[victim->channel]++; // Audible steals
        voice_count_remove(s, victim);
        victim->stolen = true;
        s->dying_count++;
        fs_voice_kill(victim, s->sample_rate);
    }

    // A free slot (there is one unless all the spares are fading)
    if (s->active_count < FS_TOTAL_VOICES) {
        for (uint32_t i = 0; i < FS_TOTAL_VOICES; ++i) {
            if (s->voices[i].state == FS_VOICE_FREE) {
                s->active[s->active_count++] = (uint8_t)i;
                return &s->voices[i];
            }
        }
    }

    // No free slot: cut off the quietest fading voice and reuse it
    int best = -1;
    for (uint32_t i = 0; i < s->active_count; ++i) {
        const FsVoice *v = &s->voices[s->active[i]];
        if (v->stolen && (best < 0 || v->amp < s->voices[s->active[best]].amp)) best = (int)i;
    }
    if (best < 0) return NULL;
    FsVoice *v = &s->voices[s->active[best]];
    s->declick_l += v->last_l;
    s->declick_r += v->last_r;
    s->stats.voices_cut++;
    v->stolen = false;
    s->dying_count--;
    return v; // Stays in the active list; the caller restarts it
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

// See fs_synth.h.
void fs_synth_init(FsSynth *s, const FsFont *font, uint32_t sample_rate) {
    fs_math_init();
    memset(&s->stats, 0, sizeof(s->stats));
    s->font = font;
    s->sample_rate = (float)sample_rate;
    s->gain = 0.2f;
    s->master_volume = 1.0f;
    s->reverb_enabled = true;
    s->chorus_enabled = true;
    s->active_count = 0;
    s->dying_count = 0;
    memset(s->key_voices, 0, sizeof(s->key_voices));
    s->serial = 0;
    s->declick_l = 0.0f;
    s->declick_r = 0.0f;
    s->declick_decay = expf(-1.0f / (0.001f * s->sample_rate)); // 1 ms time constant
    for (uint32_t i = 0; i < FS_TOTAL_VOICES; ++i) {
        memset(&s->voices[i], 0, sizeof(FsVoice));
        s->voices[i].state = FS_VOICE_FREE;
        s->voices[i].stream.ring = s->rings[i];
    }
    for (uint8_t c = 0; c < FS_MIDI_CHANNELS; ++c) channel_init(s, c);
    const uint32_t indexed = font->header->region_count < FS_REGION_INDEX_SIZE ? font->header->region_count : FS_REGION_INDEX_SIZE;
    for (uint32_t i = 0; i < indexed; ++i) {
        const FsRegion *r = &font->regions[i];
        s->region_index[i].key_lo = r->key_lo;
        s->region_index[i].key_hi = r->key_hi;
        s->region_index[i].vel_lo = r->vel_lo;
        s->region_index[i].vel_hi = r->vel_hi;
        s->region_index[i].exclusive_class = r->exclusive_class;
    }
    fs_eq_init(&s->eq, s->sample_rate);
    s->eq_gs[0] = 0; s->eq_gs[1] = 0x40; s->eq_gs[2] = 0; s->eq_gs[3] = 0x40;
    memset(s->send_reverb, 0, sizeof(s->send_reverb));
    memset(s->send_chorus, 0, sizeof(s->send_chorus));
#if FS_ENABLE_REVERB
    fs_reverb_init(&s->reverb, s->sample_rate);
#endif
#if FS_ENABLE_CHORUS
    fs_chorus_init(&s->chorus, s->sample_rate);
#endif
}

// See fs_synth.h. Clamps every value to 0..1.
void fs_synth_set_reverb(FsSynth *s, float room, float damp, float width, float level) {
#if FS_ENABLE_REVERB
    fs_reverb_set(&s->reverb, fs_clampf(room, 0.0f, 1.0f), fs_clampf(damp, 0.0f, 1.0f),
                  fs_clampf(width, 0.0f, 1.0f), fs_clampf(level, 0.0f, 1.0f));
#else
    (void)s; (void)room; (void)damp; (void)width; (void)level;
#endif
}

// See fs_synth.h. Gains are clamped to +-24 dB, beyond the GS range, for
// host use (fsrender -E).
void fs_synth_set_eq(FsSynth *s, float low_hz, float low_db, float high_hz, float high_db) {
    fs_eq_set(&s->eq, s->sample_rate, low_hz, fs_clampf(low_db, -24.0f, 24.0f), high_hz, fs_clampf(high_db, -24.0f, 24.0f));
}

// See fs_synth.h. Stores the GS parameter (clamped to its legal range; it
// is what settings.c saves) and rebuilds the EQ from all four.
void fs_synth_set_eq_gs(FsSynth *s, uint8_t param, uint8_t value) {
    if (param > 3) return;
    if (param == 1 || param == 3) value = value < 0x34 ? 0x34 : value > 0x4c ? 0x4c : value;
    else value = value ? 1 : 0;
    s->eq_gs[param] = value;
    fs_synth_set_eq(s, s->eq_gs[0] ? 400.0f : 200.0f, (float)((int)s->eq_gs[1] - 0x40),
                    s->eq_gs[2] ? 6000.0f : 3000.0f, (float)((int)s->eq_gs[3] - 0x40));
}

// See fs_synth.h. The base delay is fixed at 12 ms.
void fs_synth_set_chorus(FsSynth *s, float rate_hz, float depth_ms, float level) {
#if FS_ENABLE_CHORUS
    fs_chorus_set(&s->chorus, s->sample_rate, fs_clampf(rate_hz, 0.01f, 10.0f), FS_CHORUS_DEFAULT_BASE,
                  fs_clampf(depth_ms, 0.0f, 20.0f), fs_clampf(level, 0.0f, 4.0f));
#else
    (void)s; (void)rate_hz; (void)depth_ms; (void)level;
#endif
}

// See fs_synth.h. Only voices still held (ON) are affected.
void FS_RAM_FUNC(fs_synth_note_off)(FsSynth *s, uint8_t chan, uint8_t key) {
    if (chan >= FS_MIDI_CHANNELS || key > 127) return;
    const bool sustain = s->channels[chan].cc[64] >= 64;
    for (uint32_t i = 0; i < s->active_count; ++i) {
        FsVoice *v = &s->voices[s->active[i]];
        if (v->channel != chan || v->key != key || v->state != FS_VOICE_ON) continue;
        if (sustain) v->state = FS_VOICE_SUSTAINED;
        else fs_voice_release(v);
    }
}

// See fs_synth.h. Three passes: find the matching regions (at most 32)
// and their exclusive classes; release or kill the channel's existing
// voices as needed; then allocate and start a voice per region. All the
// note's voices share note_serial as a floor, so stealing for a later
// layer never takes an earlier layer of the same note.
void FS_RAM_FUNC(fs_synth_note_on)(FsSynth *s, uint8_t chan, uint8_t key, uint8_t vel) {
    if (chan >= FS_MIDI_CHANNELS || key > 127) return;
    if (vel == 0) {
        fs_synth_note_off(s, chan, key);
        return;
    }
    if (vel > 127) vel = 127;
    const FsPreset *preset = s->presets[chan];
    if (!preset) {
        s->stats.notes_dropped++;
        return;
    }
    s->stats.notes++;
    const FsChannel *ch = &s->channels[chan];
    const uint32_t note_serial = s->serial + 1;

    // Re-striking a key releases the previous voices of that key, and a new
    // note kills voices in the same exclusive class (e.g. open hi-hat).
    // The region ranges are read from the RAM index, so only matching region
    // records are read from flash.
    uint32_t classes[4] = { 0 }; // 128-bit set of exclusive classes in this note
    uint32_t matched = 0;
    uint16_t match[32];
    for (uint32_t r = 0; r < preset->region_count; ++r) {
        const uint32_t ri = preset->region_first + r;
        uint8_t xc;
        if (ri < FS_REGION_INDEX_SIZE) {
            const uint8_t *e = &s->region_index[ri].key_lo;
            if (key < e[0] || key > e[1] || vel < e[2] || vel > e[3]) continue;
            xc = s->region_index[ri].exclusive_class;
        } else {
            const FsRegion *region = &s->font->regions[ri];
            if (!fs_region_matches(region, key, vel)) continue;
            xc = region->exclusive_class;
        }
        if (xc) classes[xc >> 5] |= 1u << (xc & 31);
        if (matched < 32) match[matched++] = (uint16_t)r;
    }
    for (uint32_t i = 0; i < s->active_count; ++i) {
        FsVoice *v = &s->voices[s->active[i]];
        if (v->channel != chan) continue;
        if (v->exclusive_class && (classes[v->exclusive_class >> 5] & (1u << (v->exclusive_class & 31)))) {
            fs_voice_kill(v, s->sample_rate);
        } else if (v->key == key && v->state != FS_VOICE_RELEASED) {
            fs_voice_release(v);
        }
    }

    bool any = false;
    for (uint32_t m = 0; m < matched; ++m) {
        const FsRegion *region = fs_font_preset_region(s->font, preset, match[m]);
        const uint32_t t0 = FS_CYCLE_COUNT();
        FsVoice *v = voice_alloc(s, note_serial);
        if (!v) break;
        fs_voice_start(v, s->font, region, ch, chan, key, vel, s->sample_rate, ++s->serial);
        s->stats.start_cycles += FS_CYCLE_COUNT() - t0;
        s->stats.start_count++;
        voice_count_add(s, v);
        s->stats.voices_started++;
        any = true;
    }
    if (!any) s->stats.notes_dropped++;
    if (s->active_count > s->stats.peak_voices) s->stats.peak_voices = s->active_count;
}

// Sustain pedal up: release the channel's voices that were held by it.
static void FS_RAM_FUNC(release_sustained)(FsSynth *s, uint8_t chan) {
    for (uint32_t i = 0; i < s->active_count; ++i) {
        FsVoice *v = &s->voices[s->active[i]];
        if (v->channel == chan && v->state == FS_VOICE_SUSTAINED) fs_voice_release(v);
    }
}

// CC123: release every voice on the channel (normal release times).
// Unlike a note-off this ignores the sustain pedal.
static void FS_RAM_FUNC(all_notes_off)(FsSynth *s, uint8_t chan) {
    for (uint32_t i = 0; i < s->active_count; ++i) {
        FsVoice *v = &s->voices[s->active[i]];
        if (v->channel == chan) fs_voice_release(v);
    }
}

// CC120: fade out every voice on the channel within a few ms.
static void FS_RAM_FUNC(channel_sound_off)(FsSynth *s, uint8_t chan) {
    for (uint32_t i = 0; i < s->active_count; ++i) {
        FsVoice *v = &s->voices[s->active[i]];
        if (v->channel == chan) fs_voice_kill(v, s->sample_rate);
    }
}

// See fs_synth.h. Every CC bumps the channel serial, so sounding voices
// pick up the new value at their next block. Bank select is ignored on
// drum channels.
void FS_RAM_FUNC(fs_synth_control_change)(FsSynth *s, uint8_t chan, uint8_t cc, uint8_t value) {
    if (chan >= FS_MIDI_CHANNELS || cc > 127) return;
    FsChannel *ch = &s->channels[chan];
    value &= 0x7f;
    ch->cc[cc] = value;
    ch->serial++;

    switch (cc) {
        case 0: // Bank select MSB
            if (!ch->drums) ch->bank = value;
            break;
        case 6: // Data entry MSB
            if (ch->rpn == 0) ch->pitch_bend_range = value;
            break;
        case 64: // Sustain pedal
            if (value < 64) release_sustained(s, chan);
            break;
        case 98: case 99: // NRPN select: data entry no longer targets an RPN
            ch->rpn = FS_RPN_NONE;
            break;
        case 100: // RPN LSB
            ch->rpn = (uint16_t)((ch->rpn & 0x3f80) | value);
            break;
        case 101: // RPN MSB
            ch->rpn = (uint16_t)((ch->rpn & 0x007f) | (value << 7));
            break;
        case 120: // All sound off
            channel_sound_off(s, chan);
            break;
        case 121: // Reset all controllers
            channel_reset_controllers(ch);
            release_sustained(s, chan);
            break;
        case 123: case 124: case 125: case 126: case 127: // All notes off (and mode changes)
            all_notes_off(s, chan);
            break;
        default:
            break;
    }
}

// See fs_synth.h. The preset is looked up here, once, not at each note.
void FS_RAM_FUNC(fs_synth_program_change)(FsSynth *s, uint8_t chan, uint8_t program) {
    if (chan >= FS_MIDI_CHANNELS) return;
    FsChannel *ch = &s->channels[chan];
    ch->program = program & 0x7f;
    s->presets[chan] = find_preset(s->font, ch->drums, ch->bank, ch->program);
}

// See fs_synth.h. Takes effect through the default pitch wheel modulator.
void FS_RAM_FUNC(fs_synth_pitch_bend)(FsSynth *s, uint8_t chan, uint16_t value) {
    if (chan >= FS_MIDI_CHANNELS) return;
    s->channels[chan].pitch_bend = value & 0x3fff;
    s->channels[chan].serial++;
}

// See fs_synth.h. Takes effect through modulators (by default, vibrato
// depth).
void FS_RAM_FUNC(fs_synth_channel_pressure)(FsSynth *s, uint8_t chan, uint8_t value) {
    if (chan >= FS_MIDI_CHANNELS) return;
    s->channels[chan].pressure = value & 0x7f;
    s->channels[chan].serial++;
}

// See fs_synth.h.
void FS_RAM_FUNC(fs_synth_all_sound_off)(FsSynth *s) {
    for (uint8_t c = 0; c < FS_MIDI_CHANNELS; ++c) channel_sound_off(s, c);
}

// See fs_synth.h. Gain, master volume, EQ and effect settings are device
// settings and survive the reset.
void FS_RAM_FUNC(fs_synth_reset)(FsSynth *s) {
    fs_synth_all_sound_off(s);
    for (uint8_t c = 0; c < FS_MIDI_CHANNELS; ++c) channel_init(s, c);
}

// See fs_synth.h. Data bytes are masked to 7 bits; missing ones read as 0.
// The SysEx device ID byte (msg[2]) is not checked, so any ID matches.
void FS_RAM_FUNC(fs_synth_midi)(FsSynth *s, const uint8_t *msg, uint32_t len) {
    if (len == 0) return;
    const uint8_t status = msg[0];
    const uint8_t chan = status & 0x0f;
    const uint8_t d1 = len > 1 ? msg[1] & 0x7f : 0;
    const uint8_t d2 = len > 2 ? msg[2] & 0x7f : 0;
    switch (status & 0xf0) {
        case 0x80: fs_synth_note_off(s, chan, d1); break;
        case 0x90: fs_synth_note_on(s, chan, d1, d2); break;
        case 0xb0: fs_synth_control_change(s, chan, d1, d2); break;
        case 0xc0: fs_synth_program_change(s, chan, d1); break;
        case 0xd0: fs_synth_channel_pressure(s, chan, d1); break;
        case 0xe0: fs_synth_pitch_bend(s, chan, (uint16_t)(d1 | (d2 << 7))); break;
        case 0xf0:
            if (status == 0xf0 && len >= 6 && msg[1] == 0x7e && msg[3] == 0x09 && (msg[4] == 0x01 || msg[4] == 0x03)) {
                fs_synth_reset(s); // GM / GM2 system on
            } else if (status == 0xf0 && len >= 10 && msg[1] == 0x41 && msg[3] == 0x42 && msg[4] == 0x12 &&
                       msg[5] == 0x40 && msg[6] == 0x00 && msg[7] == 0x7f) {
                fs_synth_reset(s); // GS reset
            } else if (status == 0xf0 && len >= 10 && msg[1] == 0x41 && msg[3] == 0x42 && msg[4] == 0x12 &&
                       msg[5] == 0x40 && msg[6] == 0x02 && msg[7] <= 0x03) {
                // GS master EQ: F0 41 dev 42 12 40 02 0p vv sum F7
                if (((0x40 + 0x02 + msg[7] + msg[8] + msg[9]) & 0x7f) == 0) fs_synth_set_eq_gs(s, msg[7], msg[8]);
            } else if (status == 0xf0 && len >= 7 && msg[1] == 0x7f && msg[3] == 0x04 && msg[4] == 0x01) {
                fs_synth_set_master_volume(s, (uint16_t)((msg[5] & 0x7f) | ((msg[6] & 0x7f) << 7)));
            }
            break;
        default: break; // Poly pressure not supported
    }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

// See fs_synth.h. Clears every part's buses.
void FS_RAM_FUNC(fs_synth_render_begin)(FsSynth *s) {
    memset(s->bus_l, 0, sizeof(s->bus_l));
    memset(s->bus_r, 0, sizeof(s->bus_r));
    memset(s->bus_reverb, 0, sizeof(s->bus_reverb));
    memset(s->bus_chorus, 0, sizeof(s->bus_chorus));
}

// Render one part of the active voices. Part p takes a contiguous range of
// the active list. Part 0 also runs the effects on the previous block's
// sends, so it gets FS_RENDER_EFFECTS_VOICES fewer voices when effects are
// on. Only touches those voices, this part's buses and their `finished`
// flags (part 0 also the effects state).
//
// The split: with handicap h = FS_RENDER_EFFECTS_VOICES (effects on) and
// N voices, each part's share is per = ceil((N + h) / parts), and the
// ranges are shifted down by h, so part 0 gets per - h voices (none if
// that is negative) and the others per (the last takes what is left).
void FS_RAM_FUNC(fs_synth_render_part)(FsSynth *s, uint32_t part) {
    const bool reverb = FS_ENABLE_REVERB && s->reverb_enabled;
    const bool chorus = FS_ENABLE_CHORUS && s->chorus_enabled;
    const uint32_t t0 = FS_CYCLE_COUNT();
    if (part == 0) {
#if FS_ENABLE_REVERB
        if (reverb) fs_reverb_process(&s->reverb, s->send_reverb, s->bus_l[0], s->bus_r[0], B);
#endif
#if FS_ENABLE_CHORUS
        if (chorus) fs_chorus_process(&s->chorus, s->send_chorus, s->bus_l[0], s->bus_r[0], B);
#endif
    }
    const FsBuses buses = {
        s->bus_l[part], s->bus_r[part],
        reverb ? s->bus_reverb[part] : NULL,
        chorus ? s->bus_chorus[part] : NULL,
        &s->stats.fill_cycles[part],
        &s->stats.loop_cycles[part],
    };
    const uint32_t count = s->active_count;
    const uint32_t handicap = (reverb || chorus) && FS_RENDER_PARTS > 1 ? FS_RENDER_EFFECTS_VOICES : 0;
    const uint32_t per = (count + handicap + FS_RENDER_PARTS - 1) / FS_RENDER_PARTS;
    const int32_t lo = (int32_t)(part * per) - (int32_t)handicap;
    const int32_t hi = (int32_t)((part + 1) * per) - (int32_t)handicap;
    const uint32_t first = lo < 0 ? 0 : (uint32_t)lo;
    const uint32_t end = hi < 0 ? 0 : hi > (int32_t)count ? count : (uint32_t)hi;
    for (uint32_t i = first; i < end; ++i) {
        FsVoice *v = &s->voices[s->active[i]];
        s->finished[i] = !fs_voice_render(v, &s->channels[v->channel], &buses, s->sample_rate);
    }
    s->stats.voice_blocks[part] += end - first;
    s->stats.part_cycles[part] += FS_CYCLE_COUNT() - t0;
}

// See fs_synth.h. Writes FS_BLOCK_SIZE frames to left[] and right[], and
// updates stats.peak and stats.blocks. The summed sends are kept for the
// effects in the next block's part 0.
void FS_RAM_FUNC(fs_synth_render_end_float)(FsSynth *s, float *left, float *right) {
    // Free finished voices. Going backwards, the voice swapped into slot i
    // by voice_free has already been checked.
    for (uint32_t i = s->active_count; i-- > 0;) {
        if (s->finished[i]) voice_free(s, i);
    }

    // Sum the parts into part 0, and the sends for the next block's effects
    for (int k = 0; k < B; ++k) {
        float l = s->bus_l[0][k], r = s->bus_r[0][k];
        float rv = s->bus_reverb[0][k], ch = s->bus_chorus[0][k];
        for (uint32_t p = 1; p < FS_RENDER_PARTS; ++p) {
            l += s->bus_l[p][k];
            r += s->bus_r[p][k];
            rv += s->bus_reverb[p][k];
            ch += s->bus_chorus[p][k];
        }
        s->bus_l[0][k] = l;
        s->bus_r[0][k] = r;
        s->send_reverb[k] = rv;
        s->send_chorus[k] = ch;
    }
    float *bus_l = s->bus_l[0], *bus_r = s->bus_r[0];

    // Declick: continue the output of voices that were cut off, decaying
    if (s->declick_l != 0.0f || s->declick_r != 0.0f) {
        float dl = s->declick_l, dr = s->declick_r;
        const float d = s->declick_decay;
        for (int k = 0; k < B; ++k) {
            bus_l[k] += dl;
            bus_r[k] += dr;
            dl *= d;
            dr *= d;
        }
        // Flush to zero once inaudible
        s->declick_l = fabsf(dl) < 1e-7f ? 0.0f : dl;
        s->declick_r = fabsf(dr) < 1e-7f ? 0.0f : dr;
    }

    fs_eq_process(&s->eq, bus_l, bus_r, B);

    const float g = s->gain * s->master_volume;
    float peak = s->stats.peak;
    for (int k = 0; k < B; ++k) {
        left[k] = bus_l[k] * g;
        right[k] = bus_r[k] * g;
        peak = fmaxf(peak, fmaxf(fabsf(left[k]), fabsf(right[k])));
    }
    s->stats.peak = peak;
    s->stats.blocks++;
}

// See fs_synth.h. All parts in turn on the calling thread.
void FS_RAM_FUNC(fs_synth_render_float)(FsSynth *s, float *left, float *right) {
    fs_synth_render_begin(s);
    for (uint32_t p = 0; p < FS_RENDER_PARTS; ++p) fs_synth_render_part(s, p);
    fs_synth_render_end_float(s, left, right);
}

// Float sample (full scale +-1.0) to int16, saturating and counting clips.
static inline int16_t to_int16(float x, uint32_t *clipped) {
    float y = x * 32768.0f;
    if (y > 32767.0f) { y = 32767.0f; ++*clipped; }
    else if (y < -32768.0f) { y = -32768.0f; ++*clipped; }
    return (int16_t)y;
}

// See fs_synth.h. render_end_float, then interleave and convert to int16.
void FS_RAM_FUNC(fs_synth_render_end)(FsSynth *s, int16_t *out) {
    float left[B], right[B];
    fs_synth_render_end_float(s, left, right);
    for (int k = 0; k < B; ++k) {
        out[2 * k] = to_int16(left[k], &s->stats.clipped);
        out[2 * k + 1] = to_int16(right[k], &s->stats.clipped);
    }
}

// See fs_synth.h. All parts in turn on the calling thread.
void FS_RAM_FUNC(fs_synth_render)(FsSynth *s, int16_t *out) {
    fs_synth_render_begin(s);
    for (uint32_t p = 0; p < FS_RENDER_PARTS; ++p) fs_synth_render_part(s, p);
    fs_synth_render_end(s, out);
}
