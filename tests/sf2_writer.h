#pragma once
//
// sf2_writer.h - write minimal SoundFont 2 files for testing sf2conv.
//
// The model is deliberately simple: one sample (a 100-frame-per-cycle sine
// at 44.1 kHz, looped), a list of instruments with one zone each, and a
// list of presets with one zone each pointing at an instrument.
//
//   SwInst insts[] = { { .key_lo = 0, .key_hi = 127 } };
//   SwPreset presets[] = { { "Piano", 0, 0, 0 } };
//   sw_write("x.sf2", presets, 1, insts, 1);
//
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
    const char *name;
    uint16_t bank, program;
    uint16_t inst;              // Index into the instrument list
} SwPreset;

typedef struct {
    uint8_t key_lo, key_hi;
    int16_t exclusive_class;    // 0 = none (generator omitted)
} SwInst;

// Growable byte buffer
typedef struct {
    uint8_t *p;
    size_t n, cap;
} SwBuf;

static inline void sw_put(SwBuf *b, const void *d, size_t n) {
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 256;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static inline void sw_u8(SwBuf *b, uint8_t v) { sw_put(b, &v, 1); }
static inline void sw_u16(SwBuf *b, uint16_t v) { uint8_t t[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; sw_put(b, t, 2); }
static inline void sw_u32(SwBuf *b, uint32_t v) { sw_u16(b, (uint16_t)v); sw_u16(b, (uint16_t)(v >> 16)); }
static inline void sw_name(SwBuf *b, const char *s) {
    char n[20] = { 0 };
    const size_t len = strlen(s);
    memcpy(n, s, len < sizeof(n) ? len : sizeof(n));  // Full-width names have no NUL, as in SF2
    sw_put(b, n, 20);
}

// Append a chunk: id, size, body (padded to even length)
static inline void sw_chunk(SwBuf *b, const char *id, const SwBuf *body) {
    sw_put(b, id, 4);
    sw_u32(b, (uint32_t)body->n);
    sw_put(b, body->p, body->n);
    if (body->n & 1) sw_u8(b, 0);
}

// Append a LIST chunk of the given type made of the chunks in body
static inline void sw_list(SwBuf *b, const char *type, const SwBuf *body) {
    sw_put(b, "LIST", 4);
    sw_u32(b, (uint32_t)(body->n + 4));
    sw_put(b, type, 4);
    sw_put(b, body->p, body->n);
}

#define SW_FRAMES 2000

// Build the whole file in memory; *size receives its length. Free the
// result with free().
static inline uint8_t *sw_build(const SwPreset *presets, int np, const SwInst *insts, int ni, size_t *size) {
    SwBuf info = { 0 }, sdta = { 0 }, pdta = { 0 }, tmp = { 0 };

    // INFO: version and name
    tmp.n = 0; sw_u16(&tmp, 2); sw_u16(&tmp, 1); sw_chunk(&info, "ifil", &tmp);
    tmp.n = 0; sw_put(&tmp, "Test SF2\0\0", 10); sw_chunk(&info, "INAM", &tmp);

    // sdta: the sample, plus the 46 zero frames the spec asks for after it
    tmp.n = 0;
    for (int i = 0; i < SW_FRAMES; ++i) sw_u16(&tmp, (uint16_t)(int16_t)(12000.0 * sin(2.0 * M_PI * i / 100.0)));
    for (int i = 0; i < 46; ++i) sw_u16(&tmp, 0);
    sw_chunk(&sdta, "smpl", &tmp);

    // phdr: one bag per preset, then the terminal record
    tmp.n = 0;
    for (int i = 0; i <= np; ++i) {
        sw_name(&tmp, i < np ? presets[i].name : "EOP");
        sw_u16(&tmp, i < np ? presets[i].program : 0);
        sw_u16(&tmp, i < np ? presets[i].bank : 0);
        sw_u16(&tmp, (uint16_t)i);              // Bag index
        sw_u32(&tmp, 0); sw_u32(&tmp, 0); sw_u32(&tmp, 0);
    }
    sw_chunk(&pdta, "phdr", &tmp);
    // pbag: bag i uses generator i (instrument), no modulators
    tmp.n = 0;
    for (int i = 0; i <= np; ++i) { sw_u16(&tmp, (uint16_t)i); sw_u16(&tmp, 0); }
    sw_chunk(&pdta, "pbag", &tmp);
    tmp.n = 0; for (int i = 0; i < 10; ++i) sw_u8(&tmp, 0);
    sw_chunk(&pdta, "pmod", &tmp);              // Terminal record only
    tmp.n = 0;
    for (int i = 0; i < np; ++i) { sw_u16(&tmp, 41); sw_u16(&tmp, presets[i].inst); }
    sw_u16(&tmp, 0); sw_u16(&tmp, 0);
    sw_chunk(&pdta, "pgen", &tmp);

    // inst / ibag / igen: each instrument zone is keyRange,
    // [exclusiveClass], sampleModes = loop, sampleID = 0
    tmp.n = 0;
    for (int i = 0; i <= ni; ++i) { sw_name(&tmp, i < ni ? "inst" : "EOI"); sw_u16(&tmp, (uint16_t)i); }
    sw_chunk(&pdta, "inst", &tmp);
    SwBuf igen = { 0 }, ibag = { 0 };
    uint16_t gi = 0;
    for (int i = 0; i < ni; ++i) {
        sw_u16(&ibag, gi); sw_u16(&ibag, 0);
        sw_u16(&igen, 43); sw_u8(&igen, insts[i].key_lo); sw_u8(&igen, insts[i].key_hi); ++gi;
        if (insts[i].exclusive_class) { sw_u16(&igen, 57); sw_u16(&igen, (uint16_t)insts[i].exclusive_class); ++gi; }
        sw_u16(&igen, 54); sw_u16(&igen, 1); ++gi;
        sw_u16(&igen, 53); sw_u16(&igen, 0); ++gi;
    }
    sw_u16(&ibag, gi); sw_u16(&ibag, 0);
    sw_u16(&igen, 0); sw_u16(&igen, 0);
    sw_chunk(&pdta, "ibag", &ibag);
    tmp.n = 0; for (int i = 0; i < 10; ++i) sw_u8(&tmp, 0);
    sw_chunk(&pdta, "imod", &tmp);
    sw_chunk(&pdta, "igen", &igen);

    // shdr: the sample, then the terminal record
    tmp.n = 0;
    sw_name(&tmp, "sine");
    sw_u32(&tmp, 0); sw_u32(&tmp, SW_FRAMES); sw_u32(&tmp, 500); sw_u32(&tmp, 1500);
    sw_u32(&tmp, 44100); sw_u8(&tmp, 69); sw_u8(&tmp, 0); sw_u16(&tmp, 0); sw_u16(&tmp, 1);
    sw_name(&tmp, "EOS");
    for (int i = 0; i < 26; ++i) sw_u8(&tmp, 0);
    sw_chunk(&pdta, "shdr", &tmp);

    SwBuf body = { 0 }, file = { 0 };
    sw_put(&body, "sfbk", 4);
    sw_list(&body, "INFO", &info);
    sw_list(&body, "sdta", &sdta);
    sw_list(&body, "pdta", &pdta);
    sw_put(&file, "RIFF", 4);
    sw_u32(&file, (uint32_t)body.n);
    sw_put(&file, body.p, body.n);
    free(info.p); free(sdta.p); free(pdta.p); free(tmp.p); free(igen.p); free(ibag.p); free(body.p);
    *size = file.n;
    return file.p;
}

// Write a file; returns 0 on success
static inline int sw_write_file(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    const size_t w = fwrite(data, 1, size, f);
    fclose(f);
    return w == size ? 0 : -1;
}
