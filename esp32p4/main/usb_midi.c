//
// usb_midi.c - USB MIDI device on the ESP32-P4's high-speed USB port (the
// 4-pin V / D- / D+ / G header on the P4-Pico), using TinyUSB through
// Espressif's esp_tinyusb component (main/idf_component.yml). Built when
// FSYNTH_USB_MIDI is set in menuconfig.
//
// The device looks like the RP2350 firmware's MIDI function: same test
// VID/PID and the product name "pico-fsynth", so the host/*.sh scripts find
// it with `amidi -l`. There is no USB serial console: the console stays on
// the Type-C UART.
//
// TinyUSB runs in its own task, pinned to core 0 at a low priority, well
// away from the render task on core 1. midi_in_poll() (main task) reads
// the received packets; see midi_in.c.
//
#include <stdio.h>
#include "esp_mac.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"

#include "usb_midi.h"

// Test VID/PID as used by the TinyUSB examples and the RP2350 firmware;
// change before distributing
#define USB_VID 0xCafe
#define USB_PID 0x4F53  // "FS"

enum { ITF_NUM_MIDI = 0, ITF_NUM_MIDI_STREAMING, ITF_NUM_TOTAL };
#define EPNUM_MIDI_OUT 0x01
#define EPNUM_MIDI_IN  0x81
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MIDI_DESC_LEN)

static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,     // Class given per interface
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

// Configuration 1: bus powered, 100 mA, one MIDI function. Bulk endpoints
// are 64 bytes at full speed and 512 at high speed, as USB requires (the
// string index 4 names the MIDI interface).
static const uint8_t desc_fs_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_MIDI_DESCRIPTOR(ITF_NUM_MIDI, 4, EPNUM_MIDI_OUT, EPNUM_MIDI_IN, 64),
};
#if TUD_OPT_HIGH_SPEED
static const uint8_t desc_hs_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_MIDI_DESCRIPTOR(ITF_NUM_MIDI, 4, EPNUM_MIDI_OUT, EPNUM_MIDI_IN, 512),
};
#endif

static char serial[13];     // MAC address as 12 hex digits

static const char *strings[] = {
    (const char[]){ 0x09, 0x04 },   // 0: language, English (0x0409)
    "fruit-bat",                    // 1: manufacturer
    "pico-fsynth",                  // 2: product
    serial,                         // 3: serial number
    "pico-fsynth MIDI",             // 4: MIDI interface
};

bool usb_midi_init(void) {
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_BASE);
    snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG();
    cfg.task.xCoreID = 0;       // Not the render core
    cfg.task.priority = 5;      // Below the render tasks, above the main task
    cfg.descriptor.device = &desc_device;
    cfg.descriptor.string = strings;
    cfg.descriptor.string_count = sizeof(strings) / sizeof(strings[0]);
    cfg.descriptor.full_speed_config = desc_fs_config;
#if TUD_OPT_HIGH_SPEED
    cfg.descriptor.high_speed_config = desc_hs_config;
    cfg.descriptor.qualifier = NULL;
#endif
    return tinyusb_driver_install(&cfg) == ESP_OK;
}

bool usb_midi_mounted(void) {
    return tud_midi_mounted();
}
