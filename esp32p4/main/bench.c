//
// bench.c - inner loop micro benchmarks on the ESP32-P4 (the `bench`
// console command). The kernels are those of firmware/bench.c (variants of
// the voice sample loop in core/fs_voice.c, each removing or changing one
// part), plus a two-pass variant and probes of the FPU's latency and
// throughput. Timed on core 0 with the scheduler suspended; compiled with
// the synth core's options (main/CMakeLists.txt).
//
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "fs_config.h"
#include "fs_math.h"
#include "bench.h"

#define B FS_BLOCK_SIZE
#define RING 1024
#define MASK (RING - 1)
#define BLOCKS 1000

typedef struct {
    int16_t ring16[RING + 4];
    float ringf[RING + 4];
    float bus_l[B], bus_r[B], bus_rv[B], bus_ch[B], buf[B], buf2[B];
    float z1, z2;
    uint32_t pos, frac;
} BenchData;

static BenchData data;

static const float b0 = 0.0675f, b1 = 0.135f, b2 = 0.0675f, a1 = -1.143f, a2 = 0.413f;
static const uint32_t inc_int = 1, inc_frac = 0x0327a5c0; // 1.0123

typedef void (*Kernel)(BenchData *d);

#define LOOP_BEGIN                                              \
    uint32_t pos = d->pos, frac = d->frac;                      \
    float z1 = d->z1, z2 = d->z2, amp = 0.5f;                   \
    const float damp = 1e-6f, gl = 0.7f, gr = 0.7f;             \
    for (int i = 0; i < B; ++i) {                               \
        const float f = fs_frac32_to_float(frac);
#define LOOP_END                                                \
        const uint32_t nf = frac + inc_frac;                    \
        pos += inc_int + (nf < frac);                           \
        frac = nf;                                              \
    }                                                           \
    d->pos = pos; d->frac = frac; d->z1 = z1; d->z2 = z2;       \
    (void)amp; (void)damp; (void)gl; (void)gr; (void)z1; (void)z2;

#define CUBIC(s0, s1, s2, s3)                                                   \
    const float c1 = 0.5f * (s2 - s0);                                          \
    const float c2 = s0 - 2.5f * s1 + 2.0f * s2 - 0.5f * s3;                    \
    const float c3 = 0.5f * (s3 - s0) + 1.5f * (s1 - s2);                       \
    float s = ((c3 * f + c2) * f + c1) * f + s1;
#define TAPS16                                                                  \
    const int16_t *p = d->ring16 + ((pos - 1) & MASK);                          \
    const float s0 = (float)p[0], s1 = (float)p[1], s2 = (float)p[2], s3 = (float)p[3];
#define FILTER                                                                  \
    { const float y = b0 * s + z1; z1 = b1 * s - a1 * y + z2; z2 = b2 * s - a2 * y; s = y; }
#define PAN_MIX                                                                 \
    amp += damp;                                                                \
    const float x = s * amp;                                                    \
    d->bus_l[i] += x * gl;                                                      \
    d->bus_r[i] += x * gr;

