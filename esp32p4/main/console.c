//
// console.c - serial console commands. See console.h.
//
//   stress N [P]  hold sustained notes until N voices are playing
//                 (program P, default 16 = Organ 1, on channels 1-9 and 11-16)
//   off           all sound off on every channel
//   reset         GM reset (programs, controllers, sound off)
//   help          list the commands
//
// Commands go through the MIDI queue (midi_in_inject), so they behave
// exactly like MIDI input: the first one stops the demo song.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "console.h"
#include "midi_in.h"

static uint32_t (*voices)(void);
static char line[64];
static unsigned line_len;

void console_init(uint32_t (*active_voices)(void)) {
    voices = active_voices;
    const int fl = fcntl(STDIN_FILENO, F_GETFL);
    fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK);
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

void console_poll(void) {
    char c;
    while (read(STDIN_FILENO, &c, 1) == 1) {
        if (c == '\r' || c == '\n') {
            if (line_len == 0) continue;
            putchar('\n');
            line[line_len] = 0;
            line_len = 0;
            run(line);
        } else if ((c == 8 || c == 127) && line_len) {
            --line_len;
            fputs("\b \b", stdout);
        } else if (c >= ' ' && c < 127 && line_len < sizeof(line) - 1) {   // Printable ASCII only (boot noise)
            line[line_len++] = c;
            putchar(c);
        }
        fflush(stdout);
    }
}
