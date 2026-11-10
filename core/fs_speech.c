//
// fs_speech.c - formant speech synthesizer (see fs_speech.h for the idea
// and the phoneme notation).
//
// The work is split in two:
//
// fs_speech_say lays the whole utterance out as three piecewise linear
// tracks of keyframes:
//   tract   formant frequencies and bandwidths (the shape of the mouth),
//   source  levels of voicing, breath and hiss, and the hiss spectrum,
//   pitch   the intonation contour.
// The tracks are separate because they move differently: formants glide
// smoothly from one sound into the next (tens of ms), source levels switch
// within a few ms (a burst of a "t" is 1 ms of rise), and pitch follows the
// phrase. A stop such as "p" is one formant segment (the closure) but three
// source segments (closure, burst, aspiration); the formant glide into the
// next vowel already runs during the burst and aspiration, as in real
// speech.
//
// fs_speech_render then runs the synthesizer, reading the tracks every
// FS_SPEECH_BLOCK samples:
//
//   glottal pulses * AV --+                     cascade (vocal tract)
//   breath noise * AH ----+--> nasal pole --> nasal zero --> F5 F4 F3 F2 F1 --+
//                                                                             +--> out
//   hiss noise * AF ------+--> resonator A * GA ---+                          |
//                         +--> resonator B * GB ---+--------------------------+
//                         +--> flat * BYPASS ------+
//
// The cascade is the classic Klatt arrangement for voiced sounds: putting
// the resonators in series gives the right relative formant levels for
// free. Hiss is made in front of the mouth, so it gets its own parallel
// filters with explicit levels.
//
// Formant transitions follow Holmes, Mattingly and Shearme (1964): at each
// boundary between two sounds, the one with the higher rank (consonants
// over vowels) decides the value at the boundary as
//   own target + prop * (neighbour target - own target)
// and how long the glides on each side take. A small prop gives a fixed
// "locus" (labial "b" pulls F2 down whatever the vowel); a larger one lets
// the vowel colour the consonant.
//
// Phoneme data: vowel formants are the averages for American men measured
// by Hillenbrand et al., "Acoustic characteristics of American English
// vowels", JASA 97 (1995); consonants and bandwidths mostly follow Klatt,
// "Software for a cascade/parallel formant synthesizer", JASA 67 (1980);
// durations follow Klatt's rules (Allen, Hunnicutt and Klatt, "From text
// to speech: the MITalk system", 1987). The levels were then set by
// measurement and the rest tuned with a speech recogniser as the listener.
//
#include "fs_speech.h"

#include <math.h>
#include <string.h>
#include <ctype.h>

// --- Phoneme table -------------------------------------------------------

enum {
    PH_VOWEL   = 1 << 0,
    PH_VOICED  = 1 << 1,
    PH_STOP    = 1 << 2,    // Closure, burst, aspiration
    PH_FRIC    = 1 << 3,
    PH_NASAL   = 1 << 4,
    PH_APPROX  = 1 << 5,    // w y r l
    PH_ASPIR   = 1 << 6,    // h: formants taken from the next sound
    PH_PAUSE   = 1 << 7,
    PH_AFFRIC  = 1 << 8,    // ch jh: closure then frication
};

#define OFF (-99)           // dB value meaning "off"

typedef struct {
    char name[3];
    uint16_t flags;
    uint8_t rank;           // Who decides the boundary (higher wins)
    uint16_t dur, min_dur;  // Inherent and minimum duration, ms
    uint16_t f[3], b[3];    // Formant targets (start of a diphthong), Hz
    uint16_t f_end[3];      // Diphthong end targets; 0 = not a diphthong
    uint16_t fnz;           // Nasal zero, Hz; 0 = not nasal
    int8_t av, ah, af;      // Voicing, aspiration, frication, dB re a vowel
    uint16_t fa, ba;        // Hiss resonator A (burst for stops)
    int8_t ga;
    uint16_t fb, bb;        // Hiss resonator B
    int8_t gb;
    int8_t bypass;          // Flat hiss, dB
    uint8_t prop[3];        // Boundary proportion per formant, %
    uint8_t t_ext, t_int;   // Glide time on the neighbour's / own side, ms
    uint8_t burst, vot;     // Stops: burst and aspiration length, ms
} PhonDef;

// Shorthands for the table
#define F3V(a, b, c) { a, b, c }
#define HISS_NONE 0, 0, OFF, 0, 0, OFF, OFF