// The real loop: int16 ring, cubic, filter, ramp, mix into 2 buses
static void IRAM_ATTR (k_full)(BenchData *d) { LOOP_BEGIN TAPS16 CUBIC(s0, s1, s2, s3) FILTER PAN_MIX LOOP_END }
// Without the filter
static void IRAM_ATTR (k_nofilter)(BenchData *d) { LOOP_BEGIN TAPS16 CUBIC(s0, s1, s2, s3) PAN_MIX LOOP_END }
// Linear interpolation
static void IRAM_ATTR (k_linear)(BenchData *d) {
    LOOP_BEGIN
    const int16_t *p = d->ring16 + (pos & MASK);
    const float s1 = (float)p[0], s2 = (float)p[1];
    float s = s1 + f * (s2 - s1);
    FILTER PAN_MIX LOOP_END
}
// Write to one buffer instead of mixing into two buses
static void IRAM_ATTR (k_buf)(BenchData *d) {
    LOOP_BEGIN TAPS16 CUBIC(s0, s1, s2, s3) FILTER
    amp += damp;
    d->buf[i] = s * amp;
    LOOP_END
}
// Only fetch and convert the 4 taps
static void IRAM_ATTR (k_taps)(BenchData *d) {
    LOOP_BEGIN TAPS16
    d->buf[i] = (s0 + s1) + (s2 + s3);
    LOOP_END
}
// Only the position stepping and a store
static void IRAM_ATTR (k_step)(BenchData *d) {
    LOOP_BEGIN
    d->buf[i] = f;
    LOOP_END
}
// Float ring: no int to float conversion
static void IRAM_ATTR (k_float_ring)(BenchData *d) {
    LOOP_BEGIN
    const float *p = d->ringf + ((pos - 1) & MASK);
    const float s0 = p[0], s1 = p[1], s2 = p[2], s3 = p[3];
    CUBIC(s0, s1, s2, s3) FILTER PAN_MIX LOOP_END
}
// Taps from two unaligned 32-bit loads
static void IRAM_ATTR (k_taps32)(BenchData *d) {
    LOOP_BEGIN
    const uint8_t *p = (const uint8_t *)(d->ring16 + ((pos - 1) & MASK));
    uint32_t w0, w1;
    memcpy(&w0, p, 4);
    memcpy(&w1, p + 4, 4);
    const float s0 = (float)(int16_t)w0, s1 = (float)((int32_t)w0 >> 16);
    const float s2 = (float)(int16_t)w1, s3 = (float)((int32_t)w1 >> 16);
    CUBIC(s0, s1, s2, s3) FILTER PAN_MIX LOOP_END
}
// The real loop with fused multiply-adds forced
static void IRAM_ATTR (k_fma)(BenchData *d) {
    LOOP_BEGIN TAPS16
    const float c1 = 0.5f * (s2 - s0);
    const float c2 = fmaf(-0.5f, s3, fmaf(2.0f, s2, fmaf(-2.5f, s1, s0)));
    const float c3 = fmaf(0.5f, s3 - s0, 1.5f * (s1 - s2));
    float s = fmaf(fmaf(fmaf(c3, f, c2), f, c1), f, s1);
    const float y = fmaf(b0, s, z1);
    z1 = fmaf(b1, s, fmaf(-a1, y, z2));
    z2 = fmaf(b2, s, -a2 * y);
    s = y;
    amp += damp;
    const float x = s * amp;
    d->bus_l[i] = fmaf(x, gl, d->bus_l[i]);
    d->bus_r[i] = fmaf(x, gr, d->bus_r[i]);
    LOOP_END
}


// Two passes: interpolate the block into buf, then filter and mix
static void IRAM_ATTR (k_twopass)(BenchData *d) {
    uint32_t pos = d->pos, frac = d->frac;
    float z1 = d->z1, z2 = d->z2, amp = 0.5f;
    const float damp = 1e-6f, gl = 0.7f, gr = 0.7f;
#pragma GCC unroll 4
    for (int i = 0; i < B; ++i) {
        const float f = fs_frac32_to_float(frac);
        TAPS16 CUBIC(s0, s1, s2, s3)
        d->buf[i] = s;
        const uint32_t nf = frac + inc_frac;
        pos += inc_int + (nf < frac);
        frac = nf;
    }
    for (int i = 0; i < B; ++i) {
        float s = d->buf[i];
        FILTER PAN_MIX
    }
    d->pos = pos; d->frac = frac; d->z1 = z1; d->z2 = z2;
}

// As the synth runs most voices: mixing into 4 buses (left, right, reverb
// and chorus sends)
#define MIX4                                                                    \
    amp += damp;                                                                \
    const float x = s * amp;                                                    \
    d->bus_l[i] += x * gl;                                                      \
    d->bus_r[i] += x * gr;                                                      \
    d->bus_rv[i] += x * 0.3f;                                                   \
    d->bus_ch[i] += x * 0.2f;
