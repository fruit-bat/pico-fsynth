//
// midi_parse.c - UART byte stream and USB MIDI packet parsers. See
// midi_parse.h.
//
// Both parsers share the SysEx collection and recognition code, so a given
// SysEx message has the same effect whichever port it arrives on.
//
#include <string.h>
#include "midi_parse.h"

// ---------------------------------------------------------------------------
// SysEx collection and recognition
// ---------------------------------------------------------------------------

// The is_*() tests take a complete SysEx message s[0..len), F0 to F7, and
// accept any device ID (s[2]). Lengths must match exactly.

// GM system on (F0 7E dev 09 01 F7), GM2 system on (09 03), or GS reset
// (F0 41 dev 42 12 40 00 7F 00 41 F7)
static bool is_reset(const uint8_t *s, uint32_t len) {
    if (len == 6 && s[1] == 0x7e && s[3] == 0x09 && (s[4] == 0x01 || s[4] == 0x03)) return true;
    if (len == 11 && s[1] == 0x41 && s[3] == 0x42 && s[4] == 0x12 && s[5] == 0x40 && s[6] == 0x00 && s[7] == 0x7f) return true;
    return false;
}

// GM master volume (F0 7F dev 04 01 lsb msb F7)
static bool is_master_volume(const uint8_t *s, uint32_t len) {
    return len == 8 && s[1] == 0x7f && s[3] == 0x04 && s[4] == 0x01;
}

// GS master EQ (F0 41 dev 42 12 40 02 0p vv sum F7), Roland checksum checked
static bool is_eq(const uint8_t *s, uint32_t len) {
    return len == 11 && s[1] == 0x41 && s[3] == 0x42 && s[4] == 0x12 && s[5] == 0x40 && s[6] == 0x02 &&
           s[7] <= 0x03 && ((s[5] + s[6] + s[7] + s[8] + s[9]) & 0x7f) == 0;
}

// pico-fsynth settings command (F0 7D 46 53 cmd F7)
static bool is_settings(const uint8_t *s, uint32_t len) {
    return len == 6 && s[1] == 0x7d && s[2] == 0x46 && s[3] == 0x53 &&
           (s[4] == MIDI_SETTINGS_SAVE || s[4] == MIDI_SETTINGS_ERASE);
}

// Act on a complete SysEx message: emit the internal code if it is one we
// understand, otherwise nothing.
static void sysex_dispatch(const uint8_t *s, uint32_t len, MidiEmit emit, void *ctx) {
    if (len < 2 || s[0] != 0xf0 || s[len - 1] != 0xf7) return;
    if (is_settings(s, len)) {
        const uint8_t m[2] = { MIDI_MSG_SETTINGS, s[4] };
        emit(ctx, m, 2);
    } else if (is_reset(s, len)) {
        const uint8_t r = MIDI_MSG_RESET;
        emit(ctx, &r, 1);
    } else if (is_master_volume(s, len)) {
        const uint8_t m[3] = { MIDI_MSG_MASTER_VOLUME, (uint8_t)(s[5] & 0x7f), (uint8_t)(s[6] & 0x7f) };
        emit(ctx, m, 3);
    } else if (is_eq(s, len)) {
        const uint8_t m[3] = { MIDI_MSG_EQ, s[7], (uint8_t)(s[8] & 0x7f) };
        emit(ctx, m, 3);
    }
}

// Start collecting a new SysEx (on F0), dropping any unfinished one.
static void sysex_start(MidiSysex *x) {
    x->buf[0] = 0xf0;
    x->len = 1;
    x->active = true;
    x->overflow = false;
}

// Append a byte to the SysEx being collected; past the buffer, remember
// that the message is too long.
static void sysex_add(MidiSysex *x, uint8_t b) {
    if (x->len < sizeof(x->buf)) x->buf[x->len++] = b;
    else x->overflow = true;
}

