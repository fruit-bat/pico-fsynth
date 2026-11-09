//
// midi_in.c - MIDI input: reads the UART and USB MIDI, parses them
// (midi_parse.c) and feeds a lock-free queue to the render core.
//
// Everything except midi_in_pop() runs on core 0. The parsing itself is in
// midi_parse.c, free of SDK calls so it can be tested on the host; this
// file connects it to the hardware and the queue. See midi_in.h for the
// interface.
//
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/sync.h"
#include "tusb.h"

#include "midi_in.h"

#define MIDI_UART uart1
#define MIDI_BAUD 31250
#define QUEUE_SIZE 256  // Messages; power of 2

// ---------------------------------------------------------------------------
// SPSC queue: core 0 produces, the render core consumes
// ---------------------------------------------------------------------------

// q_head and q_tail are free-running counts; the slot is count & mask.
// The queue is full when head - tail == QUEUE_SIZE.
static MidiMsg queue[QUEUE_SIZE];
static volatile uint32_t q_head;    // Written by the producer
static volatile uint32_t q_tail;    // Written by the consumer
static volatile uint32_t count;     // Messages queued (producer only)
static volatile uint32_t dropped;   // Messages lost to a full queue (producer only)

// Producer side: copy len (1..3) bytes into the next slot and publish it.
// Drops the message and counts it if the queue is full. Core 0 only.
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

// See midi_in.h. The first barrier orders the slot read after the head
// read; the second makes sure the slot is copied before it is released.
bool __not_in_flash_func(midi_in_pop)(MidiMsg *msg) {
    const uint32_t tail = q_tail;
    if (tail == q_head) return false;
    __dmb();
    *msg = queue[tail & (QUEUE_SIZE - 1)];
    __dmb();
    q_tail = tail + 1;
    return true;
}

// See midi_in.h.
void midi_in_inject(const uint8_t *data, uint8_t len) {
    if (len >= 1 && len <= 3) push(data, len);
}

// See midi_in.h.
uint32_t midi_in_count(void) { return count; }
uint32_t midi_in_dropped(void) { return dropped; }

// ---------------------------------------------------------------------------
// Parsers (midi_parse.c)
// ---------------------------------------------------------------------------

static MidiByteParser uart_parser;
static MidiUsbParser usb_parser;

// Latest settings command not yet collected (0 = none). Written and read on
// core 0 only; a second command before collection replaces the first. That
// loses nothing: save and erase each replace the stored settings
// completely, so only the last command decides the final state.
static volatile uint8_t settings_cmd;

// See midi_in.h.
uint8_t midi_in_settings_cmd(void) {
    const uint8_t c = settings_cmd;
    settings_cmd = 0;
    return c;
}

// Parser output (both ports): settings commands are kept for the main
// loop, everything else is queued for the render core. Core 0.
static void on_message(void *ctx, const uint8_t *msg, uint8_t len) {
    (void)ctx;
    if (msg[0] == MIDI_MSG_SETTINGS) {
        if (len == 2) settings_cmd = msg[1];
        return;
    }
    push(msg, len);
}

// ---------------------------------------------------------------------------

// See midi_in.h. 31250 baud, 8N1; TX is not used.
void midi_in_init(void) {
    midi_byte_parser_init(&uart_parser, on_message, NULL);
    midi_usb_parser_init(&usb_parser, on_message, NULL);
    uart_init(MIDI_UART, MIDI_BAUD);
    uart_set_format(MIDI_UART, 8, 1, UART_PARITY_NONE);
    gpio_set_function(FS_MIDI_UART_RX_PIN, GPIO_FUNC_UART);
    gpio_pull_up(FS_MIDI_UART_RX_PIN); // Idle high if nothing is connected
}

// See midi_in.h. The UART FIFO is 32 bytes (about 10 ms of MIDI), so this
// must run at least that often to avoid losing UART bytes.
void midi_in_poll(void) {
    while (uart_is_readable(MIDI_UART)) {
        midi_parse_byte(&uart_parser, (uint8_t)uart_getc(MIDI_UART));
    }
    uint8_t packet[4];
    while (tud_midi_available() && tud_midi_packet_read(packet)) {
        midi_parse_usb_packet(&usb_parser, packet);
    }
}
