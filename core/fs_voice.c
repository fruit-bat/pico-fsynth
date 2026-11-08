#include <string.h>
#include "fs_voice.h"
#include "fs_math.h"

#define B FS_BLOCK_SIZE

// Shortest release, avoids clicks when a region has an instant release
#define FS_MIN_RELEASE_SEC 0.002f
// Fade time used when a voice is killed
#define FS_KILL_RELEASE_SEC 0.005f
// Below this amplitude (about -100 dB) a decaying voice is considered silent
#define FS_SILENCE_AMP 1e-5f

// ---------------------------------------------------------------------------
// Modulators
// ---------------------------------------------------------------------------

static float FS_RAM_FUNC(mod_source)(uint16_t src, const FsChannel *ch, uint8_t key, uint8_t vel, bool primary) {
    const uint32_t index = src & FS_MOD_SRC_INDEX_MASK;
    float x;
    if (src & FS_MOD_SRC_CC) {
        x = (float)ch->cc[index] * (1.0f / 128.0f);
    } else {
        switch (index) {
            case FS_MOD_GC_VELOCITY: x = (float)vel * (1.0f / 128.0f); break;
            case FS_MOD_GC_KEY: x = (float)key * (1.0f / 128.0f); break;
            case FS_MOD_GC_CHAN_PRESSURE: x = (float)ch->pressure * (1.0f / 128.0f); break;
            case FS_MOD_GC_PITCH_WHEEL: x = (float)ch->pitch_bend * (1.0f / 16384.0f); break;
            case FS_MOD_GC_PITCH_WHEEL_SENSITIVITY: x = (float)ch->pitch_bend_range * (1.0f / 127.0f); break;
            case FS_MOD_GC_POLY_PRESSURE: x = 0.0f; break; // Not supported
            default:
                // "No controller": a primary source of none disables the
                // modulator, an amount source of none means 1.
                return primary ? 0.0f : 1.0f;
        }
    }
    if (src & FS_MOD_SRC_NEGATIVE) x = 1.0f - x;

    const uint32_t type = src >> FS_MOD_SRC_TYPE_SHIFT;
    if (src & FS_MOD_SRC_BIPOLAR) {
        const float t = 2.0f * x - 1.0f;
        const float a = fabsf(t);
        float y;
        switch (type) {
            case FS_MOD_TYPE_CONCAVE: y = fs_concave(a); break;
            case FS_MOD_TYPE_CONVEX: y = fs_convex(a); break;
            case FS_MOD_TYPE_SWITCH: return t >= 0.0f ? 1.0f : -1.0f;
            default: return t;
        }
        return t < 0.0f ? -y : y;
    }
    switch (type) {
        case FS_MOD_TYPE_CONCAVE: return fs_concave(x);
        case FS_MOD_TYPE_CONVEX: return fs_convex(x);
        case FS_MOD_TYPE_SWITCH: return x >= 0.5f ? 1.0f : 0.0f;
        default: return x;
    }
}

// Sum all modulator contributions per destination generator
static void FS_RAM_FUNC(eval_mods)(const FsVoice *v, const FsChannel *ch, float sum[FS_GEN_COUNT]) {
    memset(sum, 0, sizeof(float) * FS_GEN_COUNT);
    for (uint32_t i = 0; i < v->region->mod_count; ++i) {
        const FsModulator *m = &v->mods[i];
        const float s1 = mod_source(m->src, ch, v->mkey, v->mvel, true);
        if (s1 == 0.0f) continue;
        const float s2 = mod_source(m->amt_src, ch, v->mkey, v->mvel, false);
        float val = (float)m->amount * s1 * s2;
        if (m->transform == FS_MOD_TRANSFORM_ABSOLUTE) val = fabsf(val);
        sum[m->dst] += val;
    }
}

static inline float gen(const FsVoice *v, const float sum[FS_GEN_COUNT], int g) {
    return (float)v->region->gen[g] + sum[g];
}