static const PhonDef phonemes[] = {
    // name flags                     rk dur min  formants             bandwidths        diphthong end        fnz  av  ah   af   hiss A / B / bypass                prop          ext int burst vot
    // Vowels
    { "AA", PH_VOWEL | PH_VOICED,      2, 240, 100, F3V(770, 1330, 2520), F3V(130, 70, 160), F3V(0, 0, 0),         0,  0, OFF, OFF, HISS_NONE, F3V(50, 50, 50), 60, 60, 0, 0 },
    { "AE", PH_VOWEL | PH_VOICED,      2, 230,  80, F3V(760, 1680, 2500), F3V(70, 130, 300), F3V(0, 0, 0),         0,  0, OFF, OFF, HISS_NONE, F3V(50, 50, 50), 60, 60, 0, 0 },
    { "AH", PH_VOWEL | PH_VOICED,      2, 140,  60, F3V(620, 1200, 2550), F3V(80, 50, 140),  F3V(0, 0, 0),         0,  0, OFF, OFF, HISS_NONE, F3V(50, 50, 50), 60, 60, 0, 0 },
    { "AO", PH_VOWEL | PH_VOICED,      2, 240, 100, F3V(650, 1000, 2540),  F3V(90, 100, 80),  F3V(0, 0, 0),         0,  0, OFF, OFF, HISS_NONE, F3V(50, 50, 50), 60, 60, 0, 0 },
    { "AW", PH_VOWEL | PH_VOICED,      2, 260, 100, F3V(720, 1350, 2500), F3V(80, 70, 140),  F3V(420, 940, 2350),  0,  0, OFF, OFF, HISS_NONE, F3V(50, 50, 50), 60, 60, 0, 0 },
    { "AX", PH_VOWEL | PH_VOICED,      2, 120,  55, F3V(550, 1500, 2500), F3V(80, 50, 140),  F3V(0, 0, 0),         0, -1, OFF, OFF, HISS_NONE, F3V(50, 50, 50), 60, 60, 0, 0 },
    { "AY", PH_VOWEL | PH_VOICED,      2, 250, 150, F3V(700, 1250, 2550), F3V(100, 70, 200), F3V(400, 2050, 2600), 0,  0, OFF, OFF, HISS_NONE, F3V(50, 50, 50), 60, 60, 0, 0 },
    { "EH", PH_VOWEL | PH_VOICED,      2, 150,  70, F3V(580, 1800, 2600), F3V(60, 90, 200),  F3V(0, 0, 0),         0,  0, OFF, OFF, HISS_NONE, F3V(50, 50, 50), 60, 60, 0, 0 },
    { "ER", PH_VOWEL | PH_VOICED,      2, 180,  80, F3V(470, 1380, 1710), F3V(100, 60, 110), F3V(0, 0, 0),         0,  0, OFF, OFF, HISS_NONE, F3V(50, 50, 50), 60, 60, 0, 0 },
    { "EY", PH_VOWEL | PH_VOICED,      2, 190,  70, F3V(560, 1900, 2600), F3V(70, 100, 200), F3V(340, 2350, 2900), 0,  0, OFF, OFF, HISS_NONE, F3V(50, 50, 50), 60, 60, 0, 0 },
    { "IH", PH_VOWEL | PH_VOICED,      2, 135,  40, F3V(430, 2030, 2680), F3V(50, 100, 140), F3V(0, 0, 0),         0,  0, OFF, OFF, HISS_NONE, F3V(50, 50, 50), 60, 60, 0, 0 },
    { "IY", PH_VOWEL | PH_VOICED,      2, 155,  55, F3V(340, 2320, 3000), F3V(45, 200, 400), F3V(0, 0, 0),         0,  0, OFF, OFF, HISS_NONE, F3V(50, 50, 50), 60, 60, 0, 0 },
    { "OW", PH_VOWEL | PH_VOICED,      2, 220,  80, F3V(520, 1000, 2460), F3V(80, 70, 70),   F3V(420, 880, 2400),  0,  0, OFF, OFF, HISS_NONE, F3V(50, 50, 50), 60, 60, 0, 0 },
    { "OY", PH_VOWEL | PH_VOICED,      2, 280, 150, F3V(550, 960, 2400),  F3V(80, 50, 130),  F3V(400, 1950, 2550), 0,  0, OFF, OFF, HISS_NONE, F3V(50, 50, 50), 60, 60, 0, 0 },
    { "UH", PH_VOWEL | PH_VOICED,      2, 160,  60, F3V(470, 1120, 2430), F3V(80, 100, 80),  F3V(0, 0, 0),         0,  0, OFF, OFF, HISS_NONE, F3V(50, 50, 50), 60, 60, 0, 0 },
    { "UW", PH_VOWEL | PH_VOICED,      2, 210,  70, F3V(400, 1150, 2340), F3V(65, 110, 140), F3V(360, 1000, 2300),  0,  0, OFF, OFF, HISS_NONE, F3V(50, 50, 50), 60, 60, 0, 0 },

    // Glides and liquids: long, smooth transitions
    { "W",  PH_APPROX | PH_VOICED,     5,  80,  60, F3V(290, 610, 2150),  F3V(50, 80, 60),   F3V(0, 0, 0),         0, -3, OFF, OFF, HISS_NONE, F3V(30, 25, 30), 70, 40, 0, 0 },
    { "Y",  PH_APPROX | PH_VOICED,     5,  80,  40, F3V(260, 2070, 3020), F3V(40, 250, 500), F3V(0, 0, 0),         0, -3, OFF, OFF, HISS_NONE, F3V(30, 25, 30), 70, 40, 0, 0 },
    { "R",  PH_APPROX | PH_VOICED,     5,  80,  30, F3V(310, 1060, 1380), F3V(70, 100, 120), F3V(0, 0, 0),         0, -2, OFF, OFF, HISS_NONE, F3V(30, 30, 20), 70, 40, 0, 0 },
    { "L",  PH_APPROX | PH_VOICED,     5,  80,  40, F3V(310, 1050, 2880), F3V(50, 100, 280), F3V(0, 0, 0),         0, -3, OFF, OFF, HISS_NONE, F3V(30, 40, 40), 50, 30, 0, 0 },

    // Nasals: murmur of the nasal pole/zero pair, quick release
    { "M",  PH_NASAL | PH_VOICED,      6,  70,  60, F3V(480, 800, 2130), F3V(40, 200, 200), F3V(0, 0, 0),       450, -4, OFF, OFF, HISS_NONE, F3V(30, 50, 20), 40, 10, 0, 0 },
    { "N",  PH_NASAL | PH_VOICED,      6,  60,  50, F3V(480, 1600, 2470), F3V(40, 300, 300), F3V(0, 0, 0),       450, -4, OFF, OFF, HISS_NONE, F3V(30, 45, 40), 40, 10, 0, 0 },
    { "NG", PH_NASAL | PH_VOICED,      6,  95,  60, F3V(480, 2000, 2900), F3V(40, 300, 300), F3V(0, 0, 0),       450, -4, OFF, OFF, HISS_NONE, F3V(30, 60, 50), 40, 10, 0, 0 },

    // Fricatives
    { "F",  PH_FRIC,                   7, 100,  80, F3V(340, 800, 2080), F3V(200, 120, 150), F3V(0, 0, 0),        0, OFF, OFF, -18, 0, 0, OFF, 7500, 3000, -6, 0,     F3V(40, 65, 30), 40, 20, 0, 0 },
    { "V",  PH_FRIC | PH_VOICED,       7,  60,  40, F3V(220, 800, 2080), F3V(60, 90, 120),  F3V(0, 0, 0),         0, -12, OFF, -26, 0, 0, OFF, 7500, 3000, -6, 0,     F3V(40, 65, 30), 40, 20, 0, 0 },
    { "TH", PH_FRIC,                   7,  90,  60, F3V(320, 1290, 2540), F3V(200, 90, 200), F3V(0, 0, 0),         0, OFF, OFF, -16, 6500, 2500, -2, 0, 0, OFF, -6,   F3V(40, 50, 50), 40, 20, 0, 0 },
    { "DH", PH_FRIC | PH_VOICED,       7,  50,  30, F3V(270, 1290, 2540), F3V(60, 80, 170),  F3V(0, 0, 0),         0, -12, OFF, -28, 6500, 2500, -2, 0, 0, OFF, -6,   F3V(40, 50, 50), 40, 20, 0, 0 },
    { "S",  PH_FRIC,                   7, 105,  60, F3V(320, 1390, 2530), F3V(200, 80, 200), F3V(0, 0, 0),         0, OFF, OFF,  -2, 5500, 1500,  0, 8000, 2500, -6, OFF, F3V(40, 45, 50), 40, 20, 0, 0 },
    { "Z",  PH_FRIC | PH_VOICED,       7,  75,  40, F3V(240, 1390, 2530), F3V(70, 60, 180),  F3V(0, 0, 0),         0, -12, OFF,  -8, 5500, 1500,  0, 8000, 2500, -6, OFF, F3V(40, 45, 50), 40, 20, 0, 0 },
    { "SH", PH_FRIC,                   7, 105,  80, F3V(300, 1840, 2750), F3V(200, 100, 300), F3V(0, 0, 0),        0, OFF, OFF,   6, 2700, 500,   0, 3800, 1200, -6, OFF, F3V(40, 40, 50), 40, 20, 0, 0 },
    { "ZH", PH_FRIC | PH_VOICED,       7,  70,  40, F3V(220, 1840, 2750), F3V(60, 90, 300),  F3V(0, 0, 0),         0, -12, OFF,   0, 2700, 500,   0, 3800, 1200, -6, OFF, F3V(40, 40, 50), 40, 20, 0, 0 },
    { "HH", PH_ASPIR,                  1,  80,  20, F3V(500, 1500, 2500), F3V(200, 150, 300), F3V(0, 0, 0),        0, OFF, -6, OFF, HISS_NONE,                                 F3V(50, 50, 50), 30, 30, 0, 0 },

    // Stops: the formants are those at the closure; burst spectrum in hiss A / B
    { "P",  PH_STOP,                   8,  90,  50, F3V(400, 800, 2150), F3V(300, 150, 220), F3V(0, 0, 0),        0, OFF, -8,  -6, 0, 0, OFF, 0, 0, OFF, 0,          F3V(30, 65, 80), 50, 10, 8, 50 },
    { "B",  PH_STOP | PH_VOICED,       8,  85,  60, F3V(200, 800, 2150), F3V(60, 110, 130), F3V(0, 0, 0),         0, -16, OFF, -16, 0, 0, OFF, 0, 0, OFF, 0,          F3V(30, 65, 80), 50, 10, 5, 5 },
    { "T",  PH_STOP,                   8,  75,  50, F3V(400, 1600, 2600), F3V(300, 120, 250), F3V(0, 0, 0),        0, OFF, -8,  -4, 4500, 2000, 0, 7000, 2500, -4, OFF, F3V(30, 45, 50), 50, 10, 15, 55 },
    { "D",  PH_STOP | PH_VOICED,       8,  75,  50, F3V(200, 1600, 2600), F3V(60, 100, 170), F3V(0, 0, 0),         0, -16, OFF, -12, 4500, 2000, 0, 7000, 2500, -4, OFF, F3V(30, 45, 50), 50, 10, 10, 8 },
    { "K",  PH_STOP,                   8,  80,  60, F3V(300, 2300, 2100), F3V(250, 160, 330), F3V(0, 0, 0),        0, OFF, -8,   0, 2200, 600, 0, 3300, 1200, -4, OFF,  F3V(30, 75, 50), 60, 10, 25, 60 },
    { "G",  PH_STOP | PH_VOICED,       8,  80,  60, F3V(200, 2300, 2100), F3V(60, 150, 280), F3V(0, 0, 0),         0, -16, OFF, -16, 2200, 600, 0, 3300, 1200, -4, OFF,  F3V(30, 75, 50), 60, 10, 18, 10 },
    { "CH", PH_AFFRIC,                 8, 140, 100, F3V(300, 1840, 2750), F3V(200, 100, 300), F3V(0, 0, 0),        0, OFF, OFF,   0, 2700, 500,   0, 3800, 1200, -6, OFF, F3V(30, 40, 50), 50, 10, 0, 0 },
    { "JH", PH_AFFRIC | PH_VOICED,     8, 110,  70, F3V(220, 1840, 2750), F3V(60, 90, 300),  F3V(0, 0, 0),         0, -8, OFF,  -6, 2700, 500,   0, 3800, 1200, -6, OFF, F3V(30, 40, 50), 50, 10, 0, 0 },

    // Pauses (durations in ms; '_' is a short gap between words)
    { "_",  PH_PAUSE,                  0,  80,  80, F3V(500, 1500, 2500), F3V(100, 100, 100), F3V(0, 0, 0),        0, OFF, OFF, OFF, HISS_NONE, F3V(100, 100, 100), 0, 0, 0, 0 },
    { ",",  PH_PAUSE,                  0, 220, 220, F3V(500, 1500, 2500), F3V(100, 100, 100), F3V(0, 0, 0),        0, OFF, OFF, OFF, HISS_NONE, F3V(100, 100, 100), 0, 0, 0, 0 },
    { ".",  PH_PAUSE,                  0, 450, 450, F3V(500, 1500, 2500), F3V(100, 100, 100), F3V(0, 0, 0),        0, OFF, OFF, OFF, HISS_NONE, F3V(100, 100, 100), 0, 0, 0, 0 },
    { "!",  PH_PAUSE,                  0, 450, 450, F3V(500, 1500, 2500), F3V(100, 100, 100), F3V(0, 0, 0),        0, OFF, OFF, OFF, HISS_NONE, F3V(100, 100, 100), 0, 0, 0, 0 },
    { "?",  PH_PAUSE,                  0, 450, 450, F3V(500, 1500, 2500), F3V(100, 100, 100), F3V(0, 0, 0),        0, OFF, OFF, OFF, HISS_NONE, F3V(100, 100, 100), 0, 0, 0, 0 },
};

