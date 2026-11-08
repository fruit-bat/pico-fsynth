#pragma once
#include <stdint.h>
#include <stddef.h>

// Standard CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320).
// Start with crc = 0 and feed blocks in order.
uint32_t fs_crc32(uint32_t crc, const void *data, size_t len);
