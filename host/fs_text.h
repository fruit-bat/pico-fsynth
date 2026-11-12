#pragma once
//
// fs_text.h - English text to phonemes for the speech synthesizer, on the
// PC, using the CMU Pronouncing Dictionary.
//
// The dictionary is not part of this project; host/get_cmudict.sh
// downloads it (BSD licence, about 3.5 MB, 135,000 words).
//
//   FsTextDict *d = fs_text_load("build-host/cmudict.dict");
//   char ph[4096];
//   fs_text_to_phonemes(d, "Hello, world.", ph, sizeof(ph));
//   // ph = "HH AH0 L OW1 , W ER1 L D ."
//
// What it does with the text:
//   - words are looked up in the dictionary (first pronunciation);
//   - numbers are read out ("42" -> forty two, up to 999,999,999);
//   - words not in the dictionary are spelled out letter by letter;
//   - short function words (the, of, to, and, is...) lose their stress,
//     as they do in normal speech, so they do not all get a pitch accent;
//   - , ; : and - between words make a short pause, . ! ? end a phrase.
//
#include <stddef.h>

typedef struct FsTextDict FsTextDict;

// Load a dictionary file in the cmudict format ("word PH PH ..." per line,
// alternative pronunciations as "word(2)", ";;;" or "#" comments).
// Returns NULL if the file cannot be read.
FsTextDict *fs_text_load(const char *path);

void fs_text_free(FsTextDict *d);

// The dictionary pronunciation of one lower case word, or NULL
const char *fs_text_lookup(const FsTextDict *d, const char *word);

// Convert text to a phoneme string for fs_speech_say. Returns the number
// of words that had to be spelled out, or -1 if out was too small (out is
// then cut at a word boundary, so it can still be spoken).
int fs_text_to_phonemes(const FsTextDict *d, const char *text, char *out, size_t out_size);