#define PHONEME_COUNT ((int)(sizeof(phonemes) / sizeof(phonemes[0])))

// Fixed upper formants and the nasal pole, Hz. Klatt stopped at F5,
// which was enough at a 10 kHz sample rate; at 44.1 kHz the cascade then
// falls off a cliff above 4 kHz and the voice sounds muffled, so the
// series continues at the roughly 1 kHz spacing of a uniform tube.
static const float upper_f[5] = { 3500.0f, 4500.0f, 5500.0f, 6500.0f, 7500.0f };
static const float upper_b[5] = { 250.0f, 300.0f, 500.0f, 700.0f, 900.0f };
#define FNP_HZ 270.0f
#define BNP_HZ 100.0f

// Output scaling of the two branches: speech at the default voice peaks
// at about 0.8
#define CASCADE_GAIN 0.012f
#define HISS_GAIN 0.35f

// --- Helpers ---------------------------------------------------------------

static inline float fs_speech_clamp(float x, float lo, float hi) {
    return x < lo ? lo : x > hi ? hi : x;
}

static float db_to_amp(int db) {
    return db <= OFF ? 0.0f : powf(10.0f, (float)db / 20.0f);
}

static const PhonDef *find_phoneme(const char *name, size_t len) {
    for (int i = 0; i < PHONEME_COUNT; ++i) {
        if (strlen(phonemes[i].name) != len) continue;
        size_t k = 0;
        while (k < len && toupper((unsigned char)name[k]) == phonemes[i].name[k]) ++k;
        if (k == len) return &phonemes[i];
    }
    return NULL;
}

static const PhonDef *pause_def(char c) {
    return find_phoneme(&c, 1);
}

// White noise in [-1, 1) from a 32-bit LCG
static inline float noise(uint32_t *s) {
    *s = *s * 1664525u + 1013904223u;
    return (float)(int32_t)*s * (1.0f / 2147483648.0f);
}

// Two pole resonator at f Hz, bandwidth bw Hz, unity gain at DC (the
// Klatt form, used in the cascade)
static void resonator_set(FsSpeechResonator *r, float f, float bw, float rate) {
    if (f > 0.45f * rate) f = 0.45f * rate;
    const float T = 1.0f / rate;
    const float rr = expf(-(float)M_PI * bw * T);
    r->c = -rr * rr;
    r->b = 2.0f * rr * cosf(2.0f * (float)M_PI * f * T);
    r->a = 1.0f - r->b - r->c;
}

// Band pass at f Hz, bandwidth bw Hz, unity gain at the peak and none at
// DC (the "constant 0 dB peak" filter of the RBJ cookbook). Hiss has its
// energy high up; a plain resonator would still let low noise through.
static void bandpass_set(FsSpeechBandpass *r, float f, float bw, float rate) {
    if (f > 0.45f * rate) f = 0.45f * rate;
    const float w0 = 2.0f * (float)M_PI * f / rate;
    const float alpha = sinf(w0) * 0.5f * bw / f;   // sin(w0) / (2 Q), Q = f / bw
    const float inv = 1.0f / (1.0f + alpha);
    r->b0 = alpha * inv;
    r->a1 = -2.0f * cosf(w0) * inv;
    r->a2 = (1.0f - alpha) * inv;
}

static inline float bandpass(FsSpeechBandpass *r, float x) {
    const float y = r->b0 * (x - r->x2) - r->a1 * r->y1 - r->a2 * r->y2;
    r->x2 = r->x1;
    r->x1 = x;
    r->y2 = r->y1;
    r->y1 = y;
    return y;
}

static inline float resonate(FsSpeechResonator *r, float x) {
    const float y = r->a * x + r->b * r->y1 + r->c * r->y2;
    r->y2 = r->y1;
    r->y1 = y;
    return y;
}

// Anti-resonator: the inverse filter of a resonator (a notch). y1/y2 hold
// the previous inputs.
static void antiresonator_set(FsSpeechResonator *r, float f, float bw, float rate) {
    resonator_set(r, f, bw, rate);
    const float inv = 1.0f / r->a;
    r->a = inv;
    r->b = -r->b * inv;
    r->c = -r->c * inv;
}

static inline float antiresonate(FsSpeechResonator *r, float x) {
    const float y = r->a * x + r->b * r->y1 + r->c * r->y2;
    r->y2 = r->y1;
    r->y1 = x;
    return y;
}

// --- Tracks ------------------------------------------------------------------

static void track_reset(FsSpeechTrack *t) {
    t->count = 0;
    t->cursor = 0;
}

// Append a keyframe. Times must not go backwards; a key earlier than the
// last one is moved to the last one's time.
static void track_add(FsSpeechTrack *t, float time, const float *v, int n) {
    if (t->count >= FS_SPEECH_MAX_KEYS) return;
    if (t->count > 0 && time < t->key[t->count - 1].t) time = t->key[t->count - 1].t;
    FsSpeechKey *k = &t->key[t->count++];
    k->t = time;
    memcpy(k->v, v, (size_t)n * sizeof(float));
}

// Values of the track at time t (which must not go backwards between
// calls): linear between keys, held before the first and after the last.
static void track_eval(FsSpeechTrack *t, float time, float *out, int n) {
    if (t->count == 0) {
        memset(out, 0, (size_t)n * sizeof(float));
        return;
    }
    while (t->cursor + 1 < t->count && t->key[t->cursor + 1].t <= time) ++t->cursor;
    const FsSpeechKey *a = &t->key[t->cursor];
    if (t->cursor + 1 >= t->count || time <= a->t) {
        memcpy(out, a->v, (size_t)n * sizeof(float));
        return;
    }
    const FsSpeechKey *b = a + 1;
    const float x = (time - a->t) / (b->t - a->t);
    for (int i = 0; i < n; ++i) out[i] = a->v[i] + (b->v[i] - a->v[i]) * x;
}

// --- Layout --------------------------------------------------------------------

// One phoneme of the utterance as parsed
typedef struct {
    const PhonDef *def;     // The table entry, or &own for an allophone
    PhonDef own;            // A copy changed by the allophone rules
    int stress;             // 0 unstressed, 1 primary, 2 secondary
    int note;               // Sung MIDI note (@N), or -1
    float len_ms;           // Sung length (:MS) of the syllable, or 0
    float dur;              // Samples
} Phone;