// Parameters that follow controller changes during the note
static void FS_RAM_FUNC(update_dynamic)(FsVoice *v, const float sum[FS_GEN_COUNT]) {
    v->pitch_mod = sum[FS_GEN_PITCH] + 100.0f * sum[FS_GEN_COARSE_TUNE] + sum[FS_GEN_FINE_TUNE];
    v->vib_lfo_to_pitch = gen(v, sum, FS_GEN_VIB_LFO_TO_PITCH);
    v->mod_lfo_to_pitch = gen(v, sum, FS_GEN_MOD_LFO_TO_PITCH);
    v->mod_env_to_pitch = gen(v, sum, FS_GEN_MOD_ENV_TO_PITCH);
    v->fc = gen(v, sum, FS_GEN_INITIAL_FILTER_FC);
    v->q = fs_clampf(gen(v, sum, FS_GEN_INITIAL_FILTER_Q), 0.0f, 960.0f);
    v->mod_lfo_to_fc = gen(v, sum, FS_GEN_MOD_LFO_TO_FILTER_FC);
    v->mod_env_to_fc = gen(v, sum, FS_GEN_MOD_ENV_TO_FILTER_FC);
    // The EMU scale factor applies to the font's own value only, so that
    // modulators (velocity, CC7, CC11) keep their full 96 dB range.
    v->attenuation = fs_clampf((float)v->region->gen[FS_GEN_INITIAL_ATTENUATION] * FS_ATTENUATION_FACTOR +
                               sum[FS_GEN_INITIAL_ATTENUATION], 0.0f, 1440.0f);
    v->mod_lfo_to_vol = gen(v, sum, FS_GEN_MOD_LFO_TO_VOLUME);
    v->pan = fs_clampf(gen(v, sum, FS_GEN_PAN), -500.0f, 500.0f);
    v->reverb_send = fs_clampf(gen(v, sum, FS_GEN_REVERB_EFFECTS_SEND), 0.0f, 1000.0f) * 0.001f;
    v->chorus_send = fs_clampf(gen(v, sum, FS_GEN_CHORUS_EFFECTS_SEND), 0.0f, 1000.0f) * 0.001f;
    // Constant power pan
    v->pan_l = sqrtf(0.5f - v->pan * 0.001f);
    v->pan_r = sqrtf(0.5f + v->pan * 0.001f);
}

// ---------------------------------------------------------------------------
// Envelopes
// ---------------------------------------------------------------------------

static uint32_t FS_RAM_FUNC(tc_to_samples)(float tc, float sample_rate) {
    return (uint32_t)(fs_tc_to_sec(tc) * sample_rate + 0.5f);
}

static void FS_RAM_FUNC(env_enter)(FsEnv *e, uint8_t stage) {
    e->stage = stage;
    switch (stage) {
        case FS_ENV_DELAY:
            e->level = 0.0f;
            e->remaining = e->delay;
            break;
        case FS_ENV_ATTACK:
            e->level = 0.0f;
            e->remaining = e->attack;
            e->rate = e->attack ? 1.0f / (float)e->attack : 0.0f;
            break;
        case FS_ENV_HOLD:
            e->level = 1.0f;
            e->remaining = e->hold;
            break;
        case FS_ENV_DECAY:
            // Full scale (1 -> 0) takes the decay time; stop at sustain
            e->rate = e->decay ? 1.0f / (float)e->decay : 0.0f;
            e->remaining = e->decay && e->level > e->sustain ?
                (uint32_t)((e->level - e->sustain) * (float)e->decay + 0.5f) : 0;
            break;
        case FS_ENV_SUSTAIN:
            e->level = e->sustain;
            break;
        case FS_ENV_RELEASE:
            e->rate = e->release ? 1.0f / (float)e->release : 0.0f;
            e->remaining = e->release && e->level > 0.0f ?
                (uint32_t)(e->level * (float)e->release + 0.5f) : 0;
            break;
        case FS_ENV_DONE:
            e->level = 0.0f;
            break;
    }
}

// Advance n samples and return the level at the end
static float FS_RAM_FUNC(env_advance)(FsEnv *e, uint32_t n) {
    while (n) {
        if (e->stage == FS_ENV_SUSTAIN || e->stage == FS_ENV_DONE) break;
        const uint32_t step = e->remaining < n ? e->remaining : n;
        switch (e->stage) {
            case FS_ENV_ATTACK: e->level += e->rate * (float)step; break;
            case FS_ENV_DECAY:
            case FS_ENV_RELEASE: e->level -= e->rate * (float)step; break;
            default: break;
        }
        e->remaining -= step;
        n -= step;
        if (e->remaining == 0) {
            switch (e->stage) {
                case FS_ENV_DELAY: env_enter(e, FS_ENV_ATTACK); break;
                case FS_ENV_ATTACK: env_enter(e, FS_ENV_HOLD); break;
                case FS_ENV_HOLD: env_enter(e, FS_ENV_DECAY); break;
                case FS_ENV_DECAY: env_enter(e, FS_ENV_SUSTAIN); break;
                case FS_ENV_RELEASE: env_enter(e, FS_ENV_DONE); break;
                default: break;
            }
        }
    }
    return e->level;
}