static void IRAM_ATTR (k_full4)(BenchData *d) { LOOP_BEGIN TAPS16 CUBIC(s0, s1, s2, s3) FILTER MIX4 LOOP_END }

static inline __attribute__((always_inline)) void pass_interp(BenchData *d, uint32_t *ppos, uint32_t *pfrac) {
    uint32_t pos = *ppos, frac = *pfrac;
#pragma GCC unroll 4
    for (int i = 0; i < B; ++i) {
        const float f = fs_frac32_to_float(frac);
        TAPS16 CUBIC(s0, s1, s2, s3)
        d->buf[i] = s;
        const uint32_t nf = frac + inc_frac;
        pos += inc_int + (nf < frac);
        frac = nf;
    }
    *ppos = pos; *pfrac = frac;
}

// Two passes with 4 buses
static void IRAM_ATTR (k_twopass4)(BenchData *d) {
    uint32_t pos = d->pos, frac = d->frac;
    float z1 = d->z1, z2 = d->z2, amp = 0.5f;
    const float damp = 1e-6f, gl = 0.7f, gr = 0.7f;
    pass_interp(d, &pos, &frac);
    for (int i = 0; i < B; ++i) {
        float s = d->buf[i];
        FILTER MIX4
    }
    d->pos = pos; d->frac = frac; d->z1 = z1; d->z2 = z2;
}

// Three passes with 4 buses: interpolate, filter (the only serial part),
// then the amplitude ramp and mix, unrolled
static void IRAM_ATTR (k_threepass4)(BenchData *d) {
    uint32_t pos = d->pos, frac = d->frac;
    float z1 = d->z1, z2 = d->z2;
    const float gl = 0.7f, gr = 0.7f;
    pass_interp(d, &pos, &frac);
    for (int i = 0; i < B; ++i) {
        float s = d->buf[i];
        FILTER
        d->buf[i] = s;
    }
    const float amp0 = 0.5f, damp = 1e-6f;
#pragma GCC unroll 4
    for (int i = 0; i < B; ++i) {
        const float x = d->buf[i] * (amp0 + damp * (float)(i + 1));
        d->bus_l[i] += x * gl;
        d->bus_r[i] += x * gr;
        d->bus_rv[i] += x * 0.3f;
        d->bus_ch[i] += x * 0.2f;
    }
    d->pos = pos; d->frac = frac; d->z1 = z1; d->z2 = z2;
}

// Three passes, filter pass also unrolled by 2 with the two halves of the
// biquad interleaved is not possible (serial), so instead: filter fused
// into the interpolation pass, mixing separate
static void IRAM_ATTR (k_interp_filter_mix4)(BenchData *d) {
    uint32_t pos = d->pos, frac = d->frac;
    float z1 = d->z1, z2 = d->z2;
    const float gl = 0.7f, gr = 0.7f;
#pragma GCC unroll 2
    for (int i = 0; i < B; ++i) {
        const float f = fs_frac32_to_float(frac);
        TAPS16 CUBIC(s0, s1, s2, s3)
        FILTER
        d->buf[i] = s;
        const uint32_t nf = frac + inc_frac;
        pos += inc_int + (nf < frac);
        frac = nf;
    }
    const float amp0 = 0.5f, damp = 1e-6f;
#pragma GCC unroll 4
    for (int i = 0; i < B; ++i) {
        const float x = d->buf[i] * (amp0 + damp * (float)(i + 1));
        d->bus_l[i] += x * gl;
        d->bus_r[i] += x * gr;
        d->bus_rv[i] += x * 0.3f;
        d->bus_ch[i] += x * 0.2f;
    }
    d->pos = pos; d->frac = frac; d->z1 = z1; d->z2 = z2;
}

