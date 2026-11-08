//
// pico-fsynth firmware
//
// Core 0: USB (serial console + MIDI), UART MIDI input, status reporting.
//         With FS_DUAL_CORE it also renders half of the voices of every
//         block, in the inter-core FIFO interrupt so that a slow USB print
//         can never hold up the audio.
// Core 1: owns the synth; renders audio blocks into the output DMA ring and
//         applies queued MIDI messages between blocks.
//
// The compiled font (.fsf) must be flashed at FS_FONT_FLASH_OFFSET.
//
#include <stdio.h>
#include <math.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/regs/addressmap.h"
#include "hardware/structs/m33.h"
#include "pico/stdio_usb.h"
#include "tusb.h"

#include "fs_font.h"
#include "fs_synth.h"
#include "fs_smf.h"
#include "audio_out.h"
#include "midi_in.h"
#include "settings.h"
#if FS_BENCH
#include "bench.h"
#endif

#if FS_AUDIO_OUT_I2S
#define FS_AUDIO_OUT_NAME "I2S"
#else
#define FS_AUDIO_OUT_NAME "PWM"
#endif

extern const uint8_t fs_demo_midi[];
extern const size_t fs_demo_midi_size;
extern char __flash_binary_end;

static FsFont font;
static FsSynth synth;       // Large (stream rings, effects): lives in SRAM
static FsSmf demo;

static volatile bool demo_playing;
static volatile bool audio_ok;
static volatile bool audio_failed;

// Render statistics, written by core 1, read by core 0
// Core 0 asks core 1 to wait in RAM (flash writes): park_request -> parked
static volatile bool park_request;
static volatile bool core1_parked;

static volatile uint32_t stat_buffers;
static volatile uint32_t stat_busy_us;      // Accumulated render time
static volatile uint32_t stat_busy_max_us;  // Worst buffer since last report
static volatile uint32_t stat_voices_max;
static volatile uint32_t stat_core0_us;     // Time core 0 spent rendering
static volatile uint32_t stat_midi_cycles;  // Core 1 cycles applying MIDI (note on etc.)
static volatile uint32_t stat_midi_max;     // Worst block for MIDI cycles

// Inter-core FIFO commands
#define CMD_RENDER_PART 0x52454e44u  // Core 1 -> core 0: render parts 1..N-1
#define CMD_PART_DONE   0x444f4e45u  // Core 0 -> core 1: done

static void on_demo_midi(void *ctx, const uint8_t *msg, uint32_t len) {
    fs_synth_midi((FsSynth *)ctx, msg, len);
}

// MIDI is applied for at most this share of a block; the rest waits for the
// next block. A burst (a big chord arrives over USB all at once) is spread
// over a few blocks instead of causing an audio underrun.
#ifndef FS_FP_CONTRACT_OFF
#define FS_FP_CONTRACT_OFF 0
#endif

#ifndef FS_MIDI_BUDGET_PERCENT
#define FS_MIDI_BUDGET_PERCENT 25
#endif

static uint32_t midi_budget_cycles;

static void __not_in_flash_func(apply_midi)(void) {
    MidiMsg m;
    const uint32_t t0 = FS_CYCLE_COUNT();
    while (FS_CYCLE_COUNT() - t0 < midi_budget_cycles && midi_in_pop(&m)) {
        if (demo_playing) {
            // Live input takes over from the demo
            demo_playing = false;
            fs_synth_reset(&synth);
        }
        if (m.len == 1 && m.data[0] == MIDI_MSG_RESET) fs_synth_reset(&synth);
        else if (m.len == 3 && m.data[0] == MIDI_MSG_MASTER_VOLUME) {
            fs_synth_set_master_volume(&synth, (uint16_t)(m.data[1] | (m.data[2] << 7)));
        } else if (m.len == 3 && m.data[0] == MIDI_MSG_EQ) {
            fs_synth_set_eq_gs(&synth, m.data[1], m.data[2]);
        } else fs_synth_midi(&synth, m.data, m.len);
    }
}

// Start this core's cycle counter (FS_CYCLE_COUNT, used for profiling)
static void enable_cycle_counter(void) {
    m33_hw->demcr |= M33_DEMCR_TRCENA_BITS;
    m33_hw->dwt_cyccnt = 0;
    m33_hw->dwt_ctrl |= M33_DWT_CTRL_CYCCNTENA_BITS;
}