static void FS_RAM_FUNC(env_release)(FsEnv *e, bool volume) {
    if (e->stage == FS_ENV_DONE) return;
    if (e->stage == FS_ENV_DELAY) {
        env_enter(e, FS_ENV_DONE);
        return;
    }
    if (volume && e->stage == FS_ENV_ATTACK) {
        // Attack is linear amplitude, release is in the dB domain
        const float cb = fs_amp_to_cb(e->level);
        e->level = fs_clampf(1.0f - cb / 960.0f, 0.0f, 1.0f);
    }
    env_enter(e, FS_ENV_RELEASE);
}

// Volume envelope level to linear amplitude
static inline float vol_env_amp(const FsEnv *e) {
    switch (e->stage) {
        case FS_ENV_DELAY:
        case FS_ENV_DONE: return 0.0f;
        case FS_ENV_ATTACK: return e->level;
        default: return e->level <= 0.0f ? 0.0f : fs_cb_to_amp((1.0f - e->level) * 960.0f);
    }
}

static inline float mod_env_value(const FsEnv *e) {
    return e->stage == FS_ENV_DELAY ? 0.0f : e->level;
}

// ---------------------------------------------------------------------------
// LFOs
// ---------------------------------------------------------------------------

static void FS_RAM_FUNC(lfo_init)(FsLfo *l, float delay_tc, float freq_cents, float sample_rate) {
    l->delay = tc_to_samples(delay_tc, sample_rate);
    const float hz = fs_abs_cents_to_hz(fs_clampf(freq_cents, -16000.0f, 4500.0f));
    l->step = hz * (float)B / sample_rate;
    l->phase = 0.0f;
    l->value = 0.0f;
}

// Triangle starting at 0 and rising, evaluated once per block
static inline void FS_RAM_FUNC(lfo_advance)(FsLfo *l) {
    if (l->delay > B) {
        l->delay -= B;
        return;
    }
    l->delay = 0;
    float p = l->phase + 0.25f;
    if (p >= 1.0f) p -= 1.0f;
    l->value = 1.0f - 4.0f * fabsf(p - 0.5f);
    l->phase += l->step;
    while (l->phase >= 1.0f) l->phase -= 1.0f;
}

// ---------------------------------------------------------------------------
// Filter
// ---------------------------------------------------------------------------

static void FS_RAM_FUNC(filter_update)(FsVoice *v, float fc_cents, float q_cb, float sample_rate) {
    float fc_hz = fs_abs_cents_to_hz(fs_clampf(fc_cents, 1500.0f, 13500.0f));
    const float nyq_limit = 0.45f * sample_rate;
    if (fc_hz >= nyq_limit) {
        if (q_cb <= 0.0f) {
            v->filter_on = false;
            return;
        }
        fc_hz = nyq_limit;
    }
    if (v->filter_on &&
        fabsf(fc_hz - v->filter_fc_hz) <= fc_hz * 0.002f &&
        fabsf(q_cb - v->filter_q_cb) < 1.0f) {
        return; // Close enough to the current coefficients
    }
    if (!v->filter_on) {
        v->z1 = 0.0f;
        v->z2 = 0.0f;
        v->filter_on = true;
    }
    v->filter_fc_hz = fc_hz;
    v->filter_q_cb = q_cb;

    // RBJ low pass. Q follows FluidSynth: 0 cB gives a Butterworth response
    // and the gain is reduced as resonance rises. Written to need a single
    // divide: q = 10^(dB/20), gain = 1/sqrt(q), alpha = sin(w)/(2q).
    const float q_db = q_cb * 0.1f - 3.01f;
    const float log2_q = q_db * (3.321928f / 20.0f);
    const float gain = fs_exp2f(-0.5f * log2_q);
    float sn, cs;
    fs_sincos(fc_hz * v->w_scale, &sn, &cs);
    const float alpha = sn * 0.5f * fs_exp2f(-log2_q);
    const float a0_inv = 1.0f / (1.0f + alpha);
    v->b1 = (1.0f - cs) * gain * a0_inv;
    v->b0 = v->b1 * 0.5f;
    v->b2 = v->b0;
    v->a1 = -2.0f * cs * a0_inv;
    v->a2 = (1.0f - alpha) * a0_inv;
}

