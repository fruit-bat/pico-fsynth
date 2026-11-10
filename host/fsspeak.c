//
// fsspeak.c - speak (or sing) into a WAV file with the pico-fsynth speech
// synthesizer (core/fs_speech.c).
//
// Usage: fsspeak [options] "INPUT" out.wav
//   -t         INPUT is English text (needs the dictionary, see -d);
//              otherwise it is a phoneme string (see core/fs_speech.h),
//              which can also be sung
//   -d path    pronouncing dictionary for -t (default: cmudict.dict next
//              to this program; host/get_cmudict.sh downloads it)
//   -v         print the phonemes
//   -p hz      base pitch (default 110)
//   -s speed   speaking rate, 1 = normal
//   -f scale   formant scale: 1 = adult male, about 1.15 = female
//   -a accent  pitch accent on stressed vowels (default 0.3 = +30%)
//   -b breath  breathiness 0..1 (default 0.5)
//   -T bright  voice brightness 0.1..1 (default 0.5)
//   -V semis   singing vibrato depth (default 0.3 semitones)
//   -r rate    sample rate (default 44100)
//   -g gain    output gain (default 1)
//
// Examples:
//   fsspeak -t "Hello, world." hello.wav
//   fsspeak "HH AH0 L OW1 , W ER1 L D ." hello.wav
//   fsspeak "T W IH1@60:350 NG K AH0@60:350 L S T AA1@67:900 R" sing.wav
//
// Output is 16-bit mono PCM. The length and peak level are printed;
// samples beyond full scale are clipped and counted.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "fs_speech.h"
#include "fs_text.h"

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }

// 44-byte WAV header for 16-bit mono PCM
static void write_wav_header(FILE *f, uint32_t rate, uint32_t frames) {
    uint8_t h[44];
    const uint32_t data = frames * 2;
    memcpy(h, "RIFF", 4); put32(h + 4, 36 + data);
    memcpy(h + 8, "WAVEfmt ", 8); put32(h + 16, 16);
    put16(h + 20, 1); put16(h + 22, 1); put32(h + 24, rate);
    put32(h + 28, rate * 2); put16(h + 32, 2); put16(h + 34, 16);
    memcpy(h + 36, "data", 4); put32(h + 40, data);
    fwrite(h, 1, sizeof(h), f);
}

static void usage(void) {
    fprintf(stderr,
            "usage: fsspeak [-t] [-d dict] [-v] [-p hz] [-s speed] [-f formant_scale] [-a accent]\n"
            "               [-b breath] [-T bright] [-V vibrato] [-r rate] [-g gain] \"INPUT\" out.wav\n"
            "  -t: INPUT is English text, else phonemes (see core/fs_speech.h)\n");
}

static FsSpeech speech;     // Large: keep it off the stack
static char phonemes[16384];

int main(int argc, char **argv) {
    float rate = 44100.0f;
    fs_speech_init(&speech, rate);
    FsSpeechVoice v = speech.voice;
    bool text = false, verbose = false;
    const char *dict_path = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "td:vp:s:f:a:b:T:V:r:g:")) != -1) {
        switch (opt) {
        case 't': text = true; break;
        case 'd': dict_path = optarg; break;
        case 'v': verbose = true; break;
        case 'p': v.pitch_hz = strtof(optarg, NULL); break;
        case 's': v.speed = strtof(optarg, NULL); break;
        case 'f': v.formant_scale = strtof(optarg, NULL); break;
        case 'a': v.accent = strtof(optarg, NULL); break;
        case 'b': v.breathiness = strtof(optarg, NULL); break;
        case 'T': v.brightness = strtof(optarg, NULL); break;
        case 'V': v.vibrato = strtof(optarg, NULL); break;
        case 'r': rate = strtof(optarg, NULL); break;
        case 'g': v.gain = strtof(optarg, NULL); break;
        default: usage(); return 1;
        }
    }
    if (argc - optind != 2 || v.speed <= 0 || v.pitch_hz <= 0 || rate < 8000) {
        usage();
        return 1;
    }
    const char *input = argv[optind];
    if (text) {
        // Default dictionary: next to this program
        char def[4096];
        if (!dict_path) {
            const char *slash = strrchr(argv[0], '/');
            const int dir = slash ? (int)(slash - argv[0]) + 1 : 0;
            snprintf(def, sizeof(def), "%.*scmudict.dict", dir, argv[0]);
            dict_path = def;
        }
        FsTextDict *dict = fs_text_load(dict_path);
        if (!dict) {
            fprintf(stderr, "fsspeak: cannot read the dictionary %s (run host/get_cmudict.sh)\n", dict_path);
            return 1;
        }
        const int spelled = fs_text_to_phonemes(dict, input, phonemes, sizeof(phonemes));
        fs_text_free(dict);
        if (spelled < 0) {
            fprintf(stderr, "fsspeak: text too long\n");
            return 1;
        }
        if (spelled > 0) fprintf(stderr, "fsspeak: %d word(s) not in the dictionary were spelled out\n", spelled);
        input = phonemes;
    }
    if (verbose) printf("%s\n", input);

    fs_speech_init(&speech, rate);
    speech.voice = v;
    const int bad = fs_speech_say(&speech, input);
    if (bad) {
        fprintf(stderr, "fsspeak: bad phoneme (token %d)%s\n", bad,
                text ? "" : ", or too many (see FS_SPEECH_MAX_PHONEMES)");
        return 1;
    }
    FILE *out = fopen(argv[optind + 1], "wb");
    if (!out) {
        perror(argv[optind + 1]);
        return 1;
    }
    write_wav_header(out, (uint32_t)rate, 0);
    float buf[256];
    int16_t pcm[256];
    uint32_t frames = 0, clipped = 0;
    float peak = 0;
    int n;
    while ((n = fs_speech_render(&speech, buf, 256)) > 0) {
        for (int i = 0; i < n; ++i) {
            float s = buf[i];
            if (s > peak) peak = s;
            if (-s > peak) peak = -s;
            if (s > 1.0f) { s = 1.0f; ++clipped; }
            if (s < -1.0f) { s = -1.0f; ++clipped; }
            pcm[i] = (int16_t)(s * 32767.0f);
        }
        fwrite(pcm, sizeof(int16_t), (size_t)n, out);
        frames += (uint32_t)n;
    }
    rewind(out);
    write_wav_header(out, (uint32_t)rate, frames);
    fclose(out);
    printf("%.2f s, peak %.3f, %u clipped\n", frames / rate, peak, clipped);
    return 0;
}
