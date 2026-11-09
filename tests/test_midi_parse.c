//
// test_midi_parse.c - unit tests for the firmware's MIDI input parsers
// (firmware/midi_parse.c): UART byte stream and USB MIDI packets.
//
#include <string.h>
#include "test.h"
#include "midi_parse.h"

// Messages emitted by a parser
typedef struct { uint8_t len; uint8_t data[3]; } Msg;
static Msg out[64];
static int out_count;

static void sink(void *ctx, const uint8_t *msg, uint8_t len) {
    (void)ctx;
    if (out_count < 64) {
        out[out_count].len = len;
        memcpy(out[out_count].data, msg, len);
        ++out_count;
    }
}

static MidiByteParser uart;
static MidiUsbParser usb;

static void uart_feed(const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; ++i) midi_parse_byte(&uart, b[i]);
}

// Send a byte sequence over USB the way a host does: SysEx in 3-byte
// packets (CIN 4) and an end packet (CIN 5/6/7), channel messages as one
// packet each
static void usb_feed_sysex(const uint8_t *b, size_t n) {
    size_t i = 0;
    while (n - i > 3) {
        const uint8_t p[4] = { 0x04, b[i], b[i + 1], b[i + 2] };
        midi_parse_usb_packet(&usb, p);
        i += 3;
    }
    uint8_t p[4] = { (uint8_t)(0x04 + (n - i)), 0, 0, 0 };
    memcpy(&p[1], &b[i], n - i);
    midi_parse_usb_packet(&usb, p);
}

static void reset(void) {
    out_count = 0;
    midi_byte_parser_init(&uart, sink, NULL);
    midi_usb_parser_init(&usb, sink, NULL);
}

static const uint8_t gm_on[] = { 0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7 };
static const uint8_t gs_reset[] = { 0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7 };
static const uint8_t master_vol[] = { 0xf0, 0x7f, 0x7f, 0x04, 0x01, 0x11, 0x22, 0xf7 };
static const uint8_t eq_ok[] = { 0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x02, 0x01, 0x44, 0x79, 0xf7 };
static const uint8_t settings_save[] = { 0xf0, 0x7d, 0x46, 0x53, 0x01, 0xf7 };

TEST(uart_channel_messages_and_running_status) {
    reset();
    const uint8_t b[] = { 0x90, 60, 100, 61, 101, 0xc3, 7, 8, 0xe0, 0, 64 };
    uart_feed(b, sizeof(b));
    CHECK_EQ(out_count, 5);
    CHECK_EQ(out[1].len, 3);
    CHECK_EQ(out[1].data[0], 0x90);
    CHECK_EQ(out[1].data[1], 61);
    CHECK_EQ(out[3].len, 2);
    CHECK_EQ(out[3].data[0], 0xc3);
    CHECK_EQ(out[3].data[1], 8);
    CHECK_EQ(out[4].data[0], 0xe0);
}

// Real-time bytes may appear anywhere, even inside a message or SysEx
TEST(uart_realtime_ignored) {
    reset();
    const uint8_t b[] = { 0xf8, 0x90, 0xfe, 60, 0xf8, 100, 0xf0, 0x7e, 0xf8, 0x7f, 0x09, 0x01, 0xf7 };
    uart_feed(b, sizeof(b));
    CHECK_EQ(out_count, 2);
    CHECK_EQ(out[0].data[0], 0x90);
    CHECK_EQ(out[1].len, 1);
    CHECK_EQ(out[1].data[0], MIDI_MSG_RESET);
}

// Each recognised SysEx gives the same code over UART and USB
TEST(sysex_recognised_on_both_ports) {
    const uint8_t *msgs[] = { gm_on, gs_reset, master_vol, eq_ok, settings_save };
    const size_t lens[] = { sizeof(gm_on), sizeof(gs_reset), sizeof(master_vol), sizeof(eq_ok), sizeof(settings_save) };
    const uint8_t codes[] = { MIDI_MSG_RESET, MIDI_MSG_RESET, MIDI_MSG_MASTER_VOLUME, MIDI_MSG_EQ, MIDI_MSG_SETTINGS };
    for (int port = 0; port < 2; ++port) {
        for (int i = 0; i < 5; ++i) {
            reset();
            if (port == 0) uart_feed(msgs[i], lens[i]);
            else usb_feed_sysex(msgs[i], lens[i]);
            CHECK_EQ(out_count, 1);
            CHECK_EQ(out[0].data[0], codes[i]);
        }
    }
    reset();
    uart_feed(master_vol, sizeof(master_vol));
    CHECK_EQ(out[0].len, 3);
    CHECK_EQ(out[0].data[1], 0x11);
    CHECK_EQ(out[0].data[2], 0x22);
    reset();
    uart_feed(eq_ok, sizeof(eq_ok));
    CHECK_EQ(out[0].data[1], 1);
    CHECK_EQ(out[0].data[2], 0x44);
    reset();
    uart_feed(settings_save, sizeof(settings_save));
    CHECK_EQ(out[0].len, 2);
    CHECK_EQ(out[0].data[1], MIDI_SETTINGS_SAVE);
}

