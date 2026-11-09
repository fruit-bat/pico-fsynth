//
// fs_crc32.c - table driven CRC-32, processed 4 bits at a time.
//
// A 16-entry table (64 bytes) instead of the usual 256-entry one (1 KB).
// It does two table steps per byte instead of one, which is fine: the CRC
// only runs at start-up, in the host tools and when settings are saved.
//
#include "fs_crc32.h"

// CRC of each 4-bit value, for the reflected polynomial 0xEDB88320.
// Small enough to keep in SRAM on the Pico.
static const uint32_t fs_crc32_table[16] = {
    0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
    0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
    0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
    0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
};

// See fs_crc32.h. The running value is kept inverted between calls, as the
// standard requires, so chained calls give the same result as one call.
uint32_t fs_crc32(uint32_t crc, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ fs_crc32_table[crc & 15];  // Low nibble
        crc = (crc >> 4) ^ fs_crc32_table[crc & 15];  // High nibble
    }
    return ~crc;
}
