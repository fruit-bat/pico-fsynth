//
// fs_codec.c - decoder for compressed font sample data. See fs_codec.h for
// the format. Runs on the render path (fs_stream_fill), so it is placed in
// RAM on the devices.
//
// The cost per frame is one Rice code: find the first one bit (the unary
// part), then take k bits. Finding the one bit is a count of trailing
// zeros: one instruction on the Cortex-M33 (RBIT + CLZ) and on RISC-V with
// the Zbb extension, and a 256-entry table otherwise (the ESP32-P4's cores
// have no Zbb). Words are loaded only when the buffer runs out, about once
// every three frames.
//
// The decoder works on a local copy of its state so the compiler can keep
// it in registers, and writes it back at the end.
//
#include "fs_codec.h"

#if defined(__ARM_FEATURE_CLZ) || defined(__riscv_zbb) || defined(__x86_64__) || defined(__i386__)
#define FS_HAVE_CTZ 1
#else
#define FS_HAVE_CTZ 0
// Trailing zeros of a byte (8 for 0)
static const uint8_t ctz8[256] = {
    8,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    5,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    6,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    5,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    7,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    5,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    6,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    5,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
};
#endif

// Trailing zeros of x, which is not 0
static inline uint32_t ctz32(uint32_t x) {
#if FS_HAVE_CTZ
    return (uint32_t)__builtin_ctz(x);
#else
    uint32_t n = 0;
    while (!(x & 0xff)) { x >>= 8; n += 8; }
    return n + ctz8[x & 0xff];
#endif
}

// The next word. Past the end of the data: all ones, so corrupt data
// decodes to noise but cannot read outside the font or spin in the unary
// loop.
static inline uint32_t load(FsDecoder *d) {
    return d->pos < d->count ? d->words[d->pos++] : 0xffffffffu;
}

// One Rice coded residual, zigzag decoded.
static inline int32_t rice(FsDecoder *d) {
    // Unary part: count zeros up to the first one. Bits above `bits` in buf
    // are zero, so a set bit is always a real one.
    uint32_t q = 0;
    while (d->buf == 0) {
        q += d->bits;
        d->buf = load(d);
        d->bits = 32;
    }
    const uint32_t t = ctz32(d->buf);
    q += t;
    // Drop the zeros and the one (t + 1 may be 32)
    d->buf = t == 31 ? 0 : d->buf >> (t + 1);
    d->bits -= t + 1;
    // Low k bits
    const uint32_t k = d->k;
    uint32_t low;
    if (d->bits >= k) {
        low = d->buf & ((1u << k) - 1);
        d->buf >>= k;
        d->bits -= k;
    } else {
        // Split across words (k <= 15, bits < k)
        const uint32_t w = load(d);
        low = (d->buf | (w << d->bits)) & ((1u << k) - 1);
        const uint32_t used = k - d->bits;
        d->buf = w >> used;
        d->bits = 32 - used;
    }
    const uint32_t u = (q << k) | low;
    return (int32_t)(u >> 1) ^ -(int32_t)(u & 1);
}

// One packed residual of d->k bits (0..20), zigzag decoded.
static inline int32_t packed(FsDecoder *d) {
    const uint32_t w = d->k;
    uint32_t u;
    if (d->bits >= w) {
        u = d->buf & ((1u << w) - 1);
        d->buf >>= w;
        d->bits -= w;
    } else {
        // Split across words (bits < w <= 20)
        const uint32_t x = load(d);
        u = (d->buf | (x << d->bits)) & ((1u << w) - 1);
        const uint32_t used = w - d->bits;
        d->buf = x >> used;
        d->bits = 32 - used;
    }
    return (int32_t)(u >> 1) ^ -(int32_t)(u & 1);
}

