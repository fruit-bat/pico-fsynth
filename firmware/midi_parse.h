#pragma once
//
// midi_parse.h - MIDI input parsers: a raw byte stream (UART) and USB MIDI
// event packets, turned into complete messages.
//
// Pure C with no Pico SDK dependencies, so the host test suite can run it
// (tests/test_midi_parse.c). midi_in.c feeds it from the hardware and
// queues what comes out.
//
// Each parser hands complete messages to an emit callback:
//   channel messages   status + 1 or 2 data bytes, as received
//   recognised SysEx   as a short internal code (below); the SysEx itself
//                      is not passed on
// Real-time bytes, system common messages and unrecognised SysEx are
// dropped.
//
// SysEx is collected into a 16-byte buffer: enough for every message the
// synth understands (the longest, GS EQ, is 11 bytes). A longer message is
// discarded as a whole, never acted on from its first 16 bytes.
//
#include <stdint.h>
#include <stdbool.h>

// Internal codes for recognised SysEx. They are real-time status bytes,
// which the parsers drop, so they can never be confused with real input.
//   GM/GM2 on, GS reset:  MIDI_MSG_RESET                      (1 byte)
//   GM master volume:     MIDI_MSG_MASTER_VOLUME, lsb, msb    (3 bytes)
//   GS master EQ:         MIDI_MSG_EQ, parameter 0..3, value  (3 bytes)
//   Settings command:     MIDI_MSG_SETTINGS, MIDI_SETTINGS_*  (2 bytes)
#define MIDI_MSG_RESET 0xff
#define MIDI_MSG_MASTER_VOLUME 0xfe
#define MIDI_MSG_EQ 0xfd
#define MIDI_MSG_SETTINGS 0xfc

// Settings commands, SysEx F0 7D 46 53 cmd F7 (7D = non-commercial ID,
// 46 53 = "FS"). See host/settings.sh.
#define MIDI_SETTINGS_SAVE 1        // Save master volume and EQ to flash
#define MIDI_SETTINGS_ERASE 2       // Erase saved settings, back to defaults

// Receives each complete message (1..3 bytes); msg is only valid during the
// call. ctx is the pointer given at init.
typedef void (*MidiEmit)(void *ctx, const uint8_t *msg, uint8_t len);

// SysEx being collected
typedef struct {
    uint8_t buf[16];        // From F0
    uint8_t len;
    bool active;            // Between F0 and its end
    bool overflow;          // Longer than buf: discard at the end
} MidiSysex;

// Byte stream parser state (UART)
typedef struct {
    MidiEmit emit;
    void *ctx;
    uint8_t status;         // Running status (0 = none)
    uint8_t data[2];        // Data bytes collected for the current message
    uint8_t have;           // Data bytes collected so far
    uint8_t need;           // Data bytes the current status takes (1 or 2)
    MidiSysex sysex;
} MidiByteParser;

// USB MIDI packet parser state
typedef struct {
    MidiEmit emit;
    void *ctx;
    MidiSysex sysex;
} MidiUsbParser;

// Reset a parser and set where its messages go.
void midi_byte_parser_init(MidiByteParser *p, MidiEmit emit, void *ctx);
void midi_usb_parser_init(MidiUsbParser *p, MidiEmit emit, void *ctx);

// Feed one byte of a raw MIDI stream. Handles running status, real-time
// bytes interleaved anywhere, and SysEx (which must end with F7; a SysEx
// cut short by another status byte is dropped).
void midi_parse_byte(MidiByteParser *p, uint8_t b);

// Feed one 4-byte USB MIDI event packet: p[0] low nibble is the Code Index
// Number (CIN), p[1..3] the MIDI bytes. The cable number is ignored.
void midi_parse_usb_packet(MidiUsbParser *p, const uint8_t packet[4]);
