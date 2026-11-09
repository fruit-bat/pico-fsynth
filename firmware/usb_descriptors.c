//
// usb_descriptors.c - USB descriptors: composite CDC (serial console) + MIDI.
//
// TinyUSB calls the tud_descriptor_*_cb() callbacks below (from tud_task()
// on core 0) when the host enumerates the device. Interface 0-1 are the
// CDC console used by stdio; 2-3 are the MIDI function that midi_in.c reads.
// The class configuration they must match is in tusb_config.h.
//
#include <string.h>
#include "pico/unique_id.h"
#include "tusb.h"

// Test VID/PID as used by the TinyUSB examples; change before distributing
#define USB_VID 0xCafe
#define USB_PID 0x4F53  // "FS"

static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    // Interface Association Descriptors are used for the CDC function
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

// TinyUSB callback: return the device descriptor.
const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&desc_device;
}

// Interface numbers, in descriptor order
enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_MIDI,
    ITF_NUM_MIDI_STREAMING,
    ITF_NUM_TOTAL
};

// Endpoint addresses (0x80 bit = IN, device to host)
#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT   0x02
#define EPNUM_CDC_IN    0x82
#define EPNUM_MIDI_OUT  0x03
#define EPNUM_MIDI_IN   0x83

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MIDI_DESC_LEN)

// Configuration 1: bus powered, 100 mA, CDC then MIDI. The numbers 4 and 5
// in the interface descriptors are string indices (see string_desc).
static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
    TUD_MIDI_DESCRIPTOR(ITF_NUM_MIDI, 5, EPNUM_MIDI_OUT, EPNUM_MIDI_IN, 64),
};

// TinyUSB callback: return the configuration descriptor (there is only one,
// so the index is ignored).
const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

// String descriptors by index, as ASCII; converted to UTF-16 on request
static const char *const string_desc[] = {
    NULL,               // 0: language (handled below)
    "fruit-bat",        // 1: Manufacturer
    "pico-fsynth",      // 2: Product
    NULL,               // 3: Serial (board unique id)
    "pico-fsynth CDC",  // 4: CDC interface
    "pico-fsynth MIDI", // 5: MIDI interface
};

// Reply buffer for string requests: a header word plus up to 32 UTF-16
// characters. Must stay valid after the callback returns, hence static.
static uint16_t desc_str[32 + 1];

// TinyUSB callback: return string descriptor `index` as UTF-16 (ASCII
// widened, truncated to 32 characters), or NULL for an unknown index.
// Index 0 is the list of supported languages (English only); index 3 is
// the board's unique flash ID as hex, so each board has its own serial
// number. The language ID is ignored.
const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    uint32_t n;
    if (index == 0) {
        desc_str[1] = 0x0409; // English
        n = 1;
    } else {
        char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
        const char *s;
        if (index == 3) {
            pico_get_unique_board_id_string(serial, sizeof(serial));
            s = serial;
        } else if (index < sizeof(string_desc) / sizeof(string_desc[0])) {
            s = string_desc[index];
        } else {
            return NULL;
        }
        n = (uint32_t)strlen(s);
        if (n > 32) n = 32;
        for (uint32_t i = 0; i < n; ++i) desc_str[1 + i] = (uint16_t)s[i];
    }
    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return desc_str;
}
