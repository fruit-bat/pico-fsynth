#pragma once
//
// fs_crc32.h - CRC-32 checksum used to validate compiled fonts and the
// saved settings record.
//
// sf2conv writes CRCs into the .fsf header, fs_font_init checks them, and
// the firmware protects its settings sector with the same function.
//
#include <stdint.h>
#include <stddef.h>

// Standard CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320), the same value
// as zlib's crc32() and `crc32` on the command line.
//
// crc   0 for the first block; to checksum data in pieces, pass the value
//       returned for the previous piece.
// data  bytes to add; len may be 0.
// Returns the CRC of everything fed so far.
uint32_t fs_crc32(uint32_t crc, const void *data, size_t len);
