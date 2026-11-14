//
// midi_in.c - MIDI input on the ESP32-P4: UART (DIN/TRS MIDI) parsed by
// the shared firmware/midi_parse.c into a lock-free queue for the render
// task. Implements firmware/midi_in.h, as the RP2350 firmware does. USB
// MIDI is not implemented yet.
//
// midi_in_poll(), midi_in_inject() and midi_in_settings_cmd() run in the
// main task (core 0); midi_in_pop() runs in the render task (core 1).
//
#include <string.h>
#include "esp_attr.h"
#include "driver/uart.h"
#include "sdkconfig.h"

#include "midi_in.h"

#define MIDI_UART UART_NUM_1
#define MIDI_BAUD 31250
#define QUEUE_SIZE 256  // Messages; power of 2

#define FENCE() __atomic_thread_fence(__ATOMIC_SEQ_CST)

// ---------------------------------------------------------------------------
// SPSC queue: the main task produces, the render task consumes. q_head and
// q_tail are free-running counts; the slot is count & mask.
// ---------------------------------------------------------------------------

static MidiMsg queue[QUEUE_SIZE];
static volatile uint32_t q_head;    // Written by the producer
static volatile uint32_t q_tail;    // Written by the consumer
static volatile uint32_t count;     // Messages queued (producer only)
static volatile uint32_t dropped;   // Messages lost to a full queue (producer only)

// Copy len (1..3) bytes into the next slot and publish it. Drops the
// message and counts it if the queue is full. Producer only.
static void push(const uint8_t *data, uint8_t len) {
    const uint32_t head = q_head;
    if (head - q_tail >= QUEUE_SIZE) {
        dropped = dropped + 1;
        return;
    }
    MidiMsg *m = &queue[head & (QUEUE_SIZE - 1)];
    m->len = len;
    memcpy(m->data, data, len);
    FENCE();
    q_head = head + 1;
    count = count + 1;
}

// See midi_in.h.
bool IRAM_ATTR midi_in_pop(MidiMsg *msg) {
    const uint32_t tail = q_tail;
    if (tail == q_head) return false;
    FENCE();
    *msg = queue[tail & (QUEUE_SIZE - 1)];
    FENCE();
    q_tail = tail + 1;
    return true;
}

// See midi_in.h.
void midi_in_inject(const uint8_t *data, uint8_t len) {
    if (len >= 1 && len <= 3) push(data, len);
}

uint32_t midi_in_count(void) { return count; }
uint32_t midi_in_dropped(void) { return dropped; }

// ---------------------------------------------------------------------------

static MidiByteParser uart_parser;
static volatile uint8_t settings_cmd;   // Latest settings command, 0 = none
static bool uart_ok;

// See midi_in.h.
uint8_t midi_in_settings_cmd(void) {
    const uint8_t c = settings_cmd;
    settings_cmd = 0;
    return c;
}

// Parser output: settings commands are kept for the main loop, everything
// else is queued for the render task.
static void on_message(void *ctx, const uint8_t *msg, uint8_t len) {
    (void)ctx;
    if (msg[0] == MIDI_MSG_SETTINGS) {
        if (len == 2) settings_cmd = msg[1];
        return;
    }
    push(msg, len);
}

// See midi_in.h. 31250 baud, 8N1, RX only, on the pin set in menuconfig
// (-1 = no UART MIDI).
void midi_in_init(void) {
    midi_byte_parser_init(&uart_parser, on_message, NULL);
#if CONFIG_FSYNTH_MIDI_UART_RX_PIN >= 0
    const uart_config_t cfg = {
        .baud_rate = MIDI_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_ok = uart_driver_install(MIDI_UART, 512, 0, 0, NULL, 0) == ESP_OK
           && uart_param_config(MIDI_UART, &cfg) == ESP_OK
           && uart_set_pin(MIDI_UART, UART_PIN_NO_CHANGE, CONFIG_FSYNTH_MIDI_UART_RX_PIN,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) == ESP_OK;
#endif
}

// See midi_in.h. The driver buffers 512 bytes (160 ms of MIDI) in its
// interrupt, so polling every millisecond or so is plenty.
void midi_in_poll(void) {
    if (!uart_ok) return;
    uint8_t buf[64];
    int n;
    while ((n = uart_read_bytes(MIDI_UART, buf, sizeof(buf), 0)) > 0) {
        for (int i = 0; i < n; ++i) midi_parse_byte(&uart_parser, buf[i]);
    }
}