// Many voices: the same kernels cycling through NV separate rings (each
// with its own playhead), as the synth does, so each block's ring is
// usually not in the L1 cache
#define NV 64
static int16_t *rings;      // NV x (RING + 4), malloc'd
static uint32_t mv_pos[NV], mv_frac[NV];
static int mv;              // Voice of the next block

static void IRAM_ATTR (k_mv_full4)(BenchData *d) {
    const int v = mv; mv = (mv + 1) % NV;
    const int16_t *ring16 = rings + v * (RING + 4);
    uint32_t pos = mv_pos[v], frac = mv_frac[v];
    float z1 = d->z1, z2 = d->z2, amp = 0.5f;
    const float damp = 1e-6f, gl = 0.7f, gr = 0.7f;
    for (int i = 0; i < B; ++i) {
        const float f = fs_frac32_to_float(frac);
        const int16_t *p = ring16 + ((pos - 1) & MASK);
        const float s0 = (float)p[0], s1 = (float)p[1], s2 = (float)p[2], s3 = (float)p[3];
        CUBIC(s0, s1, s2, s3) FILTER MIX4
        const uint32_t nf = frac + inc_frac;
        pos += inc_int + (nf < frac);
        frac = nf;
    }
    mv_pos[v] = pos; mv_frac[v] = frac; d->z1 = z1; d->z2 = z2;
}

static void IRAM_ATTR (k_mv_twopass4)(BenchData *d) {
    const int v = mv; mv = (mv + 1) % NV;
    const int16_t *ring16 = rings + v * (RING + 4);
    uint32_t pos = mv_pos[v], frac = mv_frac[v];
    float z1 = d->z1, z2 = d->z2, amp = 0.5f;
    const float damp = 1e-6f, gl = 0.7f, gr = 0.7f;
#pragma GCC unroll 4
    for (int i = 0; i < B; ++i) {
        const float f = fs_frac32_to_float(frac);
        const int16_t *p = ring16 + ((pos - 1) & MASK);
        const float s0 = (float)p[0], s1 = (float)p[1], s2 = (float)p[2], s3 = (float)p[3];
        CUBIC(s0, s1, s2, s3)
        d->buf[i] = s;
        const uint32_t nf = frac + inc_frac;
        pos += inc_int + (nf < frac);
        frac = nf;
    }
    for (int i = 0; i < B; ++i) {
        float s = d->buf[i];
        FILTER MIX4
    }
    mv_pos[v] = pos; mv_frac[v] = frac; d->z1 = z1; d->z2 = z2;
}

// Prefetch: load one frame from each cache line the block will read
// before starting, then the one-pass loop
static void IRAM_ATTR (k_mv_prefetch)(BenchData *d) {
    const int v = mv; mv = (mv + 1) % NV;
    const int16_t *ring16 = rings + v * (RING + 4);
    uint32_t pos = mv_pos[v], frac = mv_frac[v];
    {
        volatile int16_t sink;
        const uint32_t first = (pos - 1) & MASK;
        sink = ring16[first];
        sink = ring16[(first + 32) & MASK];
        sink = ring16[(first + 64) & MASK];
        (void)sink;
    }
    float z1 = d->z1, z2 = d->z2, amp = 0.5f;
    const float damp = 1e-6f, gl = 0.7f, gr = 0.7f;
#pragma GCC unroll 4
    for (int i = 0; i < B; ++i) {
        const float f = fs_frac32_to_float(frac);
        const int16_t *p = ring16 + ((pos - 1) & MASK);
        const float s0 = (float)p[0], s1 = (float)p[1], s2 = (float)p[2], s3 = (float)p[3];
        CUBIC(s0, s1, s2, s3)
        d->buf[i] = s;
        const uint32_t nf = frac + inc_frac;
        pos += inc_int + (nf < frac);
        frac = nf;
    }
    for (int i = 0; i < B; ++i) {
        float s = d->buf[i];
        FILTER MIX4
    }
    mv_pos[v] = pos; mv_frac[v] = frac; d->z1 = z1; d->z2 = z2;
}