// A formant segment: one sound's stretch of the tract track
typedef struct {
    const PhonDef *def;
    float start, end;       // Samples
    float v0[FS_SP_TRACT_COUNT];    // Targets at the start
    float v1[FS_SP_TRACT_COUNT];    // and the end (differ for diphthongs)
} TractSeg;

// A source segment
typedef struct {
    float start, end;
    float v[FS_SP_SOURCE_COUNT];
    float ramp;             // Crossfade from the previous segment, samples
} SourceSeg;

// Scratch space for the layout. Static, so FsSpeech itself stays the only
// large object; fs_speech_say is not reentrant.
static Phone phones[FS_SPEECH_MAX_PHONEMES + 2];
static TractSeg tsegs[FS_SPEECH_MAX_PHONEMES + 2];
static SourceSeg ssegs[(FS_SPEECH_MAX_PHONEMES + 2) * 3];

// Fill the tract targets of a phoneme
static void tract_targets(const PhonDef *d, float scale, float *v0, float *v1) {
    for (int i = 0; i < 3; ++i) {
        v0[FS_SP_F1 + i] = d->f[i] * scale;
        v0[FS_SP_B1 + i] = d->b[i];
        v1[FS_SP_F1 + i] = (d->f_end[0] ? d->f_end[i] : d->f[i]) * scale;
        v1[FS_SP_B1 + i] = d->b[i];
    }
    v0[FS_SP_FNZ] = v1[FS_SP_FNZ] = d->fnz ? d->fnz : FNP_HZ;
}

// Fill the source values of a phoneme as heard in its main part
static void source_values(const PhonDef *d, float *v, float level) {
    v[FS_SP_AV] = db_to_amp(d->av) * level;
    v[FS_SP_AH] = db_to_amp(d->ah) * level;
    v[FS_SP_AF] = db_to_amp(d->af) * level;
    v[FS_SP_FA] = d->fa;
    v[FS_SP_BA] = d->ba ? d->ba : 1000.0f;
    v[FS_SP_GA] = db_to_amp(d->ga);
    v[FS_SP_FB] = d->fb;
    v[FS_SP_BB] = d->bb ? d->bb : 1000.0f;
    v[FS_SP_GB] = db_to_amp(d->gb);
    v[FS_SP_BYPASS] = db_to_amp(d->bypass);
}

// The aspiration of p t k before r l w: the air rushes through the
// approximant's narrow channel, so the start of the r ("pray", "cry",
// "truck" - after t almost "chr"), l ("play", "clay") or w ("twin",
// "queen") is a voiceless hiss. Plain breath through r's low formants is
// too weak and dull to carry the stop, and "truck" was heard as "rock".
// D before r gets voiced friction the same way ("drain", nearly "jrain").
static bool is_name(const PhonDef *d, const char *name);
static void approx_friction(const PhonDef *a, const PhonDef *stop, float *v) {
    float af = -4, fa, ba, fb, bb, gb = -6;
    if (is_name(a, "R")) {
        if (is_name(stop, "D")) {
            // "dr": voiced friction, like the start of "j"
            v[FS_SP_AV] = db_to_amp(-8);
            af = -6;
            fa = 2700; ba = 600; fb = 3800; bb = 1200;
        } else if (is_name(stop, "T")) {
            af = 6;         // As strong as sh
            fa = 2700; ba = 600; fb = 3800; bb = 1200;
        } else {
            fa = 1800; ba = 700; fb = 3000; bb = 1500; gb = -10;
        }
    } else if (is_name(a, "L")) {
        af -= 4;
        fa = 3000; ba = 2500; fb = 6000; bb = 3000; gb = -10;
    } else if (is_name(a, "W")) {
        af -= 4;
        fa = 1200; ba = 800; fb = 2400; bb = 1500; gb = -12;
    } else {
        return;     // y: plain aspiration ("cute")
    }
    v[FS_SP_AF] = db_to_amp(af);
    v[FS_SP_AH] *= db_to_amp(-10);      // The friction, not the breath, dominates
    v[FS_SP_FA] = fa;
    v[FS_SP_BA] = ba;
    v[FS_SP_GA] = 1.0f;
    v[FS_SP_FB] = fb;
    v[FS_SP_BB] = bb;
    v[FS_SP_GB] = db_to_amp(gb);
    v[FS_SP_BYPASS] = 0;
}

// Parse a decimal number at *p (advancing it); -1 if there is none
static long parse_number(const char **p, const char *end) {
    if (*p >= end || !isdigit((unsigned char)**p)) return -1;
    long v = 0;
    while (*p < end && isdigit((unsigned char)**p) && v < 1000000) v = v * 10 + (*(*p)++ - '0');
    return v;
}

// Parse the string into phones[]. Returns the count, or -(1-based token
// position) of a bad token.
static int parse(const char *s, int *count) {
    int n = 0, token = 0;
    while (*s) {
        while (*s && isspace((unsigned char)*s)) ++s;
        if (!*s) break;
        ++token;
        const char *start = s;
        while (*s && !isspace((unsigned char)*s) && !strchr(",.!?_", *s)) ++s;
        size_t len = (size_t)(s - start);
        if (len == 0) {             // Punctuation on its own (or after a word)
            len = 1;
            ++s;
            // A sung rest may have a length: "_:500"
            if (*start == '_' && *s == ':') {
                while (*s && !isspace((unsigned char)*s)) ++s;
                len = (size_t)(s - start);
            }
        }
        // Singing suffixes: @NOTE and :MS
        const char *tok_end = start + len;
        const char *suffix = start;
        while (suffix < tok_end && *suffix != '@' && *suffix != ':') ++suffix;
        int note = -1;
        long len_ms = 0;
        for (const char *q = suffix; q < tok_end;) {
            const char c = *q++;
            const long v = parse_number(&q, tok_end);
            if (v < 0) return -token;
            if (c == '@' && v <= 127) note = (int)v;
            else if (c == ':' && v > 0 && v <= 60000) len_ms = v;
            else return -token;
        }
        len = (size_t)(suffix - start);
        if (len == 0) return -token;
        int stress = 1;
        if (len > 1 && isdigit((unsigned char)start[len - 1])) {
            stress = start[len - 1] - '0';
            if (stress > 2) return -token;
            --len;
        }
        const PhonDef *d = find_phoneme(start, len);
        if (!d) return -token;
        if (n >= FS_SPEECH_MAX_PHONEMES) return -token;
        // Notes go on vowels, lengths on vowels and pauses
        if ((note >= 0 && !(d->flags & PH_VOWEL)) || (len_ms && !(d->flags & (PH_VOWEL | PH_PAUSE)))) return -token;
        phones[n].def = d;
        phones[n].stress = (d->flags & PH_VOWEL) ? stress : 0;
        phones[n].note = note;
        phones[n].len_ms = (float)len_ms;
        phones[n].dur = 0;
        ++n;
    }
    *count = n;
    return 0;
}

static bool is_phrase_end(const PhonDef *d) {
    return (d->flags & PH_PAUSE) && d->name[0] != '_';
}

// Make phones[i] an allophone: returns its own copy of the table entry to
// change
static PhonDef *allophone(int i) {
    if (phones[i].def != &phones[i].own) {
        phones[i].own = *phones[i].def;
        phones[i].def = &phones[i].own;
    }
    return &phones[i].own;
}

static bool is_name(const PhonDef *d, const char *name) {
    return strcmp(d->name, name) == 0;
}

