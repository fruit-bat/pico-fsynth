#include <string.h>
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/sync.h"
#include "tusb.h"

#include "midi_in.h"

#define MIDI_UART uart1
#define MIDI_BAUD 31250
#define QUEUE_SIZE 256  // Power of 2

// ---------------------------------------------------------------------------
// SPSC queue: core 0 produces, the render core consumes
// ---------------------------------------------------------------------------

static MidiMsg queue[QUEUE_SIZE];
static volatile uint32_t q_head;    // Written by the producer
static volatile uint32_t q_tail;    // Written by the consumer
static volatile uint32_t count;
static volatile uint32_t dropped;

static void push(const uint8_t *data, uint8_t len) {
    const uint32_t head = q_head;
    if (head - q_tail >= QUEUE_SIZE) {
        dropped = dropped + 1;
        return;
    }
    MidiMsg *m = &queue[head & (QUEUE_SIZE - 1)];
    m->len = len;
    memcpy(m->data, data, len);
    __dmb();
    q_head = head + 1;
    count = count + 1;
}

bool __not_in_flash_func(midi_in_pop)(MidiMsg *msg) {
    const uint32_t tail = q_tail;
    if (tail == q_head) return false;
    __dmb();
    *msg = queue[tail & (QUEUE_SIZE - 1)];
    __dmb();
    q_tail = tail + 1;
    return true;
}

void midi_in_inject(const uint8_t *data, uint8_t len) {
    if (len >= 1 && len <= 3) push(data, len);
}

uint32_t midi_in_count(void) { return count; }
uint32_t midi_in_dropped(void) { return dropped; }

// ---------------------------------------------------------------------------
// Byte stream parser (UART), with running status and SysEx detection
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t status;         // Running status (0 = none)
    uint8_t data[2];
    uint8_t have;
    uint8_t need;
    bool in_sysex;
    uint8_t sysex[16];
    uint8_t sysex_len;
} ByteParser;

static ByteParser uart_parser;

static uint8_t data_bytes(uint8_t status) {
    switch (status & 0xf0) {
        case 0xc0: case 0xd0: return 1;
        default: return 2;
    }
}

// GM system on (F0 7E xx 09 01 F7) or GS reset (F0 41 xx 42 12 40 00 7F 00 41 F7)
static bool is_reset_sysex(const uint8_t *s, uint32_t len) {
    if (len >= 6 && s[1] == 0x7e && s[3] == 0x09 && (s[4] == 0x01 || s[4] == 0x03)) return true;
    if (len >= 10 && s[1] == 0x41 && s[3] == 0x42 && s[4] == 0x12 && s[5] == 0x40 && s[6] == 0x00 && s[7] == 0x7f) return true;
    return false;
}

// GM master volume (F0 7F dev 04 01 lsb msb F7)
static bool is_master_volume_sysex(const uint8_t *s, uint32_t len) {
    return len >= 8 && s[1] == 0x7f && s[3] == 0x04 && s[4] == 0x01;
}

// GS master EQ (F0 41 dev 42 12 40 02 0p vv sum F7), checksum checked
static bool is_eq_sysex(const uint8_t *s, uint32_t len) {
    return len >= 11 && s[1] == 0x41 && s[3] == 0x42 && s[4] == 0x12 && s[5] == 0x40 && s[6] == 0x02 &&
           s[7] <= 0x03 && ((s[5] + s[6] + s[7] + s[8] + s[9]) & 0x7f) == 0;
}

static volatile uint8_t settings_cmd;

uint8_t midi_in_settings_cmd(void) {
    const uint8_t c = settings_cmd;
    settings_cmd = 0;
    return c;
}

