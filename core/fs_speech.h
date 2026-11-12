#pragma once
//
// fs_speech.h - a formant speech synthesizer: speaks a string of English
// phonemes.
//
// This is a "Klatt style" synthesizer, the kind of design used by DECtalk,
// rather than a player of recorded sounds like the old SP0256 chip. Speech
// is made the way the throat makes it:
//
//   - a source: a buzz of glottal pulses for voiced sounds (vowels, m, z...)
//     and noise for breath (h) and hiss (s, sh, f...),
//   - filters: resonators that put peaks (formants) into the spectrum at
//     the frequencies the shape of the mouth gives. The first three formant
//     frequencies are what tell one vowel from another.
//
// Every phoneme is a set of target values for the formants, the source
// levels and a duration. The synthesizer glides between targets the way a
// mouth moves (the glides are most of what makes stops like b / d / g
// recognisable), and the pitch follows an intonation contour of its own,
// so the voice can speak or sing at any pitch without the chipmunk effect
// of resampled recordings.
//
// Input is ARPAbet, the phoneme set of the CMU pronouncing dictionary,
// separated by spaces (case does not matter):
//
//   "HH AH0 L OW1 , W ER1 L D ."        hello, world.
//
//   Vowels   AA (father) AE (cat) AH (cut) AO (caught) AW (cow) AX (about)
//            AY (bite) EH (bet) ER (bird) EY (bait) IH (bit) IY (beet)
//            OW (boat) OY (boy) UH (book) UW (boot)
//            A digit after a vowel gives its stress: 1 primary, 2 secondary,
//            0 unstressed (shorter, quieter, no pitch accent). No digit
//            means stressed.
//   Others   B CH D DH (this) F G HH JH (judge) K L M N NG (sing) P R S
//            SH T TH (thin) V W Y Z ZH (vision)
//   Pauses   ,  short pause, pitch held     .  !  end of a statement (falls)
//            ?  question (rises)           _  short silence
//
// A separate word marker is not needed. The intonation puts a pitch accent
// on each stressed vowel and falls (or rises for "?") at the end of each
// phrase.
//
// Singing: a vowel may carry a MIDI note and a length in ms, and a "_"
// rest a length:
//
//   "T W IH1@67:400 NG K AH0 L@67:400 _:200 S T AA1@74:800 R"
//
// A note's length covers its vowel and the consonants after it (up to the
// next vowel), so the vowels start on the beat. A vowel without a note
// keeps the previous one. When anything has a note or length the whole
// string is sung: the pitch follows the notes, gliding briefly between
// them, with vibrato, and the speech intonation is off.
//
// Usage:
//
//   FsSpeech sp;
//   fs_speech_init(&sp, 44100.0f);
//   if (fs_speech_say(&sp, "HH AH0 L OW1 .") != 0) ... bad phoneme
//   while (fs_speech_render(&sp, buf, n) > 0) ... write n mono samples
//
// The whole utterance is laid out by fs_speech_say (timing, formant
// tracks, pitch contour), after which rendering just reads the tracks, so
// fs_speech_render can be called with any buffer size. Nothing is
// allocated. FsSpeech holds the tracks and is large: about 135 KB at the
// default FS_SPEECH_MAX_PHONEMES of 128, plus 41 KB of static scratch
// space for the layout. That is fine on a PC; for the Pico the tracks
// will need slimming (smaller keys per track, or laying out a phrase at a
// time).
//
#include <stdint.h>
#include <stdbool.h>

// Most phonemes (including pauses) in one fs_speech_say call
#ifndef FS_SPEECH_MAX_PHONEMES
#define FS_SPEECH_MAX_PHONEMES 128
#endif

// Keyframes per track: a stop makes 3 source segments of 2 keys each
#define FS_SPEECH_MAX_KEYS (FS_SPEECH_MAX_PHONEMES * 8 + 16)

// Frames between updates of the filter coefficients
#define FS_SPEECH_BLOCK 32

// Values of the formant (vocal tract) track
enum {
    FS_SP_F1, FS_SP_F2, FS_SP_F3,   // Formant frequencies, Hz
    FS_SP_B1, FS_SP_B2, FS_SP_B3,   // Their bandwidths, Hz
    FS_SP_FNZ,                      // Nasal zero, Hz (= nasal pole when not nasal)
    FS_SP_TRACT_COUNT
};