// Context dependent variants of phonemes (the allophones of English that
// matter most for being understood):
//   - "dark" L: an L not followed by a vowel ("fall", "milk") has a low F2,
//   - stops after S are not aspirated ("spin", "stop", "skin"),
//   - T and D between a vowel and an unstressed vowel are a quick voiced
//     tap, the American flap ("butter", "ladder", "steady"),
//   - voiceless stops before a pause are released more softly,
//   - a stop before another stop is not released ("lapped", "act"),
//   - p t k before r l w: the voice starts later ("truck", "play"),
//   - D between N or L and another consonant is dropped, as speakers
//     almost always do ("and this" -> "an' this", "old man" -> "ol' man");
//     said in full, its closure is a gap that splits the words apart.
// Returns the new number of phonemes.
static int allophones(int n) {
    int m = 0;
    for (int i = 0; i < n; ++i) {
        const PhonDef *d = phones[i].def;
        if (i > 0 && i + 1 < n && is_name(d, "D") && phones[i].note < 0 && phones[i].len_ms <= 0 &&
            (is_name(phones[i - 1].def, "N") || is_name(phones[i - 1].def, "L")) &&
            !(phones[i + 1].def->flags & (PH_VOWEL | PH_PAUSE | PH_APPROX))) continue;
        phones[m++] = phones[i];
        if (phones[m - 1].def == &phones[i].own) phones[m - 1].def = &phones[m - 1].own;
    }
    n = m;
    for (int i = 1; i < n; ++i) {
        const PhonDef *d = phones[i].def;
        const PhonDef *prev = phones[i - 1].def;
        const PhonDef *next = i + 1 < n ? phones[i + 1].def : NULL;
        const bool next_vowel = next && (next->flags & PH_VOWEL);
        if (is_name(d, "L") && !next_vowel) {
            PhonDef *a = allophone(i);
            a->f[0] = 450;
            a->f[1] = 850;
            a->f[2] = 2650;
            a->b[1] = 80;
        } else if ((d->flags & PH_STOP) && !(d->flags & PH_VOICED) && is_name(prev, "S")) {
            PhonDef *a = allophone(i);
            a->vot = 10;
            a->dur = 60;
            a->min_dur = 40;
        } else if ((d->flags & PH_STOP) && !(d->flags & PH_VOICED) && next && (next->flags & PH_APPROX) &&
                   !is_name(next, "Y")) {
            // Before r l w the air takes longer to get through, so the
            // voice starts later: with the plain delay "tr" sounds "dr"
            PhonDef *a = allophone(i);
            a->vot += 25;
        } else if (is_name(d, "D") && next && is_name(next, "R")) {
            PhonDef *a = allophone(i);
            a->vot += 15;           // Voiced friction before the r ("drain")
        } else if ((is_name(d, "T") || is_name(d, "D")) && next_vowel && phones[i + 1].stress == 0 &&
                   ((prev->flags & PH_VOWEL) || is_name(prev, "R"))) {
            PhonDef *a = allophone(i);
            a->flags |= PH_VOICED;
            a->av = -10;            // Voicing carries on through the tap
            a->af = -20;
            a->burst = 2;
            a->vot = 0;
            a->dur = a->min_dur = 25;
            a->f[0] = 300;
        } else if ((d->flags & PH_STOP) && next && (next->flags & (PH_STOP | PH_AFFRIC))) {
            // A stop before another stop is held, not released: one long
            // closure, then the second stop's burst ("lapped", "act")
            PhonDef *a = allophone(i);
            a->burst = 0;
            a->vot = 0;
        } else if ((d->flags & PH_STOP) && !(d->flags & PH_VOICED) && next && (next->flags & PH_PAUSE)) {
            PhonDef *a = allophone(i);
            a->vot = 30;
            a->af -= 6;
        }
    }
    return n;
}

// How the speaking rate changes timing. Talking faster mostly shortens
// the stretchable part of each sound (vowels most of all, and pauses);
// the minimum duration of a sound, and its fast events (bursts,
// aspiration, formant glides), shrink much less, because they carry the
// sound's identity. Both shrink by the square root of the speed: with
// everything scaled by the speed, -s 2 was close to unintelligible.
static float speed_art(float speed) { return sqrtf(speed); }

// Durations in ms by a reduced set of Klatt's rules:
//   dur = min / sqrt(speed) + (inherent - min) * percent / speed
// with the percent shortened for unstressed vowels and consonant clusters
// and lengthened at the end of a phrase.

static void durations(int n, float speed, float rate) {
    for (int i = 0; i < n; ++i) {
        const PhonDef *d = phones[i].def;
        float pct = 1.0f;
        if (d->flags & PH_PAUSE) {
            phones[i].dur = d->dur * rate / 1000.0f / speed;
            continue;
        }
        if ((d->flags & PH_VOWEL) && phones[i].stress == 0) pct *= 0.5f;
        if (!(d->flags & PH_VOWEL)) {
            const bool prev_c = i > 0 && !(phones[i - 1].def->flags & (PH_VOWEL | PH_PAUSE));
            const bool next_c = i + 1 < n && !(phones[i + 1].def->flags & (PH_VOWEL | PH_PAUSE));
            if (prev_c || next_c) pct *= 0.7f;
        }
        // Phrase final lengthening: this phoneme is in the last syllable,
        // i.e. no vowel between it and the end of the phrase... except
        // that the last vowel itself counts.
        int j = i + 1;
        while (j < n && !(phones[j].def->flags & (PH_VOWEL | PH_PAUSE))) ++j;
        if (j >= n || is_phrase_end(phones[j].def)) {
            // Only from the last vowel on
            int k = i;
            while (k >= 0 && !(phones[k].def->flags & (PH_VOWEL | PH_PAUSE))) --k;
            if (k >= 0 && (phones[k].def->flags & PH_VOWEL)) pct *= 1.4f;
        }
        // The consonant after a vowel changes its length (Klatt's rule 7):
        // English listeners hear "cap" / "cab" largely by the vowel being
        // short before a voiceless stop and long before a voiced one. The
        // effect is full at the end of a phrase and 30% of it elsewhere.
        if ((d->flags & PH_VOWEL) && i + 1 < n) {
            const PhonDef *c = phones[i + 1].def;
            float k = 1.0f;
            if (c->flags & PH_PAUSE) k = 1.2f;
            else if ((c->flags & PH_FRIC) && (c->flags & PH_VOICED)) k = 1.6f;
            else if ((c->flags & (PH_STOP | PH_AFFRIC)) && (c->flags & PH_VOICED)) k = 1.2f;
            else if (c->flags & PH_NASAL) k = 0.85f;
            else if (c->flags & (PH_STOP | PH_AFFRIC)) k = 0.7f;
            // j: the next vowel or pause, found above
            const bool final = j >= n || is_phrase_end(phones[j].def);
            pct *= final ? k : 0.7f + 0.3f * k;
        }
        phones[i].dur = (d->min_dur / speed_art(speed) + (d->dur - d->min_dur) * pct / speed) * rate / 1000.0f;
    }

    // Sung lengths: a syllable's length covers its vowel and the
    // consonants after it, up to the next vowel, so the vowels land on the
    // beat. The consonants keep their spoken length and the vowel takes
    // the rest (at least 40% of the note).
    for (int i = 0; i < n; ++i) {
        if (phones[i].len_ms <= 0) continue;
        const float len = phones[i].len_ms * rate / 1000.0f;
        if (phones[i].def->flags & PH_PAUSE) {
            phones[i].dur = len;
            continue;
        }
        float cons = 0;
        for (int j = i + 1; j < n && !(phones[j].def->flags & (PH_VOWEL | PH_PAUSE)); ++j) {
            const PhonDef *c = phones[j].def;
            cons += phones[j].dur;
            if (c->flags & PH_STOP) cons += (c->burst + c->vot) * rate / 1000.0f / speed_art(speed);
        }
        phones[i].dur = fmaxf(len - cons, 0.4f * len);
    }
}

// The value at the boundary between a (left) and b (right) for one tract
// parameter, and the glide times on each side (samples)
static void boundary(const TractSeg *a, const TractSeg *b, int p, float ms,
                     float *value, float *t_left, float *t_right) {
    const PhonDef *da = a->def, *db = b->def;
    const float va = a->v1[p], vb = b->v0[p];
    const int fi = p == FS_SP_FNZ ? -1 : p % 3;     // Formant index for prop
    if (da->rank == db->rank) {
        *value = 0.5f * (va + vb);
        *t_left = da->t_int * ms;
        *t_right = db->t_int * ms;
    } else if (da->rank > db->rank) {
        const float prop = fi < 0 ? 0.0f : da->prop[fi] * 0.01f;
        *value = va + prop * (vb - va);
        *t_left = da->t_int * ms;
        *t_right = da->t_ext * ms;
    } else {
        const float prop = fi < 0 ? 0.0f : db->prop[fi] * 0.01f;
        *value = vb + prop * (va - vb);
        *t_left = db->t_ext * ms;
        *t_right = db->t_int * ms;
    }
}

