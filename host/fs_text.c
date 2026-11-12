//
// fs_text.c - English text to phonemes (see fs_text.h).
//
// The dictionary is held in an open addressing hash table of the words,
// pointing into one block holding the file's text.
//
#include "fs_text.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct FsTextDict {
    char *text;                 // The file, with words and pronunciations NUL terminated
    const char **word;          // Hash table: word, or NULL for an empty slot
    const char **pron;          // and its pronunciation
    size_t size;                // Slots (a power of 2)
};

static uint32_t hash(const char *s) {
    uint32_t h = 2166136261u;   // FNV-1a
    while (*s) h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

FsTextDict *fs_text_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    rewind(f);
    FsTextDict *d = calloc(1, sizeof(*d));
    d->text = malloc((size_t)n + 1);
    if (n < 0 || fread(d->text, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        fs_text_free(d);
        return NULL;
    }
    fclose(f);
    d->text[n] = 0;

    size_t lines = 1;
    for (long i = 0; i < n; ++i) lines += d->text[i] == '\n';
    d->size = 1;
    while (d->size < lines * 2) d->size <<= 1;
    d->word = calloc(d->size, sizeof(*d->word));
    d->pron = calloc(d->size, sizeof(*d->pron));

    char *p = d->text;
    while (*p) {
        char *eol = strchr(p, '\n');
        if (eol) *eol = 0;
        // Cut off comments and trailing white space
        char *c = strchr(p, '#');
        if (c) *c = 0;
        if (!(p[0] == ';' && p[1] == ';')) {
            char *w = p;
            while (*w && isspace((unsigned char)*w)) ++w;
            char *we = w;
            while (*we && !isspace((unsigned char)*we)) ++we;
            if (we > w && *we) {
                *we = 0;
                char *pr = we + 1;
                while (*pr && isspace((unsigned char)*pr)) ++pr;
                char *pe = pr + strlen(pr);
                while (pe > pr && isspace((unsigned char)pe[-1])) *--pe = 0;
                // Old cmudict files are upper case; keep the first
                // pronunciation and skip "word(2)" alternatives
                for (char *q = w; *q; ++q) *q = (char)tolower((unsigned char)*q);
                if (!strchr(w, '(') && *pr) {
                    size_t h = hash(w) & (d->size - 1);
                    while (d->word[h] && strcmp(d->word[h], w) != 0) h = (h + 1) & (d->size - 1);
                    if (!d->word[h]) {
                        d->word[h] = w;
                        d->pron[h] = pr;
                    }
                }
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
    return d;
}

void fs_text_free(FsTextDict *d) {
    if (!d) return;
    free(d->text);
    free(d->word);
    free(d->pron);
    free(d);
}

const char *fs_text_lookup(const FsTextDict *d, const char *word) {
    size_t h = hash(word) & (d->size - 1);
    while (d->word[h]) {
        if (strcmp(d->word[h], word) == 0) return d->pron[h];
        h = (h + 1) & (d->size - 1);
    }
    return NULL;
}

// --- Output buffer -----------------------------------------------------------

typedef struct {
    char *p;
    size_t size, len;
    bool full;
} Out;

static void put(Out *o, const char *s) {
    const size_t n = strlen(s);
    if (o->full || o->len + n + 2 > o->size) {
        o->full = true;
        return;
    }
    if (o->len) o->p[o->len++] = ' ';
    memcpy(o->p + o->len, s, n);
    o->len += n;
    o->p[o->len] = 0;
}

// --- Words -----------------------------------------------------------------------

// Letter names, for spelling out unknown words
static const char *const letters[26] = {
    "EY1", "B IY1", "S IY1", "D IY1", "IY1", "EH1 F", "JH IY1", "EY1 CH", "AY1",
    "JH EY1", "K EY1", "EH1 L", "EH1 M", "EH1 N", "OW1", "P IY1", "K Y UW1",
    "AA1 R", "EH1 S", "T IY1", "Y UW1", "V IY1", "D AH1 B AH0 L Y UW0", "EH1 K S",
    "W AY1", "Z IY1",
};

// Words that are normally unstressed in a sentence
static const char *const function_words[] = {
    "a", "an", "the", "of", "to", "in", "on", "at", "by", "for", "from", "with",
    "and", "or", "but", "as", "is", "was", "are", "were", "be", "been", "am",
    "it", "its", "i", "he", "she", "we", "they", "you", "me", "him", "her", "us",
    "them", "my", "his", "our", "your", "their", "that", "than", "then", "has",
    "have", "had", "can", "could", "will", "would", "shall", "should", "do",
    "does", "so", "if", "into", "onto", "there",
};

static bool is_function_word(const char *w) {
    for (size_t i = 0; i < sizeof(function_words) / sizeof(function_words[0]); ++i) {
        if (strcmp(w, function_words[i]) == 0) return true;
    }
    return false;
}

// Append a pronunciation, optionally with the stress digits set to 0
static void put_pron(Out *o, const char *pron, bool destress) {
    char buf[256];
    size_t n = strlen(pron);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, pron, n);
    buf[n] = 0;
    if (destress) {
        for (char *c = buf; *c; ++c) if (*c == '1' || *c == '2') *c = '0';
    }
    put(o, buf);
}

// Say one word (lower case letters and apostrophes). Returns 1 if it had
// to be spelled out.
static int say_word(const FsTextDict *d, Out *o, const char *w, bool in_sentence) {
    const char *pr = fs_text_lookup(d, w);
    if (pr) {
        put_pron(o, pr, in_sentence && is_function_word(w));
        return 0;
    }
    for (const char *c = w; *c; ++c) {
        if (*c >= 'a' && *c <= 'z') put(o, letters[*c - 'a']);
    }
    return 1;
}

// --- Numbers ---------------------------------------------------------------------

static const char *const ones[20] = {
    "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine",
    "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen",
    "seventeen", "eighteen", "nineteen",
};
static const char *const tens[10] = {
    "", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety",
};

// Say 1..999
static void say_hundreds(const FsTextDict *d, Out *o, unsigned v) {
    if (v >= 100) {
        say_word(d, o, ones[v / 100], false);
        say_word(d, o, "hundred", false);
        v %= 100;
        if (v) say_word(d, o, "and", true);
    }
    if (v >= 20) {
        say_word(d, o, tens[v / 10], false);
        if (v % 10) say_word(d, o, ones[v % 10], false);
    } else if (v) {
        say_word(d, o, ones[v], false);
    }
}

// Say a number of up to 9 digits; longer ones digit by digit
static void say_number(const FsTextDict *d, Out *o, const char *digits) {
    const size_t n = strlen(digits);
    if (n > 9) {
        for (const char *c = digits; *c; ++c) say_word(d, o, ones[*c - '0'], false);
        return;
    }
    const unsigned long v = strtoul(digits, NULL, 10);
    if (v == 0) {
        say_word(d, o, "zero", false);
        return;
    }
    if (v >= 1000000) {
        say_hundreds(d, o, (unsigned)(v / 1000000));
        say_word(d, o, "million", false);
    }
    if (v / 1000 % 1000) {
        say_hundreds(d, o, (unsigned)(v / 1000 % 1000));
        say_word(d, o, "thousand", false);
    }
    say_hundreds(d, o, (unsigned)(v % 1000));
}

int fs_text_to_phonemes(const FsTextDict *d, const char *text, char *out, size_t out_size) {
    Out o = { out, out_size, 0, false };
    if (out_size) out[0] = 0;
    int spelled = 0;
    // Count the words first: a single word is said with its full stress
    int words = 0;
    for (const char *s = text; *s;) {
        if (isalnum((unsigned char)*s)) {
            ++words;
            while (isalnum((unsigned char)*s) || *s == '\'') ++s;
        } else {
            ++s;
        }
    }
    const char *s = text;
    while (*s) {
        const unsigned char c = (unsigned char)*s;
        if (isalpha(c)) {
            char w[64];
            size_t n = 0;
            while ((isalpha((unsigned char)*s) || *s == '\'') && n + 1 < sizeof(w)) {
                w[n++] = (char)tolower((unsigned char)*s++);
            }
            while (isalpha((unsigned char)*s) || *s == '\'') ++s;     // Overlong: cut
            while (n && w[n - 1] == '\'') --n;                          // Quote marks
            w[n] = 0;
            if (n) spelled += say_word(d, &o, w, words > 1);
        } else if (isdigit(c)) {
            char num[32];
            size_t n = 0;
            while ((isdigit((unsigned char)*s) || (*s == ',' && isdigit((unsigned char)s[1]))) && n + 1 < sizeof(num)) {
                if (*s != ',') num[n++] = *s;
                ++s;
            }
            while (isdigit((unsigned char)*s)) ++s;
            num[n] = 0;
            say_number(d, &o, num);
        } else if (c == '.' || c == '!' || c == '?') {
            const char p[2] = { (char)c, 0 };
            while (*s == '.' || *s == '!' || *s == '?') ++s;
            put(&o, p);
        } else if (c == ',' || c == ';' || c == ':' || (c == '-' && s[1] == ' ')) {
            put(&o, ",");
            ++s;
        } else {
            ++s;
        }
    }
    return o.full ? -1 : spelled;
}