// ---------------------------------------------------------------------------
// Voice lifecycle
// ---------------------------------------------------------------------------

void FS_RAM_FUNC(fs_voice_start)(FsVoice *v, const FsFont *font, const FsRegion *region,
                    const FsChannel *ch, uint8_t channel, uint8_t key, uint8_t vel,
                    float sample_rate, uint32_t serial) {
    const int16_t *g = region->gen;

    v->state = FS_VOICE_ON;
    v->channel = channel;
    v->key = key;
    v->vel = vel;
    v->mkey = g[FS_GEN_KEYNUM] >= 0 && g[FS_GEN_KEYNUM] <= 127 ? (uint8_t)g[FS_GEN_KEYNUM] : key;
    v->mvel = g[FS_GEN_VELOCITY] >= 0 && g[FS_GEN_VELOCITY] <= 127 ? (uint8_t)g[FS_GEN_VELOCITY] : vel;
    v->exclusive_class = region->exclusive_class;
    v->loop_mode = region->loop_mode;
    v->stolen = false;
    v->serial = serial;
    v->chan_serial = ch->serial;
    v->region = region;
    v->mods = fs_font_region_mods(font, region);

    fs_stream_start(&v->stream, font->data, region->start, region->end,
                    region->loop_start, region->loop_end, region->loop_mode != FS_LOOP_NONE);
    v->pos = 1;
    v->frac = 0;

    float sum[FS_GEN_COUNT];
    eval_mods(v, ch, sum);
    update_dynamic(v, sum);

    // Static pitch
    v->pitch_base = gen(v, sum, FS_GEN_SCALE_TUNING) * ((float)v->mkey - (float)region->root_key) +
                    100.0f * (float)g[FS_GEN_COARSE_TUNE] + (float)g[FS_GEN_FINE_TUNE] +
                    (float)region->pitch_correction;
    v->rate_ratio = (float)region->sample_rate / sample_rate;

    // Envelopes, with keynum scaling of hold and decay
    const float key_offset = 60.0f - (float)v->mkey;
    FsEnv *e = &v->vol_env;
    e->delay = tc_to_samples(gen(v, sum, FS_GEN_DELAY_VOL_ENV), sample_rate);
    e->attack = tc_to_samples(gen(v, sum, FS_GEN_ATTACK_VOL_ENV), sample_rate);
    e->hold = tc_to_samples(gen(v, sum, FS_GEN_HOLD_VOL_ENV) + gen(v, sum, FS_GEN_KEYNUM_TO_VOL_ENV_HOLD) * key_offset, sample_rate);
    e->decay = tc_to_samples(gen(v, sum, FS_GEN_DECAY_VOL_ENV) + gen(v, sum, FS_GEN_KEYNUM_TO_VOL_ENV_DECAY) * key_offset, sample_rate);
    e->sustain = 1.0f - fs_clampf(gen(v, sum, FS_GEN_SUSTAIN_VOL_ENV), 0.0f, 1440.0f) / 960.0f;
    if (e->sustain < 0.0f) e->sustain = 0.0f;
    e->release = tc_to_samples(gen(v, sum, FS_GEN_RELEASE_VOL_ENV), sample_rate);
    const uint32_t min_release = (uint32_t)(FS_MIN_RELEASE_SEC * sample_rate);
    if (e->release < min_release) e->release = min_release;
    env_enter(e, FS_ENV_DELAY);

    e = &v->mod_env;
    e->delay = tc_to_samples(gen(v, sum, FS_GEN_DELAY_MOD_ENV), sample_rate);
    e->attack = tc_to_samples(gen(v, sum, FS_GEN_ATTACK_MOD_ENV), sample_rate);
    e->hold = tc_to_samples(gen(v, sum, FS_GEN_HOLD_MOD_ENV) + gen(v, sum, FS_GEN_KEYNUM_TO_MOD_ENV_HOLD) * key_offset, sample_rate);
    e->decay = tc_to_samples(gen(v, sum, FS_GEN_DECAY_MOD_ENV) + gen(v, sum, FS_GEN_KEYNUM_TO_MOD_ENV_DECAY) * key_offset, sample_rate);
    e->sustain = 1.0f - fs_clampf(gen(v, sum, FS_GEN_SUSTAIN_MOD_ENV), 0.0f, 1000.0f) / 1000.0f;
    e->release = tc_to_samples(gen(v, sum, FS_GEN_RELEASE_MOD_ENV), sample_rate);
    env_enter(e, FS_ENV_DELAY);

    lfo_init(&v->mod_lfo, gen(v, sum, FS_GEN_DELAY_MOD_LFO), gen(v, sum, FS_GEN_FREQ_MOD_LFO), sample_rate);
    lfo_init(&v->vib_lfo, gen(v, sum, FS_GEN_DELAY_VIB_LFO), gen(v, sum, FS_GEN_FREQ_VIB_LFO), sample_rate);

    v->filter_on = false;
    v->w_scale = 6.2831853f / sample_rate;
    v->filter_fc_hz = 0.0f;
    v->filter_q_cb = 0.0f;
    v->amp = 0.0f;
    v->last_l = 0.0f;
    v->last_r = 0.0f;
}