// Values of the source track: levels are linear amplitudes, 1 = a vowel
enum {
    FS_SP_AV,                       // Voicing (glottal pulses)
    FS_SP_AH,                       // Aspiration: breath noise through the formants
    FS_SP_AF,                       // Frication: noise through the hiss filters
    FS_SP_FA, FS_SP_BA, FS_SP_GA,   // Hiss resonator A: frequency, bandwidth, gain
    FS_SP_FB, FS_SP_BB, FS_SP_GB,   // Hiss resonator B
    FS_SP_BYPASS,                   // Unfiltered (flat) noise gain
    FS_SP_SOURCE_COUNT
};

// One point of a piecewise linear track: time in samples and values
typedef struct {
    float t;
    float v[FS_SP_SOURCE_COUNT];    // The larger of the two counts
} FsSpeechKey;

// A track: keyframes in time order, with a read cursor
typedef struct {
    FsSpeechKey key[FS_SPEECH_MAX_KEYS];
    int count;
    int cursor;                     // Index of the key at or before the read time
} FsSpeechTrack;

// A two pole resonator (or, used inverted, an anti-resonator)
typedef struct {
    float a, b, c;                  // y = a x + b y1 + c y2
    float y1, y2;
} FsSpeechResonator;

// A band pass filter (two poles, zeros at DC and Nyquist), for the hiss
typedef struct {
    float b0, a1, a2;               // y = b0 (x - x2) - a1 y1 - a2 y2
    float x1, x2, y1, y2;
} FsSpeechBandpass;

// Voice settings; change between fs_speech_init and fs_speech_say
typedef struct {
    float pitch_hz;                 // Base pitch (80 = a deep male voice, the clearest)
    float speed;                    // Speaking rate: 1 normal; 2 halves the stretchable
                                    // part of each sound, its minimum by 1/sqrt(2)
    float formant_scale;            // 1 = adult male; about 1.15 sounds female
    float accent;                   // Pitch accent on stressed vowels: 0.2 = +20%
    float breathiness;              // Breath noise mixed into voicing: 0..1
    float brightness;               // Voice source brightness: 0.1 dull .. 1 bright
    float gain;                     // Output level (0.85 leaves headroom for loud consonants)
    float vibrato;                  // Singing: vibrato depth, semitones
    float vibrato_hz;               // Singing: vibrato rate
} FsSpeechVoice;

typedef struct {
    float rate;                     // Sample rate, Hz
    FsSpeechVoice voice;

    // Tracks built by fs_speech_say
    FsSpeechTrack tract, source, pitch;
    uint32_t length;                // Total length in samples
    uint32_t pos;                   // Samples rendered so far

    // Filter state
    FsSpeechResonator nasal_pole, nasal_zero, formant[8];
    FsSpeechBandpass hiss_a, hiss_b;
    float hiss_prev;                // Last noise sample (the hiss is its difference)
    float tract_now[FS_SP_TRACT_COUNT];   // Smoothed formant values
    bool tract_valid;
    float src_from[FS_SP_SOURCE_COUNT];   // Source values at the start / end
    float src_to[FS_SP_SOURCE_COUNT];     // of the current block
    uint32_t block_left;            // Samples until the next coefficient update
    uint32_t block_len;

    // Glottal source
    float period;                   // Current pitch period, samples
    float phase;                    // Samples into the current period
    float vib_phase;                // Vibrato, 0..1
    float pulse_gain;               // Level of the pulses at this pitch
    float tilt_state;               // Spectral tilt low pass
    uint32_t noise;                 // Noise generator state
    float noise_lp;                 // Low passed noise for aspiration
    float hp_x1, hp_y1;             // DC blocking high pass on the output
} FsSpeech;

// Set the sample rate and default voice; silent until fs_speech_say.
void fs_speech_init(FsSpeech *sp, float sample_rate);

// Lay out a phoneme string (see the file header) and start speaking it,
// replacing anything still being said. Returns 0, or the 1-based position
// of the first bad token in the string (nothing is said then).
int fs_speech_say(FsSpeech *sp, const char *phonemes);

// Render up to n mono samples (about -1..1) into out. Returns the number
// rendered: less than n at the end of the utterance, 0 once it is over.
int fs_speech_render(FsSpeech *sp, float *out, int n);

// True while there is something left to say
bool fs_speech_busy(const FsSpeech *sp);

// Samples in the utterance laid out by the last fs_speech_say
uint32_t fs_speech_length(const FsSpeech *sp);
