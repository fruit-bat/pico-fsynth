#pragma once
//
// console.h - a small command line on the serial console, for testing
// without MIDI hardware. Type `help` in `idf.py monitor`.
//
#include <stdint.h>

// active_voices: returns the synth's current voice count (read only).
// codec_bench: runs the sample decoding benchmark (the `codec` command).
void console_init(uint32_t (*active_voices)(void), void (*codec_bench)(void));

// Read any typed characters and run a command when a line is complete.
// Call often from the main task. Commands inject MIDI with midi_in_inject.
void console_poll(void);