static void __not_in_flash_func(core1_main)(void) {
    enable_cycle_counter();
    midi_budget_cycles = (uint32_t)((uint64_t)clock_get_hz(clk_sys) * FS_BLOCK_SIZE / FS_SAMPLE_RATE * FS_MIDI_BUDGET_PERCENT / 100);
    if (!audio_out_init(FS_SAMPLE_RATE)) {
        audio_failed = true;
        for (;;) tight_loop_contents();
    }
    audio_ok = true;

    uint32_t demo_gap = 0; // Blocks of silence between demo repeats
    for (;;) {
        if (park_request) {
            // Between blocks: wait here (RAM only) while core 0 writes flash.
            // The DMA interrupt keeps running and plays silence.
            core1_parked = true;
            while (park_request) tight_loop_contents();
            core1_parked = false;
        }
        int16_t *buf = audio_out_acquire();
        const uint32_t t0 = time_us_32();
        for (int b = 0; b < AUDIO_OUT_FRAMES / FS_BLOCK_SIZE; ++b) {
            const uint32_t c0 = FS_CYCLE_COUNT();
            apply_midi();
            const uint32_t mc = FS_CYCLE_COUNT() - c0;
            stat_midi_cycles = stat_midi_cycles + mc;
            if (mc > stat_midi_max) stat_midi_max = mc;
            if (demo_playing) {
                if (demo_gap) {
                    if (--demo_gap == 0) fs_smf_init(&demo, fs_demo_midi, fs_demo_midi_size, FS_SAMPLE_RATE);
                } else if (!fs_smf_advance(&demo, FS_BLOCK_SIZE, on_demo_midi, &synth)) {
                    demo_gap = 3 * FS_SAMPLE_RATE / FS_BLOCK_SIZE; // Restart after 3 s
                }
            }
            int16_t *out = buf + b * FS_BLOCK_SIZE * 2;
#if FS_DUAL_CORE
            fs_synth_render_begin(&synth);
            multicore_fifo_push_blocking(CMD_RENDER_PART);
            fs_synth_render_part(&synth, 0);
            while (multicore_fifo_pop_blocking() != CMD_PART_DONE) tight_loop_contents();
            fs_synth_render_end(&synth, out);
#else
            fs_synth_render(&synth, out);
#endif
        }
        const uint32_t dt = time_us_32() - t0;
        audio_out_submit();

        stat_buffers = stat_buffers + 1;
        stat_busy_us = stat_busy_us + dt;
        if (dt > stat_busy_max_us) stat_busy_max_us = dt;
        if (fs_synth_active_voices(&synth) > stat_voices_max) stat_voices_max = fs_synth_active_voices(&synth);
    }
}

#if FS_DUAL_CORE
// Core 0 side of the split render: runs in the FIFO interrupt, so it
// preempts whatever core 0 is doing (USB, printing).
static void __not_in_flash_func(core0_fifo_irq)(void) {
    while (multicore_fifo_rvalid()) {
        const uint32_t cmd = multicore_fifo_pop_blocking();
        if (cmd == CMD_RENDER_PART) {
            const uint32_t t0 = time_us_32();
            for (uint32_t p = 1; p < FS_RENDER_PARTS; ++p) fs_synth_render_part(&synth, p);
            stat_core0_us = stat_core0_us + (time_us_32() - t0);
            multicore_fifo_push_blocking(CMD_PART_DONE);
        }
    }
    multicore_fifo_clear_irq();
}
#endif

// Keep USB alive while waiting
static void usb_sleep_ms(uint32_t ms) {
    const absolute_time_t until = make_timeout_time_ms(ms);
    while (!time_reached(until)) {
        tud_task();
        midi_in_poll();
    }
}

static void print_font_info(void) {
    const FsFontHeader *h = font.header;
    printf("font '%s': %lu presets, %lu regions, %lu frames at 0x%08lx\n",
           h->name, (unsigned long)h->preset_count, (unsigned long)h->region_count,
           (unsigned long)h->data_frames, (unsigned long)(XIP_BASE + FS_FONT_FLASH_OFFSET));
}