// Build the tract track from the formant segments
static void build_tract(FsSpeech *sp, int n) {
    const float ms = sp->rate / 1000.0f / speed_art(sp->voice.speed);
    FsSpeechTrack *tr = &sp->tract;
    float bval[FS_SPEECH_MAX_PHONEMES + 2][FS_SP_TRACT_COUNT];
    float tl[FS_SPEECH_MAX_PHONEMES + 2], trr[FS_SPEECH_MAX_PHONEMES + 2];

    // Boundaries: bval[i] is between segment i and i + 1. The glide times
    // use the longest of the parameters (all formants move together).
    for (int i = 0; i + 1 < n; ++i) {
        tl[i] = trr[i] = 0;
        for (int p = 0; p < FS_SP_TRACT_COUNT; ++p) {
            float l, r;
            boundary(&tsegs[i], &tsegs[i + 1], p, ms, &bval[i][p], &l, &r);
            if (l > tl[i]) tl[i] = l;
            if (r > trr[i]) trr[i] = r;
        }
    }

    for (int i = 0; i < n; ++i) {
        const TractSeg *s = &tsegs[i];
        const float len = s->end - s->start;
        float tin = i > 0 ? trr[i - 1] : 0;
        float tout = i + 1 < n ? tl[i] : 0;
        if (tin + tout > len) {         // Short sound: the target is not reached
            const float k = len / (tin + tout);
            tin *= k;
            tout *= k;
        }
        float v[FS_SP_SOURCE_COUNT] = { 0 };
        if (i == 0) track_add(tr, s->start, s->v0, FS_SP_TRACT_COUNT);
        // Start of the steady part, end of it, boundary with the next
        float in_v[FS_SP_TRACT_COUNT], out_v[FS_SP_TRACT_COUNT];
        memcpy(in_v, s->v0, sizeof(in_v));
        memcpy(out_v, s->v1, sizeof(out_v));
        if (tin + tout >= len && i > 0 && i + 1 < n) {
            // No steady part: a single turning point between the glides
            for (int p = 0; p < FS_SP_TRACT_COUNT; ++p) v[p] = 0.5f * (in_v[p] + out_v[p]);
            track_add(tr, s->start + tin, v, FS_SP_TRACT_COUNT);
        } else {
            track_add(tr, s->start + tin, in_v, FS_SP_TRACT_COUNT);
            track_add(tr, s->end - tout, out_v, FS_SP_TRACT_COUNT);
        }
        if (i + 1 < n) track_add(tr, s->end, bval[i], FS_SP_TRACT_COUNT);
        else track_add(tr, s->end, s->v1, FS_SP_TRACT_COUNT);
    }
}

// Build the source track: hold each segment's values, crossfading over
// the entered segment's ramp at each boundary
static void build_source(FsSpeech *sp, int n) {
    FsSpeechTrack *tr = &sp->source;
    for (int i = 0; i < n; ++i) {
        const SourceSeg *s = &ssegs[i];
        if (i == 0) {
            track_add(tr, s->start, s->v, FS_SP_SOURCE_COUNT);
            continue;
        }
        const SourceSeg *p = &ssegs[i - 1];
        float r = s->ramp;
        const float lim = fminf(p->end - p->start, s->end - s->start);
        if (r > lim) r = lim;
        track_add(tr, s->start - 0.5f * r, p->v, FS_SP_SOURCE_COUNT);
        track_add(tr, s->start + 0.5f * r, s->v, FS_SP_SOURCE_COUNT);
    }
    if (n > 0) track_add(tr, ssegs[n - 1].end, ssegs[n - 1].v, FS_SP_SOURCE_COUNT);
}

// A stressed or unstressed vowel's place in time, for the pitch contour
typedef struct {
    float start, end;
    int stress;
} VowelTime;

static VowelTime vtimes[FS_SPEECH_MAX_PHONEMES + 2];

static float note_hz(int note) {
    return 440.0f * powf(2.0f, (note - 69) / 12.0f);
}

// Build the pitch track for singing: each vowel glides to its note (a
// vowel without a note keeps the last one) and holds it, with vibrato
// (the track's second value, in semitones) growing in after the onset
static void build_pitch_sung(FsSpeech *sp, int n) {
    FsSpeechTrack *tr = &sp->pitch;
    const float ms = sp->rate / 1000.0f;
    float v[FS_SP_SOURCE_COUNT] = { 0 };
    float f = sp->voice.pitch_hz;
    for (int i = 0; i < n; ++i) {
        if (phones[i].note >= 0) {
            f = note_hz(phones[i].note);
            break;
        }
    }
    v[0] = f;
    track_add(tr, 0, v, 2);
    for (int i = 0; i < n; ++i) {
        if (!(phones[i].def->flags & PH_VOWEL)) continue;
        const VowelTime *vt = &vtimes[i];
        const float len = vt->end - vt->start;
        const float target = phones[i].note >= 0 ? note_hz(phones[i].note) : f;
        const float glide = fminf(60.0f * ms, 0.3f * len);
        const float vib_in = fminf(350.0f * ms, 0.8f * len);
        v[0] = f;
        v[1] = 0;
        track_add(tr, vt->start, v, 2);
        v[0] = target;
        track_add(tr, vt->start + glide, v, 2);
        if (vib_in > glide) {
            v[1] = sp->voice.vibrato;
            track_add(tr, vt->start + vib_in, v, 2);
        }
        track_add(tr, vt->end, v, 2);
        f = target;
    }
}

// Build the pitch track: per phrase a gently falling line (declination),
// a rise-fall accent on stressed vowels and a fall (statement) or rise
// (question) at the end
static void build_pitch(FsSpeech *sp, int n) {
    FsSpeechTrack *tr = &sp->pitch;
    const float base = sp->voice.pitch_hz;
    const float acc = sp->voice.accent;
    float v[FS_SP_SOURCE_COUNT] = { 0 };
    int i = 0;
    while (i < n) {
        // Skip pauses, then find the phrase [i, j) and its ending
        while (i < n && (phones[i].def->flags & PH_PAUSE)) ++i;
        if (i >= n) break;
        int j = i;
        while (j < n && !is_phrase_end(phones[j].def)) ++j;
        const char end = j < n ? phones[j].def->name[0] : '.';
        const float ps = tsegs[i].start;
        const float pe = j > i ? vtimes[j - 1].end : ps + 1;
        int last_vowel = -1;
        for (int k = i; k < j; ++k) if (phones[k].def->flags & PH_VOWEL) last_vowel = k;
        const float span = pe > ps ? pe - ps : 1;
#define DECL(t) (base * (1.08f - 0.18f * ((t) - ps) / span))
        v[0] = DECL(ps);
        track_add(tr, ps, v, 1);
        for (int k = i; k < j; ++k) {
            if (!(phones[k].def->flags & PH_VOWEL)) continue;
            const VowelTime *vt = &vtimes[k];
            const float len = vt->end - vt->start;
            const float a = vt->stress == 1 ? acc : vt->stress == 2 ? 0.5f * acc : 0.0f;
            if (k == last_vowel && end == '?') {
                v[0] = DECL(vt->start) * (1.0f + 0.5f * a);
                track_add(tr, vt->start + 0.2f * len, v, 1);
                v[0] = base * 1.45f;
                track_add(tr, vt->end, v, 1);
            } else if (k == last_vowel && end != ',') {
                v[0] = DECL(vt->start) * (1.0f + a);
                track_add(tr, vt->start + 0.25f * len, v, 1);
                v[0] = base * 0.78f;
                track_add(tr, vt->end, v, 1);
            } else if (k == last_vowel) {   // Continuation: slight rise
                v[0] = DECL(vt->start) * (1.0f + 0.5f * a);
                track_add(tr, vt->start + 0.3f * len, v, 1);
                v[0] = base * 1.05f;
                track_add(tr, vt->end, v, 1);
            } else {
                v[0] = DECL(vt->start + 0.35f * len) * (1.0f + a);
                track_add(tr, vt->start + 0.35f * len, v, 1);
            }
        }
#undef DECL
        i = j;
    }
    if (tr->count == 0) {
        v[0] = base;
        track_add(tr, 0, v, 1);
    }
}

// --- API -------------------------------------------------------------------------

