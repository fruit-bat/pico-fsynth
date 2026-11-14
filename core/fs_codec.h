#pragma once
//
// fs_codec.h - lossless compression of font sample data (FS_DATA_RICE).
//
// The sample data is a sequence of blocks of 1..FS_CODEC_BLOCK frames,
// each starting on a 4-byte boundary and decodable on its own (no state
// carries over from the previous block), so decoding can start at any
// block. The encoder starts a new block at every position the synth jumps
// to: each region's start and, for looping regions, its loop start. Those
// positions are listed with their byte offsets in the font's seek table
// (FsSeekPoint), so a jump never decodes frames it does not use.
//
// The data is read as little-endian 32-bit words, and bits are taken from
// each word least significant first, so decoding needs only aligned word
// loads and no byte swapping.
//
// A block is a header word, then one code per remaining frame, then zero
// bits up to the next word:
//   header bits 0-7     frames - 1                  (1..128 frames)
//               8-12    Rice: parameter k (0..15); packed: width w (0..20)
//               13-14   predictor order p           (0..3)
//               15      mode: 0 Rice, 1 packed
//               16-31   frame 0, two's complement
//
// Frame i (i >= 1) is predicted from the previous frames of the block by
// the fixed polynomial predictor of order min(p, i), as in FLAC:
//   order 0: 0
//   order 1: x[i-1]
//   order 2: 2 x[i-1] - x[i-2]
//   order 3: 3 x[i-1] - 3 x[i-2] + x[i-3]
// and the residual x[i] - prediction is coded as u = zigzag(residual)
// (0, -1, 1, -2, 2 ... -> 0, 1, 2, 3, 4 ...), either
//   Rice (smaller):  u >> k zero bits, a one bit, then the low k bits of u
//   packed (faster): u in exactly w bits
// with all bit fields least significant bit first. Packed decoding has no
// data dependent branches; Rice needs a count of the zero bits.
//
// Decoding runs where the streamer used to copy samples (fs_stream_fill).
//
#include <stdint.h>
#include <stddef.h>
#include "fs_config.h"

#define FS_CODEC_BLOCK 128   // Most frames in a block

// Decoder state: a position in the bit stream plus the current block.
typedef struct {
    const uint32_t *words;  // Compressed sample data, 4-byte aligned
    uint32_t count;         // Its size in words (reads beyond give all ones)
    uint32_t pos;           // Next word to load
    uint32_t buf;           // Unread bits, next bit in bit 0; zero above them
    uint32_t bits;          // Number of unread bits in buf
    uint32_t left;          // Frames still to decode in the current block
    uint32_t have;          // Frames of this block decoded so far, capped at 3
    uint32_t order;         // Predictor order of the current block
    uint32_t k;             // Rice parameter, or packed width, of the current block
    uint32_t packed;        // Current block is packed (else Rice)
    int32_t h1, h2, h3;     // The last three decoded frames
} FsDecoder;

// Start decoding at the block at byte offset (a multiple of 4) of data,
// which holds size bytes and must be 4-byte aligned.
void fs_decoder_seek(FsDecoder *d, const uint8_t *data, uint32_t size, uint32_t offset);

// Decode the next n frames into out, crossing blocks as needed.
void fs_decoder_read(FsDecoder *d, int16_t *out, uint32_t n);

// Encoder block modes
typedef enum {
    FS_CODEC_FAST = 0,      // Packed blocks only
    FS_CODEC_SMALL,         // Rice blocks only
} FsCodecMode;

// Host side (fs_codec_enc.c): encode pcm[0, frames) with a block starting
// at every one of the cuts (sorted ascending, no duplicates, each < frames;
// 0 is always a block start). Returns the compressed data (malloc'd, *size
// bytes, a multiple of 4) and writes each cut's byte offset to
// cut_offsets[]. NULL if out of memory.
uint8_t *fs_encode(const int16_t *pcm, uint32_t frames, const uint32_t *cuts, uint32_t cut_count,
                   uint32_t *cut_offsets, size_t *size, FsCodecMode mode);
