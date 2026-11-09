#pragma once
//
// bench.h - inner loop micro benchmarks (FS_BENCH builds only), see bench.c.
//
#include "fs_synth.h"

// Run all benchmarks and keep the results for bench_print(). Launches and
// then resets core 1, and plays notes through the synth (left reset, effects
// on, afterwards), so call from core 0 before core 1 is started for audio.
// Takes a second or two.
void bench_run(FsSynth *synth);

// Print the stored results to stdio. Core 0 (USB console).
void bench_print(void);