void fs_speech_init(FsSpeech *sp, float sample_rate) {
    memset(sp, 0, sizeof(*sp));
    sp->rate = sample_rate;
    sp->voice.pitch_hz = 80.0f;
    sp->voice.speed = 1.0f;
    sp->voice.formant_scale = 1.0f;
    sp->voice.accent = 0.3f;
    sp->voice.breathiness = 0.5f;
    sp->voice.brightness = 0.5f;
    sp->voice.gain = 0.85f;
    sp->voice.vibrato = 0.3f;
    sp->voice.vibrato_hz = 5.5f;
    sp->noise = 0x12345678u;
    sp->period = sample_rate / sp->voice.pitch_hz;
    sp->pulse_gain = 1.0f;
}

int fs_speech_say(FsSpeech *sp, const char *str) {
    int n = 0;
    const int err = parse(str, &n);
    if (err) return -err;

    // Lead in, and end with a phrase end so the last phrase falls
    memmove(&phones[1], &phones[0], (size_t)n * sizeof(Phone));
    phones[0].def = pause_def('_');
    phones[0].stress = 0;
    phones[0].note = -1;
    phones[0].len_ms = 0;
    ++n;
    if (!is_phrase_end(phones[n - 1].def)) {
        phones[n].def = pause_def('.');
        phones[n].stress = 0;
        phones[n].note = -1;
        phones[n].len_ms = 0;
        ++n;
    }
    bool sung = false;
    for (int i = 0; i < n; ++i) if (phones[i].note >= 0 || phones[i].len_ms > 0) sung = true;

    const float rate = sp->rate;
    const float ms = rate / 1000.0f / speed_art(sp->voice.speed);
    const float fscale = sp->voice.formant_scale;
    n = allophones(n);
    durations(n, sp->voice.speed, rate);

    // Formant and source segments
    int ns = 0;
    float t_src = 0, t_form = 0;
    for (int i = 0; i < n; ++i) {
        const PhonDef *d = phones[i].def;
        const float dur = phones[i].dur;
        TractSeg *ts = &tsegs[i];
        ts->def = d;
        ts->start = t_form;
        tract_targets(d, fscale, ts->v0, ts->v1);
        const float level = (d->flags & PH_VOWEL) && phones[i].stress == 0 && !sung ? 0.75f : 1.0f;
        SourceSeg *ss = &ssegs[ns];

        if (d->flags & PH_STOP) {
            // Closure: silence, or a low voice bar for b d g
            ss->start = t_src;
            ss->end = t_src + dur;
            source_values(d, ss->v, 1.0f);
            ss->v[FS_SP_AH] = ss->v[FS_SP_AF] = 0;
            ss->ramp = 8 * ms;
            ts->end = t_form = ss->end;        // The next formant glide starts at the release
            t_src = ss->end;
            ++ns;
            // Burst: hiss with the place's spectrum, quick attack
            ss = &ssegs[ns++];
            ss->start = t_src;
            ss->end = t_src + d->burst * ms;
            source_values(d, ss->v, 1.0f);
            ss->v[FS_SP_AH] = 0;
            ss->ramp = 1 * ms;
            if (!d->fa) ss->v[FS_SP_BYPASS] = 1.0f;      // Labial: flat burst
            if (d->fa && d->fa < 3000) {
                // Velar: the burst sits just above the F2 of the vowel it
                // is made against: the next one, or with none (the end
                // of "tuck") the one before. Before r or w (rounded and
                // pulled back) it is lower. A fixed high burst sounds
                // like "ch".
                const PhonDef *nx = i + 1 < n ? phones[i + 1].def : d;
                const PhonDef *pv = i > 0 ? phones[i - 1].def : d;
                float f = d->f[1];
                if (nx->flags & PH_VOWEL) f = nx->f[1];
                else if (pv->flags & PH_VOWEL) f = pv->f_end[1] ? pv->f_end[1] : pv->f[1];
                f = f * fscale + 200.0f;
                if (is_name(nx, "R") || is_name(nx, "W")) f = 1700.0f * fscale;
                f = fminf(fmaxf(f, 1300.0f), 3200.0f);
                ss->v[FS_SP_FA] = f;
                ss->v[FS_SP_FB] = f + 1100.0f;
            }
            if ((is_name(d, "T") || is_name(d, "D")) && i + 1 < n && is_name(phones[i + 1].def, "R")) {
                // Before r the tongue is already drawn back: the release
                // is a strong "sh" like burst, "tr" nearly "chr" and "dr"
                // nearly "jr"
                ss->v[FS_SP_AF] = db_to_amp(is_name(d, "T") ? 6 : 0);
                ss->v[FS_SP_FA] = 2700;
                ss->v[FS_SP_BA] = 600;
                ss->v[FS_SP_GA] = 1.0f;
                ss->v[FS_SP_FB] = 3800;
                ss->v[FS_SP_BB] = 1200;
                ss->v[FS_SP_GB] = db_to_amp(-6);
                ss->v[FS_SP_BYPASS] = 0;
            }
            t_src = ss->end;
            // Aspiration (long for p t k) or the onset of voicing
            ss = &ssegs[ns++];
            ss->start = t_src;
            ss->end = t_src + d->vot * ms;
            source_values(d, ss->v, 1.0f);
            ss->v[FS_SP_AF] = 0;
            ss->v[FS_SP_AV] = 0;
            ss->ramp = 2 * ms;
            if (i + 1 < n && (phones[i + 1].def->flags & PH_APPROX) &&
                (!(d->flags & PH_VOICED) || (is_name(d, "D") && is_name(phones[i + 1].def, "R"))))
                approx_friction(phones[i + 1].def, d, ss->v);
            t_src = ss->end;
            continue;
        }
        if (d->flags & PH_AFFRIC) {
            // Closure then frication; one formant segment for both
            ss->start = t_src;
            ss->end = t_src + 0.45f * dur;
            source_values(d, ss->v, 1.0f);
            ss->v[FS_SP_AF] = 0;
            ss->v[FS_SP_AV] *= 0.1f;
            ss->ramp = 8 * ms;
            ++ns;
            ss = &ssegs[ns++];
            ss->start = ssegs[ns - 2].end;
            ss->end = t_src + dur;
            source_values(d, ss->v, 1.0f);
            ss->ramp = 3 * ms;
            t_src = ts->end = t_form = ss->end;
            continue;
        }
        ss->start = t_src;
        ss->end = t_src + dur;
        source_values(d, ss->v, level);
        ss->ramp = (d->flags & PH_FRIC) ? 12 * ms : (d->flags & PH_APPROX) ? 15 * ms :
                   (d->flags & PH_NASAL) ? 6 * ms : 8 * ms;
        if (d->flags & PH_PAUSE) ss->ramp = 10 * ms;
        ++ns;
        t_src = ts->end = t_form = ss->end;
        if (d->flags & PH_VOWEL) {
            vtimes[i].start = ts->start;
            vtimes[i].end = ts->end;
            vtimes[i].stress = phones[i].stress;
        }
    }
    for (int i = 0; i < n; ++i) if (!(phones[i].def->flags & PH_VOWEL)) vtimes[i].end = tsegs[i].end;

    // Sounds that take their formants from a neighbour: h from the next
    // sound, pauses from whichever side has speech
    for (int i = 0; i < n; ++i) {
        const PhonDef *d = phones[i].def;
        if (d->flags & PH_ASPIR && i + 1 < n && !(phones[i + 1].def->flags & PH_PAUSE)) {
            memcpy(tsegs[i].v0, tsegs[i + 1].v0, sizeof(tsegs[i].v0));
            memcpy(tsegs[i].v1, tsegs[i + 1].v0, sizeof(tsegs[i].v1));
        }
    }
    for (int i = n - 1; i >= 0; --i) {
        if ((phones[i].def->flags & PH_PAUSE) && i + 1 < n) {
            memcpy(tsegs[i].v0, tsegs[i + 1].v0, sizeof(tsegs[i].v0));
            memcpy(tsegs[i].v1, tsegs[i + 1].v0, sizeof(tsegs[i].v1));
        }
    }
    for (int i = 1; i < n; ++i) {
        if ((phones[i].def->flags & PH_PAUSE) && (i + 1 >= n || (phones[i + 1].def->flags & PH_PAUSE))) {
            memcpy(tsegs[i].v0, tsegs[i - 1].v1, sizeof(tsegs[i].v0));
            memcpy(tsegs[i].v1, tsegs[i - 1].v1, sizeof(tsegs[i].v1));
        }
    }
    // Hiss spectrum where there is no hiss: copy a neighbour's so a fade
    // in or out does not sweep the filters
    for (int i = 0; i < ns; ++i) {
        if (ssegs[i].v[FS_SP_AF] > 0 || ssegs[i].v[FS_SP_AH] > 0) continue;
        int j = i + 1;
        while (j < ns && ssegs[j].v[FS_SP_AF] == 0) ++j;
        if (j >= ns) {
            j = i - 1;
            while (j >= 0 && ssegs[j].v[FS_SP_AF] == 0) --j;
        }
        if (j >= 0 && j < ns) {
            for (int p = FS_SP_FA; p < FS_SP_SOURCE_COUNT; ++p) ssegs[i].v[p] = ssegs[j].v[p];
        }
    }

    track_reset(&sp->tract);
    track_reset(&sp->source);
    track_reset(&sp->pitch);
    build_tract(sp, n);
    build_source(sp, ns);
    if (sung) build_pitch_sung(sp, n);
    else build_pitch(sp, n);

    sp->length = (uint32_t)(t_src > t_form ? t_src : t_form);
    sp->pos = 0;
    sp->block_left = 0;
    sp->tract_valid = false;
    return 0;
}

