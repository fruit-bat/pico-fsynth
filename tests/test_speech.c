//
// test_speech.c - tests of the formant speech synthesizer (core/fs_speech.c):
// phoneme parsing, timing, pitch (spoken and sung) and the spectra of
// vowels and fricatives.
//
// Spectra are measured with a plain DFT over a stretch of a sustained
// sound, so the checks are about where the energy is (which is what makes
// an "ee" an "ee"), not exact values.
//
#include <string.h>
#include "test.h"
#include "fs_speech.h"

#define RATE 44100
#define MAX_SAMPLES (RATE * 8)

static FsSpeech sp;
static float wav[MAX_SAMPLES];

// Say a phoneme string with the default voice (and no vibrato, so pitch
// can be measured exactly); returns the number of samples
static int say(const char *ph) {
    fs_speech_init(&sp, RATE);
    sp.voice.vibrato = 0;
    if (fs_speech_say(&sp, ph) != 0) return -1;
    int n = 0, k;
    while (n < MAX_SAMPLES && (k = fs_speech_render(&sp, wav + n, MAX_SAMPLES - n > 512 ? 512 : MAX_SAMPLES - n)) > 0) n += k;
    return n;
}

// Energy between lo and hi Hz of wav[start, start + len), by DFT
static double band_energy(int start, int len, double lo, double hi) {
    double e = 0;
    for (int bin = (int)(lo * len / RATE); bin <= (int)(hi * len / RATE); ++bin) {
        double re = 0, im = 0;
        for (int i = 0; i < len; ++i) {
            const double w = 0.5 - 0.5 * cos(2 * M_PI * i / len);     // Hann window
            const double a = 2 * M_PI * bin * i / len;
            re += wav[start + i] * w * cos(a);
            im -= wav[start + i] * w * sin(a);
        }
        e += re * re + im * im;
    }
    return e;
}

// Pitch of wav[start, start + len) by autocorrelation, Hz (60..450 Hz;
// keep len short where the pitch moves)
static double pitch(int start, int len) {
    double best = -1;
    int best_lag = 0;
    for (int lag = RATE / 450; lag < RATE / 60; ++lag) {
        double c = 0;
        for (int i = 0; i + lag < len; ++i) c += wav[start + i] * wav[start + i + lag];
        if (c > best) {
            best = c;
            best_lag = lag;
        }
    }
    // Refine between samples with a parabola through the peak
    double c[3];
    for (int k = -1; k <= 1; ++k) {
        c[k + 1] = 0;
        for (int i = 0; i + best_lag + k < len; ++i) c[k + 1] += wav[start + i] * wav[start + i + best_lag + k];
    }
    const double d = 0.5 * (c[0] - c[2]) / (c[0] - 2 * c[1] + c[2]);
    return RATE / (best_lag + d);
}

static double rms(int start, int len) {
    double e = 0;
    for (int i = 0; i < len; ++i) e += wav[start + i] * wav[start + i];
    return sqrt(e / len);
}

TEST(silent_until_said) {
    fs_speech_init(&sp, RATE);
    CHECK(!fs_speech_busy(&sp));
    CHECK_EQ(fs_speech_render(&sp, wav, 100), 0);
}

TEST(parse_errors) {
    fs_speech_init(&sp, RATE);
    CHECK_EQ(fs_speech_say(&sp, "HH AH0 L OW1 , W ER1 L D ."), 0);
    CHECK_EQ(fs_speech_say(&sp, "hh ah0 l ow1"), 0);       // Case does not matter
    CHECK_EQ(fs_speech_say(&sp, "HH AH0 XX OW1"), 3);      // Position of the bad token
    CHECK_EQ(fs_speech_say(&sp, "AA3"), 1);                // Stress 0..2 only
    CHECK_EQ(fs_speech_say(&sp, "AA1@128"), 1);            // MIDI notes only
    CHECK_EQ(fs_speech_say(&sp, "L@60:300"), 1);           // Notes go on vowels
    CHECK_EQ(fs_speech_say(&sp, "AA1@60:0"), 1);
    CHECK_EQ(fs_speech_say(&sp, "AA1@60:300 _:200"), 0);   // A rest may have a length
    CHECK_EQ(fs_speech_say(&sp, "AA1@"), 1);
    CHECK_EQ(fs_speech_say(&sp, "OW1."), 0);               // Punctuation stuck to a word

    // Too many phonemes is an error, not an overflow
    static char longs[FS_SPEECH_MAX_PHONEMES * 4 + 16];
    longs[0] = 0;
    for (int i = 0; i < FS_SPEECH_MAX_PHONEMES + 1; ++i) strcat(longs, "AA ");
    CHECK_EQ(fs_speech_say(&sp, longs), FS_SPEECH_MAX_PHONEMES + 1);
    longs[strlen(longs) - 3] = 0;
    CHECK_EQ(fs_speech_say(&sp, longs), 0);
}

