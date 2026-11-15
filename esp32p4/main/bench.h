#pragma once
//
// bench.h - inner loop micro benchmarks on the ESP32-P4 (bench.c).
//

// Time every kernel and print cycles per sample. Suspends the scheduler on
// this core for about a second, so the audio underruns meanwhile.
void bench_run_print(void);
