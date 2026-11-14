//
// fs_codec_enc.c - encoder for compressed font sample data (host only:
// sf2conv and the tests). See fs_codec.h for the format.
//
// Blocks start on 32-bit word boundaries. Each block is cut at the next seek point or after FS_CODEC_BLOCK frames,
// whichever comes first. For each block every predictor order (0..3) is
// tried, with every Rice parameter (0..15) or the packed width it needs,
// and the smallest is written.
//
#include <stdlib.h>
#include <string.h>
#include "fs_codec.h"

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;         // Whole bytes written
    uint32_t acc;       // Pending bits, first in bit 0
    uint32_t n;         // Number of pending bits (0..7 between calls)
    int failed;
} BitWriter;

static void put_byte(BitWriter *w, uint8_t b) {
    if (w->len == w->cap) {
        const size_t cap = w->cap ? w->cap * 2 : 1 << 16;
        uint8_t *p = realloc(w->buf, cap);
        if (!p) { w->failed = 1; return; }
        w->buf = p;
        w->cap = cap;
    }
    w->buf[w->len++] = b;
}

// Write the low `bits` (0..24) bits of v, least significant first
static void put_bits(BitWriter *w, uint32_t v, uint32_t bits) {
    if (!bits) return;
    w->acc |= (v & ((1u << bits) - 1)) << w->n;
    w->n += bits;
    while (w->n >= 8) {
        put_byte(w, (uint8_t)w->acc);
        w->acc >>= 8;
        w->n -= 8;
    }
}

// Pad with zero bits to the next 32-bit word
static void align(BitWriter *w) {
    if (w->n) put_bits(w, 0, 8 - w->n);
    while (w->len & 3) put_byte(w, 0);
}

static uint32_t zigzag(int32_t r) {
    return r >= 0 ? (uint32_t)r * 2 : (uint32_t)(-(int64_t)r) * 2 - 1;
}

// Residual of frame i of the block x[0..] for predictor order p
static int32_t residual(const int16_t *x, uint32_t i, uint32_t p) {
    const uint32_t o = i < p ? i : p;
    switch (o) {
    case 0: return x[i];
    case 1: return x[i] - x[i - 1];
    case 2: return x[i] - 2 * x[i - 1] + x[i - 2];
    default: return x[i] - 3 * x[i - 1] + 3 * x[i - 2] - x[i - 3];
    }
}

// Bits needed for u (0 for 0)
static uint32_t bit_width(uint32_t u) {
    uint32_t n = 0;
    while (u) { ++n; u >>= 1; }
    return n;
}

static void encode_block(BitWriter *w, const int16_t *x, uint32_t len, FsCodecMode mode) {
    uint32_t best_p = 0, best_k = 0;
    uint64_t best = UINT64_MAX;
    for (uint32_t p = 0; p <= 3; ++p) {
        uint32_t u[FS_CODEC_BLOCK];
        for (uint32_t i = 1; i < len; ++i) u[i] = zigzag(residual(x, i, p));
        if (mode == FS_CODEC_FAST) {
            uint32_t width = 0;
            for (uint32_t i = 1; i < len; ++i) { const uint32_t b = bit_width(u[i]); if (b > width) width = b; }
            const uint64_t bits = (uint64_t)width * (len - 1);
            if (bits < best) { best = bits; best_p = p; best_k = width; }
        } else {
            for (uint32_t k = 0; k <= 15; ++k) {
                uint64_t bits = 0;
                for (uint32_t i = 1; i < len; ++i) bits += (u[i] >> k) + 1 + k;
                if (bits < best) { best = bits; best_p = p; best_k = k; }
            }
        }
    }
    const uint32_t is_packed = mode == FS_CODEC_FAST;
    // Header word: frames - 1, k or width, order, mode, frame 0
    put_bits(w, (len - 1) | (best_k << 8) | (best_p << 13) | (is_packed << 15), 16);
    put_bits(w, (uint16_t)x[0], 16);
    for (uint32_t i = 1; i < len; ++i) {
        const uint32_t u = zigzag(residual(x, i, best_p));
        if (is_packed) {
            put_bits(w, u, best_k);
        } else {
            uint32_t q = u >> best_k;
            while (q >= 24) { put_bits(w, 0, 24); q -= 24; }
            put_bits(w, 1u << q, q + 1);           // q zeros then a one
            put_bits(w, u, best_k);
        }
    }
    align(w);
}

// See fs_codec.h.
uint8_t *fs_encode(const int16_t *pcm, uint32_t frames, const uint32_t *cuts, uint32_t cut_count,
                   uint32_t *cut_offsets, size_t *size, FsCodecMode mode) {
    BitWriter w = { 0 };
    uint32_t c = 0;         // Next cut
    uint32_t pos = 0;
    while (pos < frames) {
        while (c < cut_count && cuts[c] < pos) ++c;   // Cuts already passed (never: each starts a block)
        if (c < cut_count && cuts[c] == pos) cut_offsets[c++] = (uint32_t)w.len;
        uint32_t len = frames - pos;
        if (len > FS_CODEC_BLOCK) len = FS_CODEC_BLOCK;
        if (c < cut_count && cuts[c] - pos < len) len = cuts[c] - pos;
        encode_block(&w, pcm + pos, len, mode);
        pos += len;
    }
    if (w.failed) { free(w.buf); return NULL; }
    *size = w.len;
    return w.buf ? w.buf : malloc(1);
}
