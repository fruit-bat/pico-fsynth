//
// serial_midi.c - bridge an ALSA sequencer MIDI port to a serial port, for
// the ESP32-P4 firmware, which takes raw MIDI on its serial console (the
// Type-C port). Linux only (ALSA).
//
//   serial_midi [-b BAUD] [DEVICE]      default /dev/ttyACM0 at 115200
//
// Creates a sequencer port named "fsynth-serial". Anything sent to it is
// written to the serial port as raw MIDI bytes, every message with its
// status byte: the firmware needs that to tell MIDI from typed text (see
// esp32p4/main/console.c). Whatever the board prints (its status lines) is
// copied to stdout. Typed lines on stdin are sent too, so console commands
// (help, stress 80, off...) work from the same terminal.
//
//   aplaymidi -p fsynth-serial song.mid
//   aconnect 'My Keyboard' fsynth-serial
//
// Opening the port does not reset the board: the auto-reset circuit only
// acts when DTR and RTS differ, and Linux raises both together.
//
#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static speed_t baud_constant(long baud) {
    switch (baud) {
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 921600: return B921600;
    default: return 0;
    }
}

static int open_serial(const char *dev, speed_t speed) {
    const int fd = open(dev, O_RDWR | O_NOCTTY);
    if (fd < 0) return -1;
    struct termios t;
    if (tcgetattr(fd, &t) < 0) { close(fd); return -1; }
    cfmakeraw(&t);
    cfsetispeed(&t, speed);
    cfsetospeed(&t, speed);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &t) < 0) { close(fd); return -1; }
    return fd;
}

// Write all of buf, retrying on short writes.
static int write_all(int fd, const unsigned char *buf, size_t len) {
    while (len) {
        const ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

int main(int argc, char **argv) {
    long baud = 115200;
    const char *dev = "/dev/ttyACM0";
    int opt;
    while ((opt = getopt(argc, argv, "b:h")) != -1) {
        if (opt == 'b') baud = strtol(optarg, NULL, 10);
        else {
            fprintf(stderr, "usage: %s [-b BAUD] [DEVICE]   (default /dev/ttyACM0, 115200)\n", argv[0]);
            return opt == 'h' ? 0 : 2;
        }
    }
    if (optind < argc) dev = argv[optind];
    const speed_t speed = baud_constant(baud);
    if (!speed) { fprintf(stderr, "unsupported baud rate %ld\n", baud); return 2; }

    const int tty = open_serial(dev, speed);
    if (tty < 0) { fprintf(stderr, "%s: %s\n", dev, strerror(errno)); return 1; }

    snd_seq_t *seq;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, 0) < 0) {
        fprintf(stderr, "cannot open the ALSA sequencer\n");
        return 1;
    }
    snd_seq_set_client_name(seq, "fsynth-serial");
    const int port = snd_seq_create_simple_port(seq, "fsynth-serial",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (port < 0) { fprintf(stderr, "cannot create the sequencer port\n"); return 1; }
    snd_midi_event_t *dec;
    if (snd_midi_event_new(4096, &dec) < 0) return 1;
    snd_midi_event_no_status(dec, 1);   // Never use running status

    fprintf(stderr, "fsynth-serial: sequencer port %d:%d -> %s at %ld baud. Ctrl+C quits.\n",
            snd_seq_client_id(seq), port, dev, baud);

    const int nseq = snd_seq_poll_descriptors_count(seq, POLLIN);
    struct pollfd fds[2 + 8];
    if (nseq > 8) return 1;
    fds[0].fd = tty;          fds[0].events = POLLIN;
    fds[1].fd = STDIN_FILENO; fds[1].events = POLLIN;
    snd_seq_poll_descriptors(seq, &fds[2], (unsigned)nseq, POLLIN);
    bool stdin_open = true;

    for (;;) {
        if (poll(fds, (nfds_t)(2 + nseq), -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[0].revents & (POLLERR | POLLHUP)) { fprintf(stderr, "%s closed\n", dev); break; }
        if (fds[0].revents & POLLIN) {
            unsigned char buf[512];
            const ssize_t n = read(tty, buf, sizeof(buf));
            if (n > 0) { fwrite(buf, 1, (size_t)n, stdout); fflush(stdout); }
        }
        if (stdin_open && (fds[1].revents & (POLLIN | POLLHUP))) {
            char buf[256];
            const ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) { stdin_open = false; fds[1].fd = -1; }
            else {
                // Text only: drop anything that would look like MIDI
                for (ssize_t i = 0; i < n; ++i) if ((unsigned char)buf[i] >= 0x80) buf[i] = '?';
                for (ssize_t i = 0; i < n; ++i) if (buf[i] == '\n') buf[i] = '\r';
                if (write_all(tty, (unsigned char *)buf, (size_t)n) < 0) break;
            }
        }
        snd_seq_event_t *ev;
        while (snd_seq_event_input_pending(seq, 1) > 0 && snd_seq_event_input(seq, &ev) >= 0) {
            unsigned char buf[4096];
            const long n = snd_midi_event_decode(dec, buf, sizeof(buf), ev);
            if (n > 0 && write_all(tty, buf, (size_t)n) < 0) {
                fprintf(stderr, "%s: write failed: %s\n", dev, strerror(errno));
                return 1;
            }
        }
    }
    return 0;
}
