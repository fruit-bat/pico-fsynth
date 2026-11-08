#pragma once
// Inner loop micro benchmarks (FS_BENCH builds only), see bench.c

#include "fs_synth.h"

// Run all benchmarks. Uses core 1 and the synth (left reset afterwards), so
// call before the audio starts.
void bench_run(FsSynth *synth);

// Print the results to stdio
void bench_print(void);
