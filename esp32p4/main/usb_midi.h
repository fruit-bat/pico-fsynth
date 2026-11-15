#pragma once
//
// usb_midi.h - USB MIDI device on the ESP32-P4 high-speed USB port
// (FSYNTH_USB_MIDI). Received MIDI is read by midi_in_poll().
//
#include <stdbool.h>

// Start the USB device (TinyUSB task on core 0). Returns false on failure.
bool usb_midi_init(void);

// True while a host has the device configured.
bool usb_midi_mounted(void);
