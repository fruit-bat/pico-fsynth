#pragma once
//
// midi_in_serial.h - raw MIDI bytes from the serial console (ESP32-P4 only,
// an addition to firmware/midi_in.h). console.c separates them from typed
// text and passes them here; they go through their own parser into the
// same queue as UART MIDI. Main task only.
//
#include <stdint.h>

void midi_in_serial_byte(uint8_t b);
