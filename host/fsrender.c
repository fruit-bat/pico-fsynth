//
// fsrender - render a MIDI file to a WAV file with the pico-fsynth core.
//
// Usage: fsrender [-r rate] [-g gain] font.fsf song.mid out.wav
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <semaphore.h>

#include "fs_font.h"
#include "fs_synth.h"
#include "fs_smf.h"

static void *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    rewind(f);
    void *buf = malloc(sz > 0 ? (size_t)sz : 1);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fprintf(stderr, "cannot read %s\n", path); exit(1); }
    fclose(f);
    *size = (size_t)sz;
    return buf;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }

static void write_wav_header(FILE *f, uint32_t rate, uint32_t frames) {
    uint8_t h[44];
    const uint32_t data = frames * 4;
    memcpy(h, "RIFF", 4); put32(h + 4, 36 + data);
    memcpy(h + 8, "WAVEfmt ", 8); put32(h + 16, 16);
    put16(h + 20, 1); put16(h + 22, 2); put32(h + 24, rate);
    put32(h + 28, rate * 4); put16(h + 32, 4); put16(h + 34, 16);
    memcpy(h + 36, "data", 4); put32(h + 40, data);
    fwrite(h, 1, sizeof(h), f);
}

// Threaded rendering (-t): a worker thread renders part 1 of every block,
// like core 0 does on the Pico, to check the split has no shared state.
static sem_t work_go, work_done;
static volatile bool work_quit;

static void *worker(void *arg) {
    FsSynth *synth = (FsSynth *)arg;
    for (;;) {
        sem_wait(&work_go);
        if (work_quit) return NULL;
        for (uint32_t p = 1; p < FS_RENDER_PARTS; ++p) fs_synth_render_part(synth, p);
        sem_post(&work_done);
    }
}

static void on_midi(void *ctx, const uint8_t *msg, uint32_t len) {
    fs_synth_midi((FsSynth *)ctx, msg, len);
}