void FS_RAM_FUNC(fs_voice_release)(FsVoice *v) {
    if (v->state == FS_VOICE_RELEASED || v->state == FS_VOICE_FREE) return;
    v->state = FS_VOICE_RELEASED;
    env_release(&v->vol_env, true);
    env_release(&v->mod_env, false);
    if (v->loop_mode == FS_LOOP_UNTIL_RELEASE) fs_stream_stop_looping(&v->stream);
}

void FS_RAM_FUNC(fs_voice_kill)(FsVoice *v, float sample_rate) {
    if (v->state == FS_VOICE_FREE) return;
    const uint32_t fast = (uint32_t)(FS_KILL_RELEASE_SEC * sample_rate);
    if (v->vol_env.release > fast) v->vol_env.release = fast;
    if (v->state == FS_VOICE_RELEASED) {
        // Already releasing: restart the release from the current level, faster
        if (v->vol_env.stage == FS_ENV_RELEASE) env_enter(&v->vol_env, FS_ENV_RELEASE);
        return;
    }
    v->state = FS_VOICE_RELEASED;
    env_release(&v->vol_env, true);
    env_release(&v->mod_env, false);
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

// Inner loop: interpolate from the stream ring, optionally filter, apply the
// amplitude ramp and mix into the left and right buses and, with `rev` /
// `cho`, the effect send buses. Inlined for each combination so the tests
// are outside the loop. Returns the last sample.
//
// Measured on the RP2350 (firmware/bench.c): mixing the sends here instead
// of in separate passes over a buffer saves about 20 cycles per sample, and
// plain multiply + add is faster than fused multiply-add (VFMA).
static inline __attribute__((always_inline)) float render_samples(
    FsVoice *v, float *restrict out_l, float *restrict out_r, float *restrict out_rv, float *restrict out_ch,
    float gl, float gr, float grv, float gch,
    uint32_t inc_int, uint32_t inc_frac, float amp, float damp, bool filter, bool rev, bool cho) {

    const int16_t *ring = v->stream.ring;
    uint32_t pos = v->pos;
    uint32_t frac = v->frac;
    const float b0 = v->b0, b1 = v->b1, b2 = v->b2, a1 = v->a1, a2 = v->a2;
    float z1 = v->z1, z2 = v->z2;
    float x = 0.0f;

    for (int i = 0; i < B; ++i) {
        const float f = fs_frac32_to_float(frac);
#if FS_INTERP_CUBIC
        // Guard frames after the ring end make the 4 frames contiguous
        const int16_t *p = ring + ((pos - 1) & FS_STREAM_MASK);
        const float s0 = (float)p[0];
        const float s1 = (float)p[1];
        const float s2 = (float)p[2];
        const float s3 = (float)p[3];
        const float c1 = 0.5f * (s2 - s0);
        const float c2 = s0 - 2.5f * s1 + 2.0f * s2 - 0.5f * s3;
        const float c3 = 0.5f * (s3 - s0) + 1.5f * (s1 - s2);
        float s = ((c3 * f + c2) * f + c1) * f + s1;
#else
        const int16_t *p = ring + (pos & FS_STREAM_MASK);
        const float s1 = (float)p[0];
        const float s2 = (float)p[1];
        float s = s1 + f * (s2 - s1);
#endif
        if (filter) {
            const float y = b0 * s + z1;
            z1 = b1 * s - a1 * y + z2;
            z2 = b2 * s - a2 * y;
            s = y;
        }
        amp += damp;
        x = s * amp;
        out_l[i] += x * gl;
        out_r[i] += x * gr;
        if (rev) out_rv[i] += x * grv;
        if (cho) out_ch[i] += x * gch;

        const uint32_t nf = frac + inc_frac;
        pos += inc_int + (nf < frac);
        frac = nf;
    }

    v->pos = pos;
    v->frac = frac;
    if (filter) {
        v->z1 = z1;
        v->z2 = z2;
    }
    return x;
}

bool FS_RAM_FUNC(fs_voice_render)(FsVoice *v, const FsChannel *ch, const FsBuses *buses, float sample_rate) {
    if (v->chan_serial != ch->serial) {
        float sum[FS_GEN_COUNT];
        eval_mods(v, ch, sum);
        update_dynamic(v, sum);
        v->chan_serial = ch->serial;
    }

    // Control rate updates
    lfo_advance(&v->mod_lfo);
    lfo_advance(&v->vib_lfo);
    const float mod_env = mod_env_value(&v->mod_env);
    env_advance(&v->mod_env, B);
    env_advance(&v->vol_env, B);

    const float mod_lfo = v->mod_lfo.value;
    const float vib_lfo = v->vib_lfo.value;

    // Pitch -> playback increment (32.32 fixed point)
    const float cents = v->pitch_base + v->pitch_mod +
                        vib_lfo * v->vib_lfo_to_pitch +
                        mod_lfo * v->mod_lfo_to_pitch +
                        mod_env * v->mod_env_to_pitch;
    float inc = v->rate_ratio * fs_cents_to_ratio(cents);
    if (inc > FS_MAX_INCREMENT) inc = FS_MAX_INCREMENT;
    const uint32_t inc_int = (uint32_t)inc;
    const uint32_t inc_frac = (uint32_t)((inc - (float)inc_int) * 4294967296.0f);

    // Filter
    filter_update(v, v->fc + mod_lfo * v->mod_lfo_to_fc + mod_env * v->mod_env_to_fc, v->q, sample_rate);

    // Amplitude target for the end of this block
    const float env_amp = vol_env_amp(&v->vol_env);
    float atten = v->attenuation - mod_lfo * v->mod_lfo_to_vol;
    if (atten < 0.0f) atten = 0.0f;
    const float target = env_amp * fs_cb_to_amp(atten);

    // Make sure the frames this block reads, pos - 1 to the last position
    // + 2, are in the ring
    const uint32_t need = inc_int * B + (uint32_t)(((uint64_t)inc_frac * B + v->frac) >> 32) + 4;
    if (fs_stream_available(&v->stream, v->pos) < need) {
        const uint32_t t0 = FS_CYCLE_COUNT();
        fs_stream_fill(&v->stream, v->pos, need + FS_STREAM_REFILL);
        *buses->fill_cycles += FS_CYCLE_COUNT() - t0;
    }

    // Interpolate, filter, apply the amplitude ramp and mix in one pass
    const float start_amp = v->amp * (1.0f / 32768.0f);
    const float damp = (target - v->amp) * (1.0f / (32768.0f * (float)B));
    v->amp = target;
    const float rs = buses->reverb ? v->reverb_send : 0.0f;
    const float cs = buses->chorus ? v->chorus_send : 0.0f;
    const float gl = v->pan_l, gr = v->pan_r;
    float last;
    const uint32_t loop_t0 = FS_CYCLE_COUNT();
    float *l = buses->left, *r = buses->right, *rv = buses->reverb, *cb = buses->chorus;
#define FS_RENDER(filter, rev, cho) \
    render_samples(v, l, r, rv, cb, gl, gr, rs, cs, inc_int, inc_frac, start_amp, damp, filter, rev, cho)
    const bool use_rev = rs > 0.0f, use_cho = cs > 0.0f;
    if (v->filter_on) {
        if (use_rev) last = use_cho ? FS_RENDER(true, true, true) : FS_RENDER(true, true, false);
        else last = use_cho ? FS_RENDER(true, false, true) : FS_RENDER(true, false, false);
    } else {
        if (use_rev) last = use_cho ? FS_RENDER(false, true, true) : FS_RENDER(false, true, false);
        else last = use_cho ? FS_RENDER(false, false, true) : FS_RENDER(false, false, false);
    }
#undef FS_RENDER
    *buses->loop_cycles += FS_CYCLE_COUNT() - loop_t0;
    v->last_l = last * gl;
    v->last_r = last * gr;

    // Finished?
    if (v->vol_env.stage == FS_ENV_DONE) return false;
    if (fs_stream_finished(&v->stream, v->pos)) return false;
    if (v->vol_env.stage >= FS_ENV_DECAY && target < FS_SILENCE_AMP && env_amp < FS_SILENCE_AMP) return false;
    return true;
}
