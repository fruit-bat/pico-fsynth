#include <string.h>
#include <math.h>
#include "fs_effects.h"
#include "fs_math.h"

// ---------------------------------------------------------------------------
// Reverb
// ---------------------------------------------------------------------------

static const uint16_t comb_len[FS_REVERB_COMBS] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
static const uint16_t ap_len[FS_REVERB_ALLPASSES] = { 556, 441, 341, 225 };

// Keeps the recirculating filters out of denormal territory
#define FS_ANTI_DENORMAL 1e-20f

static float *delay_init(FsDelayLine *d, float *mem, uint32_t len) {
    d->buf = mem;
    d->len = len;
    d->pos = 0;
    d->store = 0.0f;
    return mem + len;
}

void fs_reverb_init(FsReverb *r, float sample_rate) {
    memset(r->comb_mem, 0, sizeof(r->comb_mem));
    memset(r->ap_mem, 0, sizeof(r->ap_mem));
    const float scale = sample_rate / 44100.0f;
    float *m = r->comb_mem;
    for (int i = 0; i < FS_REVERB_COMBS; ++i) {
        m = delay_init(&r->comb[i], m, (uint32_t)(comb_len[i] * scale));
    }
    m = r->ap_mem;
    for (int i = 0; i < FS_REVERB_ALLPASSES; ++i) {
        m = delay_init(&r->ap_l[i], m, (uint32_t)(ap_len[i] * scale));
        m = delay_init(&r->ap_r[i], m, (uint32_t)((ap_len[i] + FS_REVERB_STEREO_SPREAD) * scale));
    }
    // Defaults chosen to be close to FluidSynth: RT60 about 3.5 s, wet about
    // -11 dB below dry at full send, similar brightness
    fs_reverb_set(r, 0.88f, 0.2f, 1.0f, 0.2f);
}

void fs_reverb_set(FsReverb *r, float room, float damp, float width, float level) {
    r->feedback = room * 0.28f + 0.7f;
    r->damp = damp * 0.4f;
    const float wet = level * 3.0f;
    r->wet1 = wet * (width * 0.5f + 0.5f);
    r->wet2 = wet * ((1.0f - width) * 0.5f);
    r->input_gain = 0.015f * 2.0f; // Mono input: both Freeverb inputs
}

// The delay lines are processed a block at a time in groups of 4, in runs
// that stop where one of the lines wraps. Within a run the line pointers
// and filter states stay in registers.

static inline uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

// Run length until the first of 4 delay lines wraps, at most n
static inline uint32_t run_len(FsDelayLine *const d[4], uint32_t n) {
    for (int k = 0; k < 4; ++k) n = min_u32(n, d[k]->len - d[k]->pos);
    return n;
}

static inline void advance(FsDelayLine *const d[4], uint32_t run) {
    for (int k = 0; k < 4; ++k) {
        d[k]->pos += run;
        if (d[k]->pos >= d[k]->len) d[k]->pos = 0;
    }
}

// 4 damped combs: acc[i] += comb outputs, input x[i]
static void FS_RAM_FUNC(comb4)(FsDelayLine *const d[4], const float *x, float *acc, uint32_t n, float fb, float damp1, float damp2) {
    float st0 = d[0]->store, st1 = d[1]->store, st2 = d[2]->store, st3 = d[3]->store;
    uint32_t i = 0;
    while (i < n) {
        const uint32_t run = run_len(d, n - i);
        float *p0 = d[0]->buf + d[0]->pos, *p1 = d[1]->buf + d[1]->pos;
        float *p2 = d[2]->buf + d[2]->pos, *p3 = d[3]->buf + d[3]->pos;
        for (uint32_t k = 0; k < run; ++k) {
            const float in = x[i + k];
            const float y0 = p0[k], y1 = p1[k], y2 = p2[k], y3 = p3[k];
            st0 = y0 * damp2 + st0 * damp1;
            st1 = y1 * damp2 + st1 * damp1;
            st2 = y2 * damp2 + st2 * damp1;
            st3 = y3 * damp2 + st3 * damp1;
            p0[k] = in + st0 * fb;
            p1[k] = in + st1 * fb;
            p2[k] = in + st2 * fb;
            p3[k] = in + st3 * fb;
            acc[i + k] = acc[i + k] + y0 + y1 + y2 + y3;
        }
        advance(d, run);
        i += run;
    }
    d[0]->store = st0; d[1]->store = st1; d[2]->store = st2; d[3]->store = st3;
}