int main(int argc, char **argv) {
    uint32_t rate = 44100;
    float gain = 0.2f;
    bool dry = false;
    bool threaded = false;
    float rv[4] = { -1, -1, -1, -1 };   // Reverb room, damp, width, level (-1 = default)
    float ch[3] = { -1, -1, -1 };       // Chorus rate, depth, level
    float eq[4] = { 200, 0, 3000, 0 };  // Master EQ low Hz, low dB, high Hz, high dB
    int argi = 1;
    while (argi < argc && argv[argi][0] == '-') {
        if (!strcmp(argv[argi], "-r") && argi + 1 < argc) rate = (uint32_t)atoi(argv[++argi]);
        else if (!strcmp(argv[argi], "-g") && argi + 1 < argc) gain = (float)atof(argv[++argi]);
        else if (!strcmp(argv[argi], "-d")) dry = true;
        else if (!strcmp(argv[argi], "-t")) threaded = true;
        else if (!strcmp(argv[argi], "-R") && argi + 1 < argc) sscanf(argv[++argi], "%f,%f,%f,%f", &rv[0], &rv[1], &rv[2], &rv[3]);
        else if (!strcmp(argv[argi], "-C") && argi + 1 < argc) sscanf(argv[++argi], "%f,%f,%f", &ch[0], &ch[1], &ch[2]);
        else if (!strcmp(argv[argi], "-E") && argi + 1 < argc) sscanf(argv[++argi], "%f,%f,%f,%f", &eq[1], &eq[3], &eq[0], &eq[2]);
        else break;
        ++argi;
    }
    if (argc - argi != 3 || rate < 8000 || rate > FS_MAX_SAMPLE_RATE) {
        fprintf(stderr, "usage: fsrender [-r rate (8000..%d)] [-g gain] [-d (dry, no effects)] [-t (render on two threads)]\n"
                "                [-R room,damp,width,level] [-C rate_hz,depth_ms,level]\n"
                "                [-E bass_db,treble_db[,bass_hz,treble_hz]] font.fsf song.mid out.wav\n", FS_MAX_SAMPLE_RATE);
        return 2;
    }

    size_t font_size, midi_size;
    void *font_data = read_file(argv[argi], &font_size);
    void *midi_data = read_file(argv[argi + 1], &midi_size);

    FsFont font;
    const FsFontResult res = fs_font_init(&font, font_data, font_size, true, false);
    if (res != FS_FONT_OK) { fprintf(stderr, "%s: %s\n", argv[argi], fs_font_result_str(res)); return 1; }

    static FsSmf smf;
    if (!fs_smf_init(&smf, midi_data, midi_size, rate)) { fprintf(stderr, "%s: not a supported MIDI file\n", argv[argi + 1]); return 1; }

    static FsSynth synth; // Large: keep it off the stack
    fs_synth_init(&synth, &font, rate);
    fs_synth_set_gain(&synth, gain);
    fs_synth_set_effects(&synth, !dry, !dry);
    if (rv[0] >= 0) fs_synth_set_reverb(&synth, rv[0], rv[1], rv[2], rv[3]);
    if (ch[0] >= 0) fs_synth_set_chorus(&synth, ch[0], ch[1], ch[2]);
    fs_synth_set_eq(&synth, eq[0], eq[1], eq[2], eq[3]);

    FILE *out = fopen(argv[argi + 2], "wb");
    if (!out) { fprintf(stderr, "cannot create %s\n", argv[argi + 2]); return 1; }
    write_wav_header(out, rate, 0);

    int16_t block[FS_BLOCK_SIZE * 2];
    uint32_t frames = 0;
    uint32_t tail_blocks = 0;
    const uint32_t max_tail = 5 * rate / FS_BLOCK_SIZE;
    const clock_t t0 = clock();
    pthread_t thread;
    if (threaded) {
        sem_init(&work_go, 0, 0);
        sem_init(&work_done, 0, 0);
        pthread_create(&thread, NULL, worker, &synth);
    }

    for (;;) {
        const bool playing = fs_smf_advance(&smf, FS_BLOCK_SIZE, on_midi, &synth);
        if (threaded) {
            fs_synth_render_begin(&synth);
            sem_post(&work_go);
            fs_synth_render_part(&synth, 0);
            sem_wait(&work_done);
            fs_synth_render_end(&synth, block);
        } else {
            fs_synth_render(&synth, block);
        }
        fwrite(block, sizeof(int16_t), FS_BLOCK_SIZE * 2, out);
        frames += FS_BLOCK_SIZE;
        if (!playing) {
            // Let notes and reverb ring out, at most 5 s
            if (fs_synth_active_voices(&synth) == 0 && tail_blocks > rate / FS_BLOCK_SIZE) break;
            if (++tail_blocks > max_tail) break;
        }
    }

    if (threaded) {
        work_quit = true;
        sem_post(&work_go);
        pthread_join(thread, NULL);
    }
    const double cpu = (double)(clock() - t0) / CLOCKS_PER_SEC;
    fseek(out, 0, SEEK_SET);
    write_wav_header(out, rate, frames);
    fclose(out);

    const FsSynthStats *st = &synth.stats;
    const double secs = (double)frames / rate;
    printf("%s: %.1f s audio, %.2f s CPU (%.0fx realtime)\n", argv[argi + 2], secs, cpu, cpu > 0 ? secs / cpu : 0.0);
    printf("  notes %u, voices started %u, peak %u, stolen %u, dropped %u, clipped samples %u\n",
           st->notes, st->voices_started, st->peak_voices, st->voices_stolen, st->notes_dropped, st->clipped);
    if (st->voices_stolen) {
        printf("  stolen voices faded %u, cut %u; held notes stolen per channel:", st->voices_stolen - st->voices_cut, st->voices_cut);
        for (int c = 0; c < 16; ++c) if (st->stolen_by_channel[c]) printf(" ch%d=%u", c, st->stolen_by_channel[c]);
        printf("\n");
    }
    free(font_data);
    free(midi_data);
    return 0;
}