int main(void) {
    if (FS_SYS_CLOCK_KHZ > 150000) {
        // Overclocking: raise the core voltage first
        vreg_set_voltage(VREG_VOLTAGE_1_20);
        sleep_ms(10);
    }
    set_sys_clock_khz(FS_SYS_CLOCK_KHZ, true);

    tusb_init();
    stdio_init_all();
    midi_in_init();

    const uint8_t *blob = (const uint8_t *)(XIP_BASE + FS_FONT_FLASH_OFFSET);
    const bool overlap = (uintptr_t)&__flash_binary_end > (uintptr_t)blob;
    const FsFontResult res = overlap ? FS_FONT_ERR_LAYOUT :
        fs_font_init(&font, blob, PICO_FLASH_SIZE_BYTES - FS_FONT_FLASH_OFFSET, true, false);
    while (res != FS_FONT_OK) {
        if (overlap) printf("firmware overlaps the font area at 0x%x\n", FS_FONT_FLASH_OFFSET);
        else printf("no usable font at flash offset 0x%x: %s\n", FS_FONT_FLASH_OFFSET, fs_font_result_str(res));
        usb_sleep_ms(2000);
    }

    fs_synth_init(&synth, &font, FS_SAMPLE_RATE);
    const bool settings_loaded = settings_load(&synth);
    if (fs_demo_midi_size && fs_smf_init(&demo, fs_demo_midi, fs_demo_midi_size, FS_SAMPLE_RATE)) {
        demo_playing = true;
    }

#if FS_BENCH
    bench_run(&synth); // Uses core 1 and the synth, so before the audio starts
#endif
    enable_cycle_counter();
    multicore_launch_core1(core1_main);
#if FS_DUAL_CORE
    // After the launch handshake, which also uses the FIFO
    irq_set_exclusive_handler(SIO_IRQ_FIFO, core0_fifo_irq);
    irq_set_priority(SIO_IRQ_FIFO, PICO_HIGHEST_IRQ_PRIORITY);
    irq_set_enabled(SIO_IRQ_FIFO, true);
#endif
    while (!audio_ok && !audio_failed) tight_loop_contents();

    if (!demo_playing && audio_ok) {
        // Short arpeggio so there is something to hear at power up
        static const uint8_t keys[] = { 60, 64, 67, 72 };
        for (unsigned i = 0; i < sizeof(keys); ++i) {
            const uint8_t on[3] = { 0x90, keys[i], 100 };
            midi_in_inject(on, 3);
            usb_sleep_ms(120);
        }
        usb_sleep_ms(600);
        const uint8_t off[3] = { 0xb0, 123, 0 }; // All notes off
        midi_in_inject(off, 3);
    }

    const float buffer_us = (float)AUDIO_OUT_FRAMES * 1e6f / audio_out_rate();
    absolute_time_t next_report = make_timeout_time_ms(1000);
    bool info_printed = false;

    for (;;) {
        tud_task();
        midi_in_poll();

        const uint8_t cmd = midi_in_settings_cmd();
        if (cmd) {
            if (!settings_available(&font)) {
                printf("settings: not saved, the font reaches the settings sector\n");
            } else {
                // Core 1 stops between blocks (core 0 keeps serving its render
                // requests until then), then flash is ours
                park_request = true;
                while (!core1_parked) tight_loop_contents();
                if (cmd == MIDI_SETTINGS_SAVE) settings_save(&synth);
                else {
                    settings_erase();
                    fs_synth_set_master_volume(&synth, 16383);
                    for (uint8_t p = 0; p < 4; ++p) fs_synth_set_eq_gs(&synth, p, (p & 1) ? 0x40 : 0);
                }
                park_request = false;
                printf("settings: %s\n", cmd == MIDI_SETTINGS_SAVE ? "saved to flash" : "erased, defaults restored");
            }
        }

        if (time_reached(next_report)) {
            next_report = make_timeout_time_ms(1000);
            if (!info_printed && stdio_usb_connected()) {
                print_font_info();
                printf("audio: %s at %.1f Hz, sys clock %lu kHz, %d voices max, block %d, buffer %d frames, %s\n",
                       FS_AUDIO_OUT_NAME, (double)audio_out_rate(),
                       (unsigned long)(clock_get_hz(clk_sys) / 1000), FS_MAX_VOICES, FS_BLOCK_SIZE, AUDIO_OUT_FRAMES,
                       FS_DUAL_CORE ? "voices on both cores" : "voices on core 1 only");
                printf("settings: %s\n", settings_loaded ? "loaded from flash" : "defaults (none saved)");
                info_printed = true;
            }
            if (audio_failed) {
                printf("audio output failed to start (check pins)\n");
                continue;
            }
#if FS_BENCH
            static uint32_t reports;
            if (reports++ % 30 == 0) bench_print();
#endif
            // Snapshot and reset (racy by a buffer at worst; fine for a report)
            const uint32_t buffers = stat_buffers, busy = stat_busy_us, busy_max = stat_busy_max_us, vmax = stat_voices_max;
            const uint32_t core0 = stat_core0_us;
            const uint32_t midi_cycles = stat_midi_cycles, midi_max = stat_midi_max;
            uint32_t part_cyc[FS_RENDER_PARTS], loop_cyc[FS_RENDER_PARTS], fill_cyc[FS_RENDER_PARTS], vblocks[FS_RENDER_PARTS];
            for (int p = 0; p < FS_RENDER_PARTS; ++p) {
                part_cyc[p] = synth.stats.part_cycles[p]; synth.stats.part_cycles[p] = 0;
                loop_cyc[p] = synth.stats.loop_cycles[p]; synth.stats.loop_cycles[p] = 0;
                fill_cyc[p] = synth.stats.fill_cycles[p]; synth.stats.fill_cycles[p] = 0;
                vblocks[p] = synth.stats.voice_blocks[p]; synth.stats.voice_blocks[p] = 0;
            }
            stat_midi_max = 0;
            const uint32_t start_cycles = synth.stats.start_cycles, start_count = synth.stats.start_count;
            synth.stats.start_cycles = 0;
            synth.stats.start_count = 0;
            stat_midi_cycles = 0;
            stat_core0_us = 0;
            stat_buffers = 0;
            stat_busy_us = 0;
            stat_busy_max_us = 0;
            stat_voices_max = 0;
            const float load = buffers ? 100.0f * (float)busy / ((float)buffers * buffer_us) : 0.0f;
            const float load_max = 100.0f * (float)busy_max / buffer_us;
            // core 1 load includes waiting for core 0's half; it is what
            // limits the synth (100% = no time to spare)
            const float load0 = buffers ? 100.0f * (float)core0 / ((float)buffers * buffer_us) : 0.0f;
            // Output samples clipped since the last report, and master volume
            static uint32_t clipped_last;
            const uint32_t clipped = synth.stats.clipped;
            const float peak = synth.stats.peak;
            synth.stats.peak = 0.0f;
            const float peak_db = peak > 1e-6f ? 20.0f * log10f(peak) : -120.0f;
            printf("cpu core1 %5.1f%% (max %5.1f%%) core0 %5.1f%%  voices max %2lu  underruns %lu  midi %lu (dropped %lu)  peak %4.0f dB  clipped %lu  volume %3.0f%%  eq %+.0f/%+.0f dB%s\n",
                   (double)load, (double)load_max, (double)load0, (unsigned long)vmax,
                   (unsigned long)audio_out_underruns(), (unsigned long)midi_in_count(),
                   (unsigned long)midi_in_dropped(), (double)peak_db, (unsigned long)(clipped - clipped_last),
                   (double)(100.0f * synth.master_volume), (double)synth.eq.low_db, (double)synth.eq.high_db,
                   demo_playing ? "  [demo]" : "");
            clipped_last = clipped;
            // Where the time goes. Per voice-sample cycles from the parts on
            // core 0 (part 0 also runs the effects); effects = what is left
            // of part 0 after its voices.
            const float cycles = (float)buffers * buffer_us * (float)clock_get_hz(clk_sys) * 1e-6f;
            uint32_t vb = 0, pc = 0, lc = 0, fc = 0;
            for (int p = 1; p < FS_RENDER_PARTS; ++p) {
                vb += vblocks[p]; pc += part_cyc[p]; lc += loop_cyc[p]; fc += fill_cyc[p];
            }
            if (cycles > 0.0f && vb > 0) {
                const float vs = (float)vb * FS_BLOCK_SIZE;
                const float per_voice = (float)pc / vs;
                const float effects = (float)part_cyc[0] - per_voice * (float)vblocks[0] * FS_BLOCK_SIZE;
                const float block_cycles = (float)FS_BLOCK_SIZE * (float)clock_get_hz(clk_sys) / audio_out_rate();
                printf("    cycles per voice-sample %5.1f (loop %5.1f flash %4.1f other %5.1f)  effects %4.1f%%  midi %4.1f%% (max block %5.1f%%)\n",
                       (double)per_voice, (double)((float)lc / vs), (double)((float)fc / vs),
                       (double)((float)(pc - lc - fc) / vs), (double)(100.0f * effects / cycles),
                       (double)(100.0f * (float)midi_cycles / cycles), (double)(100.0f * (float)midi_max / block_cycles));
            }
            if (start_count) {
                printf("    voice starts %lu, %lu cycles each%s\n", (unsigned long)start_count,
                       (unsigned long)(start_cycles / start_count), FS_FP_CONTRACT_OFF ? "" : "  [fused multiply-add on]");
            }
        }
    }
}