// 4 allpasses in series, in place
static void FS_RAM_FUNC(allpass4)(FsDelayLine *const d[4], float *x, uint32_t n) {
    uint32_t i = 0;
    while (i < n) {
        const uint32_t run = run_len(d, n - i);
        float *p0 = d[0]->buf + d[0]->pos, *p1 = d[1]->buf + d[1]->pos;
        float *p2 = d[2]->buf + d[2]->pos, *p3 = d[3]->buf + d[3]->pos;
        for (uint32_t k = 0; k < run; ++k) {
            float v = x[i + k], b;
            b = p0[k]; p0[k] = v + b * 0.5f; v = b - v;
            b = p1[k]; p1[k] = v + b * 0.5f; v = b - v;
            b = p2[k]; p2[k] = v + b * 0.5f; v = b - v;
            b = p3[k]; p3[k] = v + b * 0.5f; v = b - v;
            x[i + k] = v;
        }
        advance(d, run);
        i += run;
    }
}

_Static_assert(FS_REVERB_COMBS == 8 && FS_REVERB_ALLPASSES == 4, "reverb is processed in groups of 4");

void FS_RAM_FUNC(fs_reverb_process)(FsReverb *r, const float *in, float *left, float *right, uint32_t n) {
    float x[FS_BLOCK_SIZE], l[FS_BLOCK_SIZE], rr[FS_BLOCK_SIZE];
    if (n > FS_BLOCK_SIZE) n = FS_BLOCK_SIZE;
    for (uint32_t i = 0; i < n; ++i) {
        x[i] = in[i] * r->input_gain + FS_ANTI_DENORMAL;
        l[i] = 0.0f;
    }
    FsDelayLine *const c0[4] = { &r->comb[0], &r->comb[1], &r->comb[2], &r->comb[3] };
    FsDelayLine *const c1[4] = { &r->comb[4], &r->comb[5], &r->comb[6], &r->comb[7] };
    comb4(c0, x, l, n, r->feedback, r->damp, 1.0f - r->damp);
    comb4(c1, x, l, n, r->feedback, r->damp, 1.0f - r->damp);
    memcpy(rr, l, n * sizeof(float));
    FsDelayLine *const al[4] = { &r->ap_l[0], &r->ap_l[1], &r->ap_l[2], &r->ap_l[3] };
    FsDelayLine *const ar[4] = { &r->ap_r[0], &r->ap_r[1], &r->ap_r[2], &r->ap_r[3] };
    allpass4(al, l, n);
    allpass4(ar, rr, n);
    const float wet1 = r->wet1, wet2 = r->wet2;
    for (uint32_t i = 0; i < n; ++i) {
        left[i] += l[i] * wet1 + rr[i] * wet2;
        right[i] += rr[i] * wet1 + l[i] * wet2;
    }
}

// ---------------------------------------------------------------------------
// Chorus
// ---------------------------------------------------------------------------

void fs_chorus_init(FsChorus *c, float sample_rate) {
    memset(c->buf, 0, sizeof(c->buf));
    c->pos = 0;
    c->phase = 0.0f;
    // Defaults chosen to give about the same wet level as FluidSynth
    fs_chorus_set(c, sample_rate, 0.3f, 12.0f, 4.0f, 0.9f);
    for (int t = 0; t < FS_CHORUS_TAPS; ++t) c->delay[t] = c->base;
}

void fs_chorus_set(FsChorus *c, float sample_rate, float rate_hz, float base_ms, float depth_ms, float level) {
    c->phase_step = rate_hz * (float)FS_BLOCK_SIZE / sample_rate;
    c->base = base_ms * 0.001f * sample_rate;
    c->depth = depth_ms * 0.001f * sample_rate;
    // Keep the longest delay inside the buffer
    const float max = (float)(FS_CHORUS_BUF - FS_BLOCK_SIZE - 4);
    if (c->base + c->depth > max) c->depth = max - c->base;
    if (c->depth < 0.0f) { c->depth = 0.0f; c->base = max; }
    if (c->depth > c->base - 2.0f) c->depth = c->base - 2.0f; // Delay stays positive
    c->level = level;
}

static inline float chorus_tap(const FsChorus *c, uint32_t pos, float delay) {
    const float rp = (float)pos - delay;
    const float fl = floorf(rp);
    const uint32_t i = (uint32_t)(int32_t)fl;
    const float f = rp - fl;
    const float a = c->buf[i & (FS_CHORUS_BUF - 1)];
    const float b = c->buf[(i + 1) & (FS_CHORUS_BUF - 1)];
    return a + f * (b - a);
}