// Catmull-Rom weights for 1024 fractional positions (built in
// bench_run_print): interpolation is 4 independent multiplies
#define WT_BITS 10
static float (*wt)[4];

static void IRAM_ATTR (k_mv_table)(BenchData *d) {
    const int v = mv; mv = (mv + 1) % NV;
    const int16_t *ring16 = rings + v * (RING + 4);
    uint32_t pos = mv_pos[v], frac = mv_frac[v];
    float z1 = d->z1, z2 = d->z2, amp = 0.5f;
    const float damp = 1e-6f, gl = 0.7f, gr = 0.7f;
#pragma GCC unroll 4
    for (int i = 0; i < B; ++i) {
        const float *w = wt[frac >> (32 - WT_BITS)];
        const int16_t *p = ring16 + ((pos - 1) & MASK);
        d->buf[i] = (w[0] * (float)p[0] + w[1] * (float)p[1]) + (w[2] * (float)p[2] + w[3] * (float)p[3]);
        const uint32_t nf = frac + inc_frac;
        pos += inc_int + (nf < frac);
        frac = nf;
    }
    for (int i = 0; i < B; ++i) {
        float s = d->buf[i];
        FILTER MIX4
    }
    mv_pos[v] = pos; mv_frac[v] = frac; d->z1 = z1; d->z2 = z2;
}

// FPU probes, 8 operations per "sample": a dependent chain of adds (the
// add latency), of multiplies, and 8 independent adds (throughput)
static void IRAM_ATTR (k_add_chain)(BenchData *d) {
    float x = d->z1;
    for (int i = 0; i < B; ++i) {
        x += 1.0f; x += 1.0f; x += 1.0f; x += 1.0f; x += 1.0f; x += 1.0f; x += 1.0f; x += 1.0f;
        __asm__ volatile("" : "+f"(x));
    }
    d->z1 = x * 1e-30f;
}
static void IRAM_ATTR (k_mul_chain)(BenchData *d) {
    float x = d->z2 + 1.0f;
    for (int i = 0; i < B; ++i) {
        x *= 1.0001f; x *= 0.9999f; x *= 1.0001f; x *= 0.9999f; x *= 1.0001f; x *= 0.9999f; x *= 1.0001f; x *= 0.9999f;
        __asm__ volatile("" : "+f"(x));
    }
    d->z2 = x - 1.0f;
}
static void IRAM_ATTR (k_add_indep)(BenchData *d) {
    float a = 0, b = 0, c = 0, e = 0, g = 0, h = 0, j = 0, k = 0;
    for (int i = 0; i < B; ++i) {
        a += 1.0f; b += 1.0f; c += 1.0f; e += 1.0f; g += 1.0f; h += 1.0f; j += 1.0f; k += 1.0f;
        __asm__ volatile("" : "+f"(a), "+f"(b), "+f"(c), "+f"(e), "+f"(g), "+f"(h), "+f"(j), "+f"(k));
    }
    d->buf[0] = a + b + c + e + g + h + j + k;
}
// int16 -> float conversion only (4 per sample, as the taps)
static void IRAM_ATTR (k_cvt)(BenchData *d) {
    const int16_t *p = d->ring16;
    for (int i = 0; i < B; ++i) {
        d->buf[i] = (float)p[i] + (float)p[i + 1] + (float)p[i + 2] + (float)p[i + 3];
    }
}