// The SysEx is complete: dispatch it unless it overflowed, and stop
// collecting.
static void sysex_finish(MidiSysex *x, MidiEmit emit, void *ctx) {
    if (x->active && !x->overflow) sysex_dispatch(x->buf, x->len, emit, ctx);
    x->active = false;
}

// ---------------------------------------------------------------------------
// Byte stream parser (UART)
// ---------------------------------------------------------------------------

// See midi_parse.h.
void midi_byte_parser_init(MidiByteParser *p, MidiEmit emit, void *ctx) {
    memset(p, 0, sizeof(*p));
    p->emit = emit;
    p->ctx = ctx;
}

// Number of data bytes after a channel status byte: 1 for program change
// (Cx) and channel pressure (Dx), 2 for the rest.
static uint8_t data_bytes(uint8_t status) {
    switch (status & 0xf0) {
        case 0xc0: case 0xd0: return 1;
        default: return 2;
    }
}

// See midi_parse.h. A status byte other than F7 during a SysEx abandons it.
void midi_parse_byte(MidiByteParser *p, uint8_t b) {
    if (b >= 0xf8) return; // Real-time messages (clock, active sensing...) ignored

    if (b & 0x80) {
        if (p->sysex.active) {
            if (b == 0xf7) {
                sysex_add(&p->sysex, b);
                sysex_finish(&p->sysex, p->emit, p->ctx);
                return;
            }
            p->sysex.active = false; // Cut short: dropped
        }
        if (b == 0xf0) {
            sysex_start(&p->sysex);
            p->status = 0;
            return;
        }
        if (b >= 0xf0) {
            p->status = 0; // System common (or a stray F7): cancels running status, ignored
            return;
        }
        p->status = b;
        p->have = 0;
        p->need = data_bytes(b);
        return;
    }

    if (p->sysex.active) {
        sysex_add(&p->sysex, b);
        return;
    }
    if (!p->status) return; // Data without status
    p->data[p->have++] = b;
    if (p->have == p->need) {
        const uint8_t m[3] = { p->status, p->data[0], p->data[1] };
        p->emit(p->ctx, m, (uint8_t)(1 + p->need));
        p->have = 0; // Running status
    }
}

// ---------------------------------------------------------------------------
// USB MIDI event packets
// ---------------------------------------------------------------------------

// See midi_parse.h.
void midi_usb_parser_init(MidiUsbParser *p, MidiEmit emit, void *ctx) {
    memset(p, 0, sizeof(*p));
    p->emit = emit;
    p->ctx = ctx;
}

// SysEx bytes from one packet. An F0 always starts a new message (so a
// message whose end packet was lost cannot run into the next one). With
// end, the message is complete. Bytes outside a SysEx are ignored (CIN 5
// is also used for one-byte system common messages, such as F6).
static void usb_sysex(MidiUsbParser *p, const uint8_t *b, int n, bool end) {
    for (int i = 0; i < n; ++i) {
        if (b[i] == 0xf0) sysex_start(&p->sysex);
        else if (p->sysex.active) sysex_add(&p->sysex, b[i]);
    }
    if (end) sysex_finish(&p->sysex, p->emit, p->ctx);
}

// See midi_parse.h.
void midi_parse_usb_packet(MidiUsbParser *p, const uint8_t packet[4]) {
    const uint8_t cin = packet[0] & 0x0f;
    switch (cin) {
        case 0x4: usb_sysex(p, &packet[1], 3, false); break;  // SysEx starts or continues
        case 0x5: usb_sysex(p, &packet[1], 1, true); break;   // SysEx ends with 1 byte
        case 0x6: usb_sysex(p, &packet[1], 2, true); break;   // ... 2 bytes
        case 0x7: usb_sysex(p, &packet[1], 3, true); break;   // ... 3 bytes
        case 0x8: case 0x9: case 0xa: case 0xb: case 0xe:
            p->emit(p->ctx, &packet[1], 3);
            break;
        case 0xc: case 0xd:
            p->emit(p->ctx, &packet[1], 2);
            break;
        default:
            break; // System common / real-time / reserved
    }
}