void FS_RAM_FUNC(fs_chorus_process)(FsChorus *c, const float *in, float *left, float *right, uint32_t n) {
    // New delay targets for the end of this block (sine LFO per tap)
    c->phase += c->phase_step;
    if (c->phase >= 1.0f) c->phase -= 1.0f;
    float target[FS_CHORUS_TAPS], step[FS_CHORUS_TAPS], d[FS_CHORUS_TAPS];
    for (int t = 0; t < FS_CHORUS_TAPS; ++t) {
        float p = c->phase + (float)t / (float)FS_CHORUS_TAPS;
        if (p >= 1.0f) p -= 1.0f;
        target[t] = c->base + c->depth * fs_sin_turns(p);
        step[t] = (target[t] - c->delay[t]) / (float)n;
        d[t] = c->delay[t];
    }

    // pos is kept modulo the buffer size so tap positions stay well defined
    uint32_t pos = c->pos;
    const float g = c->level;
    for (uint32_t i = 0; i < n; ++i) {
        c->buf[pos & (FS_CHORUS_BUF - 1)] = in[i];
        // Offset by the buffer size so (pos - delay) never goes negative
        const uint32_t p = pos + FS_CHORUS_BUF;
        float l = 0.0f, r = 0.0f;
        for (int t = 0; t < FS_CHORUS_TAPS; t += 2) {
            d[t] += step[t];
            d[t + 1] += step[t + 1];
            l += chorus_tap(c, p, d[t]);
            r += chorus_tap(c, p, d[t + 1]);
        }
        left[i] += l * g;
        right[i] += r * g;
        ++pos;
    }
    c->pos = pos & (FS_CHORUS_BUF - 1);
    for (int t = 0; t < FS_CHORUS_TAPS; ++t) c->delay[t] = target[t];
}

// ---------------------------------------------------------------------------
// Master EQ
// ---------------------------------------------------------------------------

// RBJ shelving filters with slope 1. Only computed when a setting changes.
static void shelf(FsBiquad *f, float sample_rate, float hz, float db, bool high) {
    const float A = powf(10.0f, db / 40.0f);
    const float w = 6.2831853f * hz / sample_rate;
    const float cs = cosf(w);
    const float alpha = sinf(w) * 0.70710678f; // sin(w)/2 * sqrt(2)
    const float k = 2.0f * sqrtf(A) * alpha;
    float b0, b1, b2, a0, a1, a2;
    if (high) {
        b0 = A * ((A + 1.0f) + (A - 1.0f) * cs + k);
        b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cs);
        b2 = A * ((A + 1.0f) + (A - 1.0f) * cs - k);
        a0 = (A + 1.0f) - (A - 1.0f) * cs + k;
        a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cs);
        a2 = (A + 1.0f) - (A - 1.0f) * cs - k;
    } else {
        b0 = A * ((A + 1.0f) - (A - 1.0f) * cs + k);
        b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cs);
        b2 = A * ((A + 1.0f) - (A - 1.0f) * cs - k);
        a0 = (A + 1.0f) + (A - 1.0f) * cs + k;
        a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cs);
        a2 = (A + 1.0f) + (A - 1.0f) * cs - k;
    }
    const float inv = 1.0f / a0;
    f->b0 = b0 * inv; f->b1 = b1 * inv; f->b2 = b2 * inv;
    f->a1 = a1 * inv; f->a2 = a2 * inv;
}

void fs_eq_init(FsEq *eq, float sample_rate) {
    memset(eq->z, 0, sizeof(eq->z));
    fs_eq_set(eq, sample_rate, 200.0f, 0.0f, 3000.0f, 0.0f);
}

void fs_eq_set(FsEq *eq, float sample_rate, float low_hz, float low_db, float high_hz, float high_db) {
    eq->low_hz = low_hz; eq->low_db = low_db;
    eq->high_hz = high_hz; eq->high_db = high_db;
    shelf(&eq->low, sample_rate, low_hz, low_db, false);
    shelf(&eq->high, sample_rate, high_hz, high_db, true);
    const bool active = low_db != 0.0f || high_db != 0.0f;
    if (active && !eq->active) memset(eq->z, 0, sizeof(eq->z));
    eq->active = active;
}

static inline float biquad(const FsBiquad *f, float x, float *z) {
    const float y = f->b0 * x + z[0];
    z[0] = f->b1 * x - f->a1 * y + z[1];
    z[1] = f->b2 * x - f->a2 * y;
    return y;
}

void FS_RAM_FUNC(fs_eq_process)(FsEq *eq, float *left, float *right, uint32_t n) {
    if (!eq->active) return;
    float *ch[2] = { left, right };
    for (int c = 0; c < 2; ++c) {
        float *x = ch[c];
        float z[4] = { eq->z[c][0], eq->z[c][1], eq->z[c][2], eq->z[c][3] };
        for (uint32_t i = 0; i < n; ++i) {
            x[i] = biquad(&eq->high, biquad(&eq->low, x[i], &z[0]), &z[2]) + FS_ANTI_DENORMAL;
        }
        for (int k = 0; k < 4; ++k) eq->z[c][k] = z[k];
    }
}
