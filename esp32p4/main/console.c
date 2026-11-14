//
// console.c - serial console: typed commands and raw MIDI. See console.h.
//
// Commands (type them in `idf.py monitor`):
//   stress N [P]  hold sustained notes until N voices are playing
//                 (program P, default 16 = Organ 1, on channels 1-9 and 11-16)
//   off           all sound off on every channel
//   reset         GM reset (programs, controllers, sound off)
//   help          list the commands
//
// Commands go through the MIDI queue (midi_in_inject), so they behave
// exactly like MIDI input: the first one stops the demo song.
//
// Raw MIDI on the same port: typed text is printable ASCII, so a byte of
// 0x80 or more can only be a MIDI status byte. It and the data bytes that
// complete its message go to the MIDI parser (midi_in_serial.h); anything
// else is text. Senders must therefore not use running status (every
// message starts with its status byte). host/serial_midi.c does that.
// SysEx runs until F7; real-time bytes (F8-FF) may appear anywhere.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "sdkconfig.h"

#include "console.h"
#include "midi_in.h"
#include "midi_in_serial.h"

#define CONSOLE_UART CONFIG_ESP_CONSOLE_UART_NUM

static uint32_t (*voices)(void);
static char line[64];
static unsigned line_len;
static unsigned midi_left;      // Data bytes still due for the current MIDI message
static bool in_sysex;

void console_init(uint32_t (*active_voices)(void)) {
    voices = active_voices;
    // Interrupt driven receive with a 1 KB buffer, so MIDI bursts are not
    // lost between polls; printf keeps working through the driver
    uart_driver_install(CONSOLE_UART, 1024, 0, 0, NULL, 0);
    uart_vfs_dev_use_driver(CONSOLE_UART);
    printf("console: type 'help' for commands\n");
}

// Wait about ms milliseconds, keeping MIDI input serviced.
static void wait_ms(uint32_t ms) {
    for (uint32_t i = 0; i < ms; ++i) {
        midi_in_poll();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static void all_off(void) {
    for (uint8_t ch = 0; ch < 16; ++ch) {
        const uint8_t off[3] = { (uint8_t)(0xb0 | ch), 120, 0 };   // All Sound Off
        midi_in_inject(off, 3);
    }
}

// Hold notes, one at a time, until the synth reports `target` voices or
// every key is in use. Each note may start more than one voice (layered
// presets), so the count is checked rather than assumed.
static void stress(uint32_t target, uint8_t program) {
    all_off();
    for (uint8_t ch = 0; ch < 16; ++ch) {
        if (ch == 9) continue;   // Percussion
        const uint8_t pc[2] = { (uint8_t)(0xc0 | ch), program };
        midi_in_inject(pc, 2);
    }
    wait_ms(20);
    uint32_t notes = 0;
    for (uint8_t key = 36; key < 96 && voices() < target; ++key) {
        for (uint8_t ch = 0; ch < 16 && voices() < target; ++ch) {
            if (ch == 9) continue;
            const uint8_t on[3] = { (uint8_t)(0x90 | ch), key, 90 };
            midi_in_inject(on, 3);
            ++notes;
            wait_ms(4);
        }
    }
    wait_ms(50);
    printf("stress: %lu notes held, %lu voices playing (program %u); 'off' stops them\n",
           (unsigned long)notes, (unsigned long)voices(), program);
}

static void run(char *cmd) {
    char *arg = strchr(cmd, ' ');
    if (arg) *arg++ = 0;
    if (!strcmp(cmd, "stress")) {
        char *arg2 = arg ? strchr(arg, ' ') : NULL;
        const long n = arg ? strtol(arg, NULL, 10) : 0;
        const long p = arg2 ? strtol(arg2, NULL, 10) : 16;
        if (n < 1 || p < 0 || p > 127) printf("usage: stress VOICES [PROGRAM]\n");
        else stress((uint32_t)n, (uint8_t)p);
    } else if (!strcmp(cmd, "off")) {
        all_off();
        printf("all sound off\n");
    } else if (!strcmp(cmd, "reset")) {
        const uint8_t r = MIDI_MSG_RESET;
        midi_in_inject(&r, 1);
        printf("reset\n");
    } else if (!strcmp(cmd, "help")) {
        printf("commands: stress VOICES [PROGRAM], off, reset, help\n");
    } else if (cmd[0]) {
        printf("unknown command '%s' (try help)\n", cmd);
    }
}

// Data bytes that follow a status byte (no running status here).
static unsigned midi_data_bytes(uint8_t status) {
    switch (status & 0xf0) {
    case 0xc0: case 0xd0: return 1;
    case 0xf0:
        return status == 0xf2 ? 2 : (status == 0xf1 || status == 0xf3) ? 1 : 0;
    default: return 2;      // 8x 9x Ax Bx Ex
    }
}

// Route one byte: MIDI (see the file header) or text. Returns true if it
// was MIDI.
static bool midi_byte(uint8_t b) {
    if (b >= 0xf8) {                    // Real time: anywhere, no state change
        midi_in_serial_byte(b);
        return true;
    }
    if (b >= 0x80) {                    // Status byte starts a message
        in_sysex = b == 0xf0;
        midi_left = in_sysex ? 0 : midi_data_bytes(b);
        midi_in_serial_byte(b);
        return true;
    }
    if (in_sysex || midi_left) {        // Data byte of the current message
        if (midi_left) --midi_left;
        midi_in_serial_byte(b);
        return true;
    }
    return false;
}

void console_poll(void) {
    uint8_t buf[64];
    int n;
    while ((n = uart_read_bytes(CONSOLE_UART, buf, sizeof(buf), 0)) > 0) {
        for (int i = 0; i < n; ++i) {
            const uint8_t c = buf[i];
            if (midi_byte(c)) continue;
            if (c == '\r' || c == '\n') {
                if (line_len == 0) continue;
                putchar('\n');
                line[line_len] = 0;
                line_len = 0;
                run(line);
            } else if ((c == 8 || c == 127) && line_len) {
                --line_len;
                fputs("\b \b", stdout);
            } else if (c >= ' ' && c < 127 && line_len < sizeof(line) - 1) {
                line[line_len++] = (char)c;
                putchar(c);
            }
        }
        fflush(stdout);
    }
}
