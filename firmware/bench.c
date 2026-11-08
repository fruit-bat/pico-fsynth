//
// Inner loop micro benchmarks (built with -DFS_BENCH=ON).
//
// Each kernel is a variant of the voice sample loop in core/fs_voice.c,
// timed with the DWT cycle counter over many 32 frame blocks. Comparing
// them shows where the cycles of the real loop go. Run at boot, before the
// audio starts; results are printed with the status lines.
//
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/structs/m33.h"
#include "hardware/sync.h"
#include "fs_config.h"
#include "fs_math.h"
#include "bench.h"

// Real synth on one core, no interrupts from the audio: loop cycles per
// voice-sample from the synth's own counters
typedef struct { const char *name; float loop, total; uint32_t voices; } SynthResult;
static SynthResult synth_result[4];

#define B FS_BLOCK_SIZE
#define RING 1024
#define MASK (RING - 1)
#define BLOCKS 1000

typedef struct {
    int16_t ring16[RING + 4];
    float ringf[RING + 4];
    float bus_l[B], bus_r[B], buf[B];
    float z1, z2;
    uint32_t pos, frac;
} BenchData;

// One set per core so the dual core test does not share data
static BenchData data[2];

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
static void __not_in_flash_func(k_full)(BenchData *d) { LOOP_BEGIN TAPS16 CUBIC(s0, s1, s2, s3) FILTER PAN_MIX LOOP_END }
// Without the filter
static void __not_in_flash_func(k_nofilter)(BenchData *d) { LOOP_BEGIN TAPS16 CUBIC(s0, s1, s2, s3) PAN_MIX LOOP_END }
// Linear interpolation
static void __not_in_flash_func(k_linear)(BenchData *d) {
    LOOP_BEGIN
    const int16_t *p = d->ring16 + (pos & MASK);
    const float s1 = (float)p[0], s2 = (float)p[1];
    float s = s1 + f * (s2 - s1);
    FILTER PAN_MIX LOOP_END
}
// Write to one buffer instead of mixing into two buses
static void __not_in_flash_func(k_buf)(BenchData *d) {
    LOOP_BEGIN TAPS16 CUBIC(s0, s1, s2, s3) FILTER
    amp += damp;
    d->buf[i] = s * amp;
    LOOP_END
}
// Only fetch and convert the 4 taps
static void __not_in_flash_func(k_taps)(BenchData *d) {
    LOOP_BEGIN TAPS16
    d->buf[i] = (s0 + s1) + (s2 + s3);
    LOOP_END
}
// Only the position stepping and a store
static void __not_in_flash_func(k_step)(BenchData *d) {
    LOOP_BEGIN
    d->buf[i] = f;
    LOOP_END
}
// Float ring: no int to float conversion
static void __not_in_flash_func(k_float_ring)(BenchData *d) {
    LOOP_BEGIN
    const float *p = d->ringf + ((pos - 1) & MASK);
    const float s0 = p[0], s1 = p[1], s2 = p[2], s3 = p[3];
    CUBIC(s0, s1, s2, s3) FILTER PAN_MIX LOOP_END
}
// Taps from two unaligned 32-bit loads
static void __not_in_flash_func(k_taps32)(BenchData *d) {
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
static void __not_in_flash_func(k_fma)(BenchData *d) {
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
};
#define KERNELS (sizeof(kernels) / sizeof(kernels[0]))

static float result[KERNELS];
static float result_dual[2];
static uint32_t checksum;

static uint32_t __not_in_flash_func(run)(Kernel k, BenchData *d) {
    k(d); // Warm up
    const uint32_t t0 = m33_hw->dwt_cyccnt;
    for (int b = 0; b < BLOCKS; ++b) k(d);
    return m33_hw->dwt_cyccnt - t0;
}

static void init_data(BenchData *d) {
    for (int i = 0; i < RING + 4; ++i) {
        d->ring16[i] = (int16_t)(12000.0f * sinf(0.05f * (float)i) + 3000.0f * sinf(0.31f * (float)i));
        d->ringf[i] = (float)d->ring16[i];
    }
    memset(d->bus_l, 0, sizeof(d->bus_l));
    memset(d->bus_r, 0, sizeof(d->bus_r));
    d->z1 = d->z2 = 0.0f;
    d->pos = 1;
    d->frac = 0;
}

static void enable_counter(void) {
    m33_hw->demcr |= M33_DEMCR_TRCENA_BITS;
    m33_hw->dwt_ctrl |= M33_DWT_CTRL_CYCCNTENA_BITS;
}

static volatile uint32_t core1_cycles;
static void core1_bench(void) {
    enable_counter();
    multicore_fifo_pop_blocking(); // Start together with core 0
    core1_cycles = run(k_full, &data[1]);
    multicore_fifo_push_blocking(1);
    for (;;) tight_loop_contents();
}

static SynthResult bench_synth(FsSynth *s, const char *name, const uint8_t *progs, int nprogs,
                               const uint8_t *keys, int nkeys, bool effects) {
    fs_synth_reset(s);
    fs_synth_set_effects(s, effects, effects);
    for (int c = 0; c < nprogs; ++c) {
        const uint8_t pc[2] = { (uint8_t)(0xC0 | c), progs[c] };
        fs_synth_midi(s, pc, 2);
        for (int k = 0; k < nkeys; ++k) {
            const uint8_t on[3] = { (uint8_t)(0x90 | c), (uint8_t)(keys[k] + (c & 1 ? 12 : 0)), 90 };
            fs_synth_midi(s, on, 3);
        }
    }
    float l[B], r[B];
    for (int b = 0; b < 20; ++b) fs_synth_render_float(s, l, r); // Past the attacks
    memset(s->stats.loop_cycles, 0, sizeof(s->stats.loop_cycles));
    memset(s->stats.part_cycles, 0, sizeof(s->stats.part_cycles));
    memset(s->stats.voice_blocks, 0, sizeof(s->stats.voice_blocks));
    const uint32_t irq = save_and_disable_interrupts();
    for (int b = 0; b < 300; ++b) fs_synth_render_float(s, l, r);
    restore_interrupts(irq);
    uint32_t loop = 0, part = 0, vb = 0;
    for (int p = 0; p < FS_RENDER_PARTS; ++p) {
        loop += s->stats.loop_cycles[p];
        part += s->stats.part_cycles[p];
        vb += s->stats.voice_blocks[p];
    }
    SynthResult res = { name, 0.0f, 0.0f, fs_synth_active_voices(s) };
    if (vb) {
        res.loop = (float)loop / ((float)vb * B);
        res.total = (float)part / ((float)vb * B);
    }
    fs_synth_all_sound_off(s);
    for (int b = 0; b < 50; ++b) fs_synth_render_float(s, l, r);
    fs_synth_reset(s);
    fs_synth_set_effects(s, true, true);
    return res;
}

void bench_run(FsSynth *synth) {
    enable_counter();
    init_data(&data[0]);
    init_data(&data[1]);
    const float samples = (float)BLOCKS * B;
    for (unsigned i = 0; i < KERNELS; ++i) {
        result[i] = (float)run(kernels[i].k, &data[0]) / samples;
    }

    // Both cores running the full kernel at once (shared SRAM and buses)
    multicore_launch_core1(core1_bench);
    multicore_fifo_push_blocking(0);
    const uint32_t c0 = run(k_full, &data[0]);
    multicore_fifo_pop_blocking();
    result_dual[0] = (float)c0 / samples;
    result_dual[1] = (float)core1_cycles / samples;
    multicore_reset_core1();

    for (int i = 0; i < B; ++i) checksum += (uint32_t)(int32_t)(data[0].bus_l[i] + data[1].buf[i]);

    static const uint8_t piano[] = { 0 }, c4[] = { 60 };
    static const uint8_t poly_progs[] = { 0, 48, 52, 19, 89, 61 };
    static const uint8_t chord[] = { 48, 52, 55, 60, 64, 67 };
    synth_result[0] = bench_synth(synth, "1 piano note", piano, 1, c4, 1, true);
    synth_result[1] = bench_synth(synth, "poly chord (6 programs x 6 keys)", poly_progs, 6, chord, 6, true);
    synth_result[2] = bench_synth(synth, "poly chord, effects off", poly_progs, 6, chord, 6, false);
    synth_result[3] = bench_synth(synth, "poly chord, one note per program", poly_progs, 6, c4, 1, true);
}

void bench_print(void) {
    printf("bench: cycles per sample (one voice), %lu blocks each\n", (unsigned long)BLOCKS);
    for (unsigned i = 0; i < KERNELS; ++i) printf("  %-44s %6.1f\n", kernels[i].name, (double)result[i]);
    printf("  full, both cores at once: core0 %6.1f core1 %6.1f   (checksum %lu)\n",
           (double)result_dual[0], (double)result_dual[1], (unsigned long)checksum);
    printf("bench: real synth, one core, interrupts off: cycles per voice-sample\n");
    for (int i = 0; i < 4; ++i) {
        printf("  %-36s voices %2lu  loop %6.1f  all %6.1f\n", synth_result[i].name,
               (unsigned long)synth_result[i].voices, (double)synth_result[i].loop, (double)synth_result[i].total);
    }
}
