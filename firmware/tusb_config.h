#pragma once
//
// tusb_config.h - TinyUSB device configuration: CDC (stdio) + MIDI composite.
//
// Read by TinyUSB at compile time. The CDC interface carries the console
// (printf status lines); the MIDI interface carries MIDI in (and an unused
// MIDI out). The descriptors that match this configuration are in
// usb_descriptors.c. CFG_TUSB_MCU is supplied by the Pico SDK build.
//

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#define CFG_TUSB_RHPORT0_MODE   OPT_MODE_DEVICE
#define CFG_TUSB_OS             OPT_OS_PICO
#define CFG_TUSB_DEBUG          0

#define CFG_TUD_ENABLED         1
#define CFG_TUD_ENDPOINT0_SIZE  64

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN      __attribute__ ((aligned(4)))

// Device classes: one CDC and one MIDI interface, nothing else
#define CFG_TUD_CDC             1
#define CFG_TUD_MSC             0
#define CFG_TUD_HID             0
#define CFG_TUD_MIDI            1
#define CFG_TUD_VENDOR          0

// FIFO sizes in bytes. The CDC TX buffer bounds how much printf output can
// be queued before stdio blocks or drops.
#define CFG_TUD_CDC_RX_BUFSIZE  64
#define CFG_TUD_CDC_TX_BUFSIZE  256
#define CFG_TUD_CDC_EP_BUFSIZE  64

// MIDI FIFO sizes in bytes (16 four-byte USB MIDI event packets). Drained by
// midi_in_poll() on core 0.
#define CFG_TUD_MIDI_RX_BUFSIZE 64
#define CFG_TUD_MIDI_TX_BUFSIZE 64