TEST(every_phoneme_renders) {
    const char *all = "AA AE AH AO AW AX AY EH ER EY IH IY OW OY UH UW B CH D DH F G HH JH K L M N NG "
                      "P R S SH T TH V W Y Z ZH _ , ! ? .";
    const int n = say(all);
    CHECK(n > RATE);
    bool finite = true;
    float peak = 0;
    for (int i = 0; i < n; ++i) {
        if (!isfinite(wav[i])) finite = false;
        if (fabsf(wav[i]) > peak) peak = fabsf(wav[i]);
    }
    CHECK(finite);
    CHECK(peak > 0.1f && peak < 1.0f);
}

TEST(deterministic) {
    const int n = say("DH AH0 K W IH1 K B R AW1 N F AA1 K S .");
    static float first[MAX_SAMPLES];
    memcpy(first, wav, sizeof(float) * (size_t)n);
    CHECK_EQ(say("DH AH0 K W IH1 K B R AW1 N F AA1 K S ."), n);
    CHECK(memcmp(first, wav, sizeof(float) * (size_t)n) == 0);
}

TEST(length_and_speed) {
    fs_speech_init(&sp, RATE);
    fs_speech_say(&sp, "S EH1 V AH0 N T IY0 S EH1 V AH0 N");
    const uint32_t normal = fs_speech_length(&sp);
    fs_speech_init(&sp, RATE);
    sp.voice.speed = 2.0f;
    fs_speech_say(&sp, "S EH1 V AH0 N T IY0 S EH1 V AH0 N");
    CHECK_NEAR((double)fs_speech_length(&sp) / normal, 0.5, 0.02);

    // Sung lengths add up: lead in (80 ms) + 500 + 300 rest + final pause (450)
    fs_speech_init(&sp, RATE);
    fs_speech_say(&sp, "AA1@60:500 _:300");
    CHECK_NEAR(fs_speech_length(&sp), (80 + 500 + 300 + 450) * RATE / 1000.0, RATE * 0.002);
    CHECK(fs_speech_busy(&sp));
}

TEST(sung_pitch) {
    // 0.5 s into a 1.5 s note, after the glide
    say("AA1@69:1500");
    CHECK_NEAR(pitch(RATE * 6 / 10, 4096), 440.0, 2.0);
    say("IY1@57:1500");
    CHECK_NEAR(pitch(RATE * 6 / 10, 4096), 220.0, 1.0);
    say("UW1@45:1500");
    CHECK_NEAR(pitch(RATE * 6 / 10, 4096), 110.0, 0.5);

    // A vowel without a note keeps the previous one
    say("AA1@64:600 IY1:600");
    CHECK_NEAR(pitch(RATE * 95 / 100, 4096), 329.6, 2.0);
}

TEST(spoken_pitch) {
    // Speech follows the base pitch, within the intonation's range
    fs_speech_init(&sp, RATE);
    say("AA1");
    const double f = pitch(RATE * 15 / 100, 1024);
    CHECK(f > 110 * 0.75 && f < 110 * 1.45);

    fs_speech_init(&sp, RATE);
    sp.voice.pitch_hz = 200;
    sp.voice.vibrato = 0;
    fs_speech_say(&sp, "AA1");
    int n = 0, k;
    while ((k = fs_speech_render(&sp, wav + n, 512)) > 0) n += k;
    const double g = pitch(RATE * 15 / 100, 1024);
    CHECK(g > 200 * 0.75 && g < 200 * 1.45);
}