// Start the next block, which begins at the next word: read its header and
// return its first frame. The rest of the current word is padding.
static inline int16_t block_start(FsDecoder *d) {
    const uint32_t h = load(d);
    d->buf = 0;
    d->bits = 0;
    d->left = h & 0xff;               // Frames after frame 0 (frames - 1)
    d->k = (h >> 8) & 31;
    d->order = (h >> 13) & 3;
    d->packed = (h >> 15) & 1;
    if (!d->packed && d->k > 15) d->k = 15;    // Corrupt data: keep shifts in range
    if (d->packed && d->k > 20) d->k = 20;
    const int16_t x0 = (int16_t)(h >> 16);
    d->h1 = x0;
    d->h2 = d->h3 = 0;
    d->have = 1;
    return x0;
}

// See fs_codec.h.
void FS_RAM_FUNC(fs_decoder_seek)(FsDecoder *d, const uint8_t *data, uint32_t size, uint32_t offset) {
    d->words = (const uint32_t *)(const void *)data;
    d->count = size / 4;
    d->pos = offset / 4;
    d->buf = 0;
    d->bits = 0;
    d->left = 0;
}

// prediction + residual as a 16-bit frame. Valid data always fits; the
// unsigned sum and truncation keep corrupt data from overflowing (the
// history then stays within int16 too). Free on both architectures.
#define ADD16(p, r) ((int16_t)(uint16_t)((uint32_t)(p) + (uint32_t)(r)))

// Decode m frames of the current block (past its ramp up) with residual
// reader RES, in a loop specialised for the predictor order.
#define DECODE_RUN(RES)                                                        \
    switch (d.order) {                                                         \
    case 0:                                                                    \
        while (m--) *out++ = (int16_t)RES(&d);                                 \
        break;                                                                 \
    case 1: {                                                                  \
        int32_t h1 = d.h1;                                                     \
        while (m--) { h1 = ADD16(h1, RES(&d)); *out++ = (int16_t)h1; }         \
        d.h1 = h1;                                                             \
        break;                                                                 \
    }                                                                          \
    case 2: {                                                                  \
        int32_t h1 = d.h1, h2 = d.h2;                                          \
        while (m--) {                                                          \
            const int16_t x = ADD16(2 * h1 - h2, RES(&d));                     \
            h2 = h1; h1 = x;                                                   \
            *out++ = x;                                                        \
        }                                                                      \
        d.h1 = h1; d.h2 = h2;                                                  \
        break;                                                                 \
    }                                                                          \
    default: {                                                                 \
        int32_t h1 = d.h1, h2 = d.h2, h3 = d.h3;                               \
        while (m--) {                                                          \
            const int16_t x = ADD16(3 * (h1 - h2) + h3, RES(&d));              \
            h3 = h2; h2 = h1; h1 = x;                                          \
            *out++ = x;                                                        \
        }                                                                      \
        d.h1 = h1; d.h2 = h2; d.h3 = h3;                                       \
        break;                                                                 \
    }                                                                          \
    }

// See fs_codec.h. Each block's first frames, which use a lower predictor
// order, go through the general path; the rest of the block runs in a
// loop specialised for its mode and order.
void FS_RAM_FUNC(fs_decoder_read)(FsDecoder *dp, int16_t *out, uint32_t n) {
    FsDecoder d = *dp;
    while (n) {
        if (d.left == 0) {
            *out++ = block_start(&d);
            --n;
            continue;
        }
        // Ramp up: predictor order limited by the frames decoded so far
        while (n && d.left && d.have < d.order) {
            const int32_t p = d.have == 1 ? d.h1 : 2 * d.h1 - d.h2;   // have == 2 only for order 3
            const int16_t x = ADD16(p, d.packed ? packed(&d) : rice(&d));
            d.h3 = d.h2; d.h2 = d.h1; d.h1 = x;
            *out++ = x;
            ++d.have; --d.left; --n;
        }
        uint32_t m = n < d.left ? n : d.left;
        n -= m;
        d.left -= m;
        if (d.packed) {
            DECODE_RUN(packed)
        } else {
            DECODE_RUN(rice)
        }
    }
    *dp = d;
}