// Read the tracks for the next block and update the filters
static void update_block(FsSpeech *sp) {
    const float rate = sp->rate;
    const uint32_t left = sp->length - sp->pos;
    sp->block_len = left < FS_SPEECH_BLOCK ? left : FS_SPEECH_BLOCK;
    sp->block_left = sp->block_len;

    // Formants: read at the middle of the block, lightly smoothed (3 ms)
    float tract[FS_SP_TRACT_COUNT];
    track_eval(&sp->tract, sp->pos + 0.5f * sp->block_len, tract, FS_SP_TRACT_COUNT);
    if (!sp->tract_valid) {
        memcpy(sp->tract_now, tract, sizeof(tract));
        sp->tract_valid = true;
    } else {
        const float k = 1.0f - expf(-(float)sp->block_len / (0.003f * rate));
        for (int p = 0; p < FS_SP_TRACT_COUNT; ++p) sp->tract_now[p] += k * (tract[p] - sp->tract_now[p]);
    }
    const float *t = sp->tract_now;
    const float fs = sp->voice.formant_scale;
    resonator_set(&sp->formant[0], t[FS_SP_F1], t[FS_SP_B1], rate);
    resonator_set(&sp->formant[1], t[FS_SP_F2], t[FS_SP_B2], rate);
    resonator_set(&sp->formant[2], t[FS_SP_F3], t[FS_SP_B3], rate);
    for (int i = 0; i < 5; ++i) resonator_set(&sp->formant[3 + i], upper_f[i] * fs, upper_b[i], rate);
    resonator_set(&sp->nasal_pole, FNP_HZ, BNP_HZ, rate);
    antiresonator_set(&sp->nasal_zero, t[FS_SP_FNZ], BNP_HZ, rate);

    // Source: values at both ends of the block, ramped per sample
    memcpy(sp->src_from, sp->src_to, sizeof(sp->src_from));
    track_eval(&sp->source, (float)(sp->pos + sp->block_len), sp->src_to, FS_SP_SOURCE_COUNT);
    if (sp->pos == 0) memcpy(sp->src_from, sp->src_to, sizeof(sp->src_from));
    bandpass_set(&sp->hiss_a, fmaxf(sp->src_to[FS_SP_FA], 100.0f), sp->src_to[FS_SP_BA], rate);
    bandpass_set(&sp->hiss_b, fmaxf(sp->src_to[FS_SP_FB], 100.0f), sp->src_to[FS_SP_BB], rate);
}

int fs_speech_render(FsSpeech *sp, float *out, int n) {
    int done = 0;
    while (done < n && sp->pos < sp->length) {
        if (sp->block_left == 0) update_block(sp);
        const float x = 1.0f - (float)sp->block_left / (float)sp->block_len;
        const float step = 1.0f / (float)sp->block_len;
        float lv[FS_SP_SOURCE_COUNT];
        for (int p = 0; p <= FS_SP_AF; ++p) lv[p] = sp->src_from[p] + (sp->src_to[p] - sp->src_from[p]) * x;
        float dlv[3];
        for (int p = 0; p <= FS_SP_AF; ++p) dlv[p] = (sp->src_to[p] - sp->src_from[p]) * step;
        const float ga = sp->src_to[FS_SP_GA], gb = sp->src_to[FS_SP_GB], gby = sp->src_to[FS_SP_BYPASS];
        const float breath = sp->voice.breathiness;
        const float out_gain = sp->voice.gain;

        while (sp->block_left > 0 && done < n) {
            // Glottal pulse: the derivative of the flow x^2 - x^3 over the
            // open part of the period, then closed. A new period picks up
            // the pitch track, with a little jitter so it is not robotic.
            if (sp->phase >= sp->period) {
                sp->phase -= sp->period;
                float pv[2];
                track_eval(&sp->pitch, (float)sp->pos, pv, 2);
                float f0 = pv[0];
                if (pv[1] > 0) f0 *= powf(2.0f, pv[1] / 12.0f * sinf(2.0f * (float)M_PI * sp->vib_phase));
                if (f0 < 20.0f) f0 = 20.0f;
                sp->period = sp->rate / f0 * (1.0f + 0.006f * noise(&sp->noise));
                // More pulses per second carry more energy: scale each so
                // the loudness stays about the same over the pitch range
                sp->pulse_gain = fs_speech_clamp(sqrtf(110.0f / f0), 0.25f, 1.0f);
            }
            const float open = 0.6f * sp->period;
            float g = 0.0f;
            bool is_open = sp->phase < open;
            if (is_open) {
                const float u = sp->phase / open;
                g = 2.0f * u - 3.0f * u * u;
            }
            sp->phase += 1.0f;
            sp->vib_phase += sp->voice.vibrato_hz / sp->rate;
            if (sp->vib_phase >= 1.0f) sp->vib_phase -= 1.0f;
            // Spectral tilt: soften the closing edge a little
            sp->tilt_state += sp->voice.brightness * (g - sp->tilt_state);
            g = sp->tilt_state;

            const float nz = noise(&sp->noise);
            sp->noise_lp += 0.5f * (nz - sp->noise_lp);
            const float av = lv[FS_SP_AV], ah = lv[FS_SP_AH], af = lv[FS_SP_AF];
            // Breath: aspiration noise, plus breathiness while voicing
            // (stronger while the glottis is open)
            const float asp = sp->noise_lp * (ah + av * breath * (is_open ? 0.25f : 0.08f));
            float x_in = av * g * 30.0f * sp->pulse_gain + asp * 40.0f;

            float y = resonate(&sp->nasal_pole, x_in);
            y = antiresonate(&sp->nasal_zero, y);
            for (int f = 7; f >= 3; --f) y = resonate(&sp->formant[f], y);
            y = resonate(&sp->formant[2], y);
            y = resonate(&sp->formant[1], y);
            y = resonate(&sp->formant[0], y);

            // Hiss: in voiced fricatives the airflow, and so the hiss,
            // pulses with the voice
            // pulses with the voice. The flat (bypass) part is
            // differenced (+6 dB per octave), as hiss has little energy at
            // low frequencies; the band passes need no help with that, and
            // differencing their input would flatten their upper skirts.
            const float h = nz * af * (av > 0.05f ? (is_open ? 1.0f : 0.4f) : 1.0f);
            const float hd = 0.5f * (h - sp->hiss_prev);
            sp->hiss_prev = h;
            const float hiss = ga * bandpass(&sp->hiss_a, h) + gb * bandpass(&sp->hiss_b, h) + gby * hd;

            const float s = y * CASCADE_GAIN + hiss * HISS_GAIN;
            // Remove DC (below about 50 Hz)
            const float hp = s - sp->hp_x1 + 0.993f * sp->hp_y1;
            sp->hp_x1 = s;
            sp->hp_y1 = hp;
            out[done++] = hp * out_gain;

            for (int p = 0; p <= FS_SP_AF; ++p) lv[p] += dlv[p];
            --sp->block_left;
            ++sp->pos;
        }
    }
    return done;
}

bool fs_speech_busy(const FsSpeech *sp) {
    return sp->pos < sp->length;
}

uint32_t fs_speech_length(const FsSpeech *sp) {
    return sp->length;
}