static void sysex_end(const uint8_t *s, uint32_t len) {
    if (len >= 6 && s[1] == 0x7d && s[2] == 0x46 && s[3] == 0x53 && s[5] == 0xf7 &&
        (s[4] == MIDI_SETTINGS_SAVE || s[4] == MIDI_SETTINGS_ERASE)) {
        settings_cmd = s[4];
        return;
    }
    if (is_reset_sysex(s, len)) {
        const uint8_t r = MIDI_MSG_RESET;
        push(&r, 1);
    } else if (is_master_volume_sysex(s, len)) {
        const uint8_t m[3] = { MIDI_MSG_MASTER_VOLUME, (uint8_t)(s[5] & 0x7f), (uint8_t)(s[6] & 0x7f) };
        push(m, 3);
    } else if (is_eq_sysex(s, len)) {
        const uint8_t m[3] = { MIDI_MSG_EQ, s[7], (uint8_t)(s[8] & 0x7f) };
        push(m, 3);
    }
}

static void parse_byte(ByteParser *p, uint8_t b) {
    if (b >= 0xf8) return; // Real-time messages (clock, active sensing...) ignored

    if (b & 0x80) {
        if (p->in_sysex) {
            p->in_sysex = false;
            if (b == 0xf7 && p->sysex_len < sizeof(p->sysex)) {
                p->sysex[p->sysex_len++] = b;
                sysex_end(p->sysex, p->sysex_len);
            }
            if (b == 0xf7) return;
        }
        if (b == 0xf0) {
            p->in_sysex = true;
            p->sysex[0] = b;
            p->sysex_len = 1;
            p->status = 0;
            return;
        }
        if (b >= 0xf0) {
            p->status = 0; // System common: cancels running status, ignored
            return;
        }
        p->status = b;
        p->have = 0;
        p->need = data_bytes(b);
        return;
    }

    if (p->in_sysex) {
        if (p->sysex_len < sizeof(p->sysex)) p->sysex[p->sysex_len++] = b;
        return;
    }
    if (!p->status) return; // Data without status
    p->data[p->have++] = b;
    if (p->have == p->need) {
        const uint8_t m[3] = { p->status, p->data[0], p->data[1] };
        push(m, (uint8_t)(1 + p->need));
        p->have = 0; // Running status
    }
}

// ---------------------------------------------------------------------------
// USB MIDI: 4-byte event packets
// ---------------------------------------------------------------------------

static uint8_t usb_sysex[16];
static uint8_t usb_sysex_len;

static void usb_sysex_add(const uint8_t *b, int n, bool end) {
    for (int i = 0; i < n; ++i) {
        if (usb_sysex_len < sizeof(usb_sysex)) usb_sysex[usb_sysex_len++] = b[i];
    }
    if (end) {
        sysex_end(usb_sysex, usb_sysex_len);
        usb_sysex_len = 0;
    }
}

static void usb_packet(const uint8_t p[4]) {
    const uint8_t cin = p[0] & 0x0f;
    switch (cin) {
        case 0x4: usb_sysex_add(&p[1], 3, false); break;  // SysEx starts or continues
        case 0x5: usb_sysex_add(&p[1], 1, true); break;   // SysEx ends with 1 byte
        case 0x6: usb_sysex_add(&p[1], 2, true); break;   // ... 2 bytes
        case 0x7: usb_sysex_add(&p[1], 3, true); break;   // ... 3 bytes
        case 0x8: case 0x9: case 0xa: case 0xb: case 0xe:
            push(&p[1], 3);
            break;
        case 0xc: case 0xd:
            push(&p[1], 2);
            break;
        default:
            break; // System common / real-time / reserved
    }
}

// ---------------------------------------------------------------------------

void midi_in_init(void) {
    uart_init(MIDI_UART, MIDI_BAUD);
    uart_set_format(MIDI_UART, 8, 1, UART_PARITY_NONE);
    gpio_set_function(FS_MIDI_UART_RX_PIN, GPIO_FUNC_UART);
    gpio_pull_up(FS_MIDI_UART_RX_PIN); // Idle high if nothing is connected
}

void midi_in_poll(void) {
    while (uart_is_readable(MIDI_UART)) {
        parse_byte(&uart_parser, (uint8_t)uart_getc(MIDI_UART));
    }
    uint8_t packet[4];
    while (tud_midi_available() && tud_midi_packet_read(packet)) {
        usb_packet(packet);
    }
}
