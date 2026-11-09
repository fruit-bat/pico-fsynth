#pragma once
//
// midi_in.h - MIDI input from a UART (DIN/TRS) port and USB MIDI.
//
// Both inputs are parsed on core 0 (midi_in_poll(), called from the main
// loop) into complete channel messages and pushed into a single-producer
// single-consumer queue. The render core pops them between audio blocks
// (midi_in_pop()). Nothing here blocks the render core.
//
// SysEx is not passed through: the few SysEx messages the synth understands
// are recognised by the parsers and turned into short internal codes (see
// MidiMsg), or, for the settings commands, handled on core 0 directly.
// Real-time bytes (clock, active sensing...) and other system messages are
// dropped.
//
// The UART is uart1 at 31250 baud on FS_MIDI_UART_RX_PIN (CMakeLists.txt).
//
#include <stdint.h>
#include <stdbool.h>
#include "midi_parse.h"   // MIDI_MSG_* and MIDI_SETTINGS_* codes

// A queued message: up to 3 bytes, status byte first. SysEx messages the
// synth understands are passed on as codes that never appear in the queue
// otherwise (0xfd..0xff are real-time status bytes, which the parsers drop):
//   GM/GS reset:   the single byte MIDI_MSG_RESET
//   Master volume: MIDI_MSG_MASTER_VOLUME, lsb, msb
//   GS master EQ:  MIDI_MSG_EQ, parameter (0..3), value
typedef struct {
    uint8_t len;            // 1..3
    uint8_t data[3];
} MidiMsg;


// Set up the MIDI UART and its RX pin. Core 0, once at start up.
void midi_in_init(void);

// Drain the UART and the USB MIDI FIFO, parsing and queueing messages.
// Call often from core 0, after tud_task(). Messages that do not fit in
// the queue are dropped and counted (midi_in_dropped()).
void midi_in_poll(void);

// Queue a message from core 0 code (e.g. a test note): 1..3 bytes, status
// byte first. Other lengths are ignored. Core 0 only (it is the producer).
void midi_in_inject(const uint8_t *data, uint8_t len);

// Consumer side (render core): copy the oldest message into *msg and remove
// it. Returns false when the queue is empty. Non-blocking, runs from RAM.
bool midi_in_pop(MidiMsg *msg);

// Total messages queued, and messages lost because the queue was full.
// Both count from power up and wrap at 2^32.
uint32_t midi_in_count(void);
uint32_t midi_in_dropped(void);

// Settings commands, SysEx F0 7D 46 53 cmd F7 (7D = non-commercial ID,
// 46 53 = "FS"). Handled on core 0, not queued: midi_in_settings_cmd()
// returns the latest command once (0 = none, else MIDI_SETTINGS_SAVE or
// MIDI_SETTINGS_ERASE). See host/settings.sh.
uint8_t midi_in_settings_cmd(void);