TEST(question_rises) {
    // The end of a question is higher than the end of a statement
    say("Y UW1 S IY1 .");
    // The last vowel "IY": its last 60 ms before the final pause (450 ms)
    const int end_s = (int)(fs_speech_length(&sp) - 0.45 * RATE);
    const double fs = pitch(end_s - RATE * 8 / 100, 2048);
    say("Y UW1 S IY1 ?");
    const int end_q = (int)(fs_speech_length(&sp) - 0.45 * RATE);
    const double fq = pitch(end_q - RATE * 8 / 100, 2048);
    CHECK(fq > fs * 1.3);
}

TEST(vowel_formants) {
    // "ee" has its second formant high (about 2.3 kHz), "ah" has its first
    // formant high (about 0.8 kHz) and the second low; "oo" has both low
    const int at = RATE * 3 / 10, len = 4096;
    say("IY1@45:1000");
    const double iy_low = band_energy(at, len, 600, 1200), iy_high = band_energy(at, len, 1900, 2800);
    say("AA1@45:1000");
    const double aa_low = band_energy(at, len, 600, 1200), aa_high = band_energy(at, len, 1900, 2800);
    say("UW1@45:1000");
    const double uw_mid = band_energy(at, len, 600, 1600), uw_f1 = band_energy(at, len, 200, 500);
    CHECK(iy_high > iy_low);
    CHECK(aa_low > 10 * aa_high);
    CHECK(iy_high / iy_low > 30 * (aa_high / aa_low));
    CHECK(uw_f1 > uw_mid);
}

TEST(fricative_spectra) {
    // "s" hisses above 4 kHz, "sh" lower down (2-4 kHz); "f" is weak
    const int at = RATE * 12 / 100, len = 2048;
    say("S");
    const double s_hi = band_energy(at, len, 4500, 9000), s_mid = band_energy(at, len, 1500, 3500);
    const double s_rms = rms(at, len);
    say("SH");
    const double sh_hi = band_energy(at, len, 4500, 9000), sh_mid = band_energy(at, len, 1500, 3500);
    const double sh_rms = rms(at, len);
    say("F");
    const double f_rms = rms(at, len);
    CHECK(s_hi > 3 * s_mid);
    CHECK(sh_mid > 2 * sh_hi);
    CHECK(f_rms < 0.5 * s_rms);
    CHECK(sh_rms > s_rms);
}

TEST(voicing) {
    // A vowel is voiced: its pitch period shows up strongly in the
    // autocorrelation (sung, so the pitch holds still); "s" is noise and
    // does not
    int at = RATE * 3 / 10;
    const int len = 1024;
    say("AA1@45:800");
    double c0 = 0, c1 = 0;
    const int lag = (int)(RATE / pitch(at, 1024) + 0.5);
    for (int i = 0; i < len; ++i) {
        c0 += wav[at + i] * wav[at + i];
        c1 += wav[at + i] * wav[at + i + lag];
    }
    CHECK(c1 > 0.8 * c0);
    say("S");
    at = RATE * 12 / 100;
    double d0 = 0, dmax = 0;
    for (int i = 0; i < len; ++i) d0 += wav[at + i] * wav[at + i];
    for (int l = RATE / 400; l < RATE / 70; ++l) {
        double d = 0;
        for (int i = 0; i < len; ++i) d += wav[at + i] * wav[at + i + l];
        if (d > dmax) dmax = d;
    }
    CHECK(dmax < 0.3 * d0);
}

int main(void) {
    RUN(silent_until_said);
    RUN(parse_errors);
    RUN(every_phoneme_renders);
    RUN(deterministic);
    RUN(length_and_speed);
    RUN(sung_pitch);
    RUN(spoken_pitch);
    RUN(question_rises);
    RUN(vowel_formants);
    RUN(fricative_spectra);
    RUN(voicing);
    return test_summary();
}