TEST(eq_bad_checksum_ignored) {
    uint8_t bad[sizeof(eq_ok)];
    memcpy(bad, eq_ok, sizeof(bad));
    bad[9] ^= 1;
    reset();
    uart_feed(bad, sizeof(bad));
    usb_feed_sysex(bad, sizeof(bad));
    CHECK_EQ(out_count, 0);
}

// Regression: a long SysEx that starts like GM System On must not reset
// the synth. USB used to act on the first 16 bytes of a longer message.
TEST(long_sysex_not_acted_on) {
    uint8_t lng[40];
    memcpy(lng, gm_on, 5);
    for (int i = 5; i < 39; ++i) lng[i] = 0x11;
    lng[39] = 0xf7;
    reset();
    uart_feed(lng, sizeof(lng));
    CHECK_EQ(out_count, 0);
    reset();
    usb_feed_sysex(lng, sizeof(lng));
    CHECK_EQ(out_count, 0);
    // A message of 6..16 bytes that only starts like one is not one either
    const uint8_t longer[] = { 0xf0, 0x7e, 0x7f, 0x09, 0x01, 0x00, 0xf7 };
    reset();
    uart_feed(longer, sizeof(longer));
    usb_feed_sysex(longer, sizeof(longer));
    CHECK_EQ(out_count, 0);
}

// Regression: a USB SysEx whose end packet is lost must not run into the
// next SysEx; the new F0 starts afresh.
TEST(usb_new_f0_restarts_sysex) {
    reset();
    const uint8_t start[4] = { 0x04, 0xf0, 0x43, 0x10 };  // Never finished
    midi_parse_usb_packet(&usb, start);
    usb_feed_sysex(gm_on, sizeof(gm_on));
    CHECK_EQ(out_count, 1);
    CHECK_EQ(out[0].data[0], MIDI_MSG_RESET);
}

// CIN 5 also carries single-byte system common messages; outside a SysEx
// they are ignored, and an F7 alone does nothing
TEST(usb_cin5_outside_sysex) {
    reset();
    const uint8_t tune[4] = { 0x05, 0xf6, 0, 0 };
    const uint8_t f7[4] = { 0x05, 0xf7, 0, 0 };
    midi_parse_usb_packet(&usb, tune);
    midi_parse_usb_packet(&usb, f7);
    CHECK_EQ(out_count, 0);
}

TEST(usb_channel_messages) {
    reset();
    const uint8_t on[4] = { 0x09, 0x91, 60, 100 };
    const uint8_t pc[4] = { 0x0c, 0xc1, 5, 0 };
    const uint8_t clock[4] = { 0x0f, 0xf8, 0, 0 };
    midi_parse_usb_packet(&usb, on);
    midi_parse_usb_packet(&usb, pc);
    midi_parse_usb_packet(&usb, clock);
    CHECK_EQ(out_count, 2);
    CHECK_EQ(out[0].len, 3);
    CHECK_EQ(out[0].data[1], 60);
    CHECK_EQ(out[1].len, 2);
    CHECK_EQ(out[1].data[1], 5);
}

// A SysEx cut short by another status byte is dropped, and that status
// byte still starts its message
TEST(uart_sysex_cut_short) {
    reset();
    const uint8_t b[] = { 0xf0, 0x7e, 0x7f, 0x09, 0x90, 60, 100 };
    uart_feed(b, sizeof(b));
    CHECK_EQ(out_count, 1);
    CHECK_EQ(out[0].data[0], 0x90);
}

int main(void) {
    RUN(uart_channel_messages_and_running_status);
    RUN(uart_realtime_ignored);
    RUN(sysex_recognised_on_both_ports);
    RUN(eq_bad_checksum_ignored);
    RUN(long_sysex_not_acted_on);
    RUN(usb_new_f0_restarts_sysex);
    RUN(usb_cin5_outside_sysex);
    RUN(usb_channel_messages);
    RUN(uart_sysex_cut_short);
    return test_summary();
}