static const struct { const char *name; Kernel k; } kernels[] = {
    { "full (int16 ring, cubic, filter, 2 buses)", k_full },
    { "  no filter", k_nofilter },
    { "  linear interpolation", k_linear },
    { "  write 1 buffer instead of 2 buses", k_buf },
    { "  float ring", k_float_ring },
    { "  taps from two 32-bit loads", k_taps32 },
    { "  with fused multiply-adds", k_fma },
    { "taps only (4 loads + converts)", k_taps },
    { "position stepping + store only", k_step },
    { "two passes (interpolate block, then filter + mix)", k_twopass },
    { "4 buses: full, one pass", k_full4 },
    { "4 buses: two passes", k_twopass4 },
    { "4 buses: three passes (interpolate, filter, mix)", k_threepass4 },
    { "4 buses: interpolate + filter, then mix", k_interp_filter_mix4 },
    { "64 rings, 4 buses: one pass", k_mv_full4 },
    { "64 rings, 4 buses: two passes", k_mv_twopass4 },
    { "64 rings, 4 buses: two passes + prefetch", k_mv_prefetch },
    { "64 rings, 4 buses: two passes, weight table", k_mv_table },
    { "FPU: 8 dependent adds", k_add_chain },
    { "FPU: 8 dependent multiplies", k_mul_chain },
    { "FPU: 8 independent adds", k_add_indep },
    { "4 int16 -> float conversions + 3 adds", k_cvt },
};
#define KERNELS (sizeof(kernels) / sizeof(kernels[0]))

static uint32_t IRAM_ATTR run(Kernel k, BenchData *d) {
    k(d); // Warm up
    const uint32_t t0 = FS_CYCLE_COUNT();
    for (int b = 0; b < BLOCKS; ++b) k(d);
    return FS_CYCLE_COUNT() - t0;
}

static void init_data(BenchData *d) {
    for (int i = 0; i < RING + 4; ++i) {
        d->ring16[i] = (int16_t)(12000.0f * sinf(0.05f * (float)i) + 3000.0f * sinf(0.31f * (float)i));
        d->ringf[i] = (float)d->ring16[i];
    }
    memset(d->bus_l, 0, sizeof(d->bus_l));
    memset(d->bus_r, 0, sizeof(d->bus_r));
    memset(d->bus_rv, 0, sizeof(d->bus_rv));
    memset(d->bus_ch, 0, sizeof(d->bus_ch));
    d->z1 = d->z2 = 0.0f;
    d->pos = 1;
    d->frac = 0;
}

void bench_run_print(void) {
    float result[KERNELS];
    if (!rings) {
        rings = malloc(NV * (RING + 4) * sizeof(int16_t));
        if (!rings) { printf("bench: out of memory\n"); return; }
        for (int v = 0; v < NV; ++v) {
            for (int i = 0; i < RING + 4; ++i) rings[v * (RING + 4) + i] = (int16_t)(i * 37 + v * 11);
            mv_pos[v] = 1 + v * 13; mv_frac[v] = 0;
        }
        wt = malloc((1u << WT_BITS) * sizeof(*wt));
        if (!wt) { printf("bench: out of memory\n"); return; }
        for (uint32_t k = 0; k < (1u << WT_BITS); ++k) {
            const float f = (float)k / (float)(1u << WT_BITS);
            wt[k][0] = -0.5f * f * f * f + f * f - 0.5f * f;
            wt[k][1] = 1.5f * f * f * f - 2.5f * f * f + 1.0f;
            wt[k][2] = -1.5f * f * f * f + 2.0f * f * f + 0.5f * f;
            wt[k][3] = 0.5f * f * f * f - 0.5f * f * f;
        }
    }
    vTaskSuspendAll();
    for (unsigned i = 0; i < KERNELS; ++i) {
        init_data(&data);
        result[i] = (float)run(kernels[i].k, &data) / (float)(BLOCKS * B);
    }
    xTaskResumeAll();
    printf("bench: cycles per sample (FPU probes: per operation x 8)\n");
    for (unsigned i = 0; i < KERNELS; ++i) printf("  %6.1f  %s\n", (double)result[i], kernels[i].name);
    printf("  (checksum %08lx)\n", (unsigned long)(data.bus_l[3] * 1000.0f + data.buf[5]));
}
