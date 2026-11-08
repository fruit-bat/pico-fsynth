#pragma once
//
// MIDI input from a DIN/TRS UART port and USB MIDI, both parsed on core 0
// into complete messages and passed to the render core through a
// single-producer single-consumer queue.
//
#include <stdint.h>
#include <stdbool.h>

// A queued message: up to 3 bytes. SysEx messages the synth understands are
// passed on as codes that never appear in the queue otherwise (real-time
// bytes are dropped by the parsers):
//   GM/GS reset:   the single byte MIDI_MSG_RESET
//   Master volume: MIDI_MSG_MASTER_VOLUME, lsb, msb
//   GS master EQ:  MIDI_MSG_EQ, parameter (0..3), value
typedef struct {
    uint8_t len;
    uint8_t data[3];
} MidiMsg;

#define MIDI_MSG_RESET 0xff
#define MIDI_MSG_MASTER_VOLUME 0xfe
#define MIDI_MSG_EQ 0xfd

void midi_in_init(void);

// Poll the UART and USB (call often from core 0, after tud_task()).
void midi_in_poll(void);

// Queue a message from core 0 code (e.g. a test note). Core 0 only.
void midi_in_inject(const uint8_t *data, uint8_t len);

// Consumer side (render core): false when the queue is empty.
bool midi_in_pop(MidiMsg *msg);

// Total messages received, and messages lost because the queue was full
uint32_t midi_in_count(void);
uint32_t midi_in_dropped(void);

// Settings commands, SysEx F0 7D 46 53 cmd F7 (7D = non-commercial ID,
// 46 53 = "FS"). Handled on core 0, not queued: midi_in_settings_cmd()
// returns the latest command once (0 = none).
#define MIDI_SETTINGS_SAVE 1        // Save master volume and EQ to flash
#define MIDI_SETTINGS_ERASE 2       // Erase saved settings, back to defaults
uint8_t midi_in_settings_cmd(void);
