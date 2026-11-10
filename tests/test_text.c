//
// test_text.c - tests of the host text to phoneme converter
// (host/fs_text.c), with a small dictionary written by the test.
//
// Usage (CTest passes nothing): test_text
// Writes tt_dict.dict to the current directory.
//
#include <string.h>
#include "test.h"
#include "fs_text.h"
#include "fs_speech.h"

static const char *dict_text =
    ";;; comment line\n"
    "hello HH AH0 L OW1\n"
    "hello(2) HH EH0 L OW1\n"
    "WORLD W ER1 L D\n"                     // Upper case, as in old cmudict files
    "the DH AH0 # a comment\n"
    "of AH1 V\n"
    "cat K AE1 T\n"
    "it's IH1 T S\n"
    "one W AH1 N\n" "two T UW1\n" "three TH R IY1\n" "four F AO1 R\n" "five F AY1 V\n"
    "twenty T W EH1 N T IY0\n" "forty F AO1 R T IY0\n" "hundred HH AH1 N D R AH0 D\n"
    "thousand TH AW1 Z AH0 N D\n" "and AH0 N D\n" "zero Z IH1 R OW0\n" "million M IH1 L Y AH0 N\n";

static FsTextDict *dict;

static const char *conv(const char *text, int *spelled) {
    static char out[1024];
    *spelled = fs_text_to_phonemes(dict, text, out, sizeof(out));
    return out;
}

#define CHECK_STR(a, b) do {                                                \
        const char *a_ = (a), *b_ = (b);                                    \
        ++test_checks;                                                      \
        if (strcmp(a_, b_) != 0) {                                          \
            ++test_failures;                                                \
            fprintf(stderr, "%s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, a_, b_); \
        }                                                                   \
    } while (0)

TEST(lookup) {
    CHECK_STR(fs_text_lookup(dict, "hello"), "HH AH0 L OW1");      // First pronunciation
    CHECK_STR(fs_text_lookup(dict, "world"), "W ER1 L D");
    CHECK_STR(fs_text_lookup(dict, "the"), "DH AH0");              // Comment removed
    CHECK(fs_text_lookup(dict, "hello(2)") == NULL);
    CHECK(fs_text_lookup(dict, "missing") == NULL);
}

TEST(sentences) {
    int sp;
    CHECK_STR(conv("Hello, world.", &sp), "HH AH0 L OW1 , W ER1 L D .");
    CHECK_EQ(sp, 0);
    // Function words lose their stress in a sentence, but not alone
    CHECK_STR(conv("The cat of it's", &sp), "DH AH0 K AE1 T AH0 V IH1 T S");
    CHECK_STR(conv("of", &sp), "AH1 V");
    CHECK_STR(conv("Hello?!  World...", &sp), "HH AH0 L OW1 ? W ER1 L D .");
    CHECK_STR(conv("\"Hello\" - 'world'; cat: cat", &sp), "HH AH0 L OW1 , W ER1 L D , K AE1 T , K AE1 T");
}

TEST(numbers) {
    int sp;
    CHECK_STR(conv("42", &sp), "F AO1 R T IY0 T UW1");
    CHECK_STR(conv("0", &sp), "Z IH1 R OW0");
    CHECK_STR(conv("105", &sp), "W AH1 N HH AH1 N D R AH0 D AH0 N D F AY1 V");
    CHECK_STR(conv("2,004", &sp), "T UW1 TH AW1 Z AH0 N D F AO1 R");
    CHECK_STR(conv("3000000", &sp), "TH R IY1 M IH1 L Y AH0 N");
    CHECK_EQ(sp, 0);
}

TEST(spelled_out) {
    int sp;
    CHECK_STR(conv("cat zq", &sp), "K AE1 T Z IY1 K Y UW1");
    CHECK_EQ(sp, 1);
}

TEST(output_is_speakable) {
    // Everything the converter makes is accepted by the synthesizer
    static FsSpeech speech;
    fs_speech_init(&speech, 44100);
    int sp;
    CHECK_EQ(fs_speech_say(&speech, conv("Hello, world. The cat of 1,234 xyz? it's 42!", &sp)), 0);
}

TEST(small_buffer) {
    char out[20];
    CHECK_EQ(fs_text_to_phonemes(dict, "hello world", out, sizeof(out)), -1);
    CHECK_STR(out, "HH AH0 L OW1");             // Cut at a word boundary
}

int main(void) {
    FILE *f = fopen("tt_dict.dict", "wb");
    fputs(dict_text, f);
    fclose(f);
    dict = fs_text_load("tt_dict.dict");
    CHECK(dict != NULL);
    CHECK(fs_text_load("no/such/file") == NULL);
    if (!dict) return test_summary();
    RUN(lookup);
    RUN(sentences);
    RUN(numbers);
    RUN(spelled_out);
    RUN(output_is_speakable);
    RUN(small_buffer);
    fs_text_free(dict);
    return test_summary();
}
