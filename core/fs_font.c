//
// fs_font.c - attach to and validate a compiled .fsf font.
//
// Reads version 1 (PCM only) and version 2 (PCM or compressed) files.
//
// The font is used in place (on the Pico, straight from XIP flash), so
// fs_font_init does all the validation up front. After it succeeds every
// offset, count and index in the font is known to be in range, and the
// synth can index the tables without further checks.
//
#include "fs_font.h"
#include "fs_config.h"
#include "fs_crc32.h"

// True if a table of count elements of elem bytes at byte offset fits
// within limit bytes and starts 4-byte aligned. 64-bit arithmetic so a
// hostile header cannot wrap the sum around.
static bool fs_range_ok(uint32_t offset, uint32_t count, uint32_t elem, uint32_t limit) {
    const uint64_t end = (uint64_t)offset + (uint64_t)count * elem;
    return end <= limit && (offset & 3) == 0;
}

// See fs_font.h. Checks run cheapest first: header, table layout, CRCs,
// then every preset and region.
FsFontResult fs_font_init(FsFont *font, const void *blob, size_t size,
                          bool check_table_crc, bool check_data_crc) {
    const uint8_t *base = (const uint8_t *)blob;
    const FsFontHeader *h = (const FsFontHeader *)base;

    if (size && size < FS_FONT_HEADER_V1_SIZE) return FS_FONT_ERR_SIZE;
    if (h->magic != FS_FONT_MAGIC) return FS_FONT_ERR_MAGIC;
    // Version 1: the short header, plain PCM. Version 2: the full header.
    const bool v1 = h->version == 1 && h->header_size == FS_FONT_HEADER_V1_SIZE;
    if (!v1 && !(h->version == FS_FONT_VERSION && h->header_size == sizeof(FsFontHeader))) return FS_FONT_ERR_VERSION;
    if (size && size < h->header_size) return FS_FONT_ERR_SIZE;
    if (size && h->total_size > size) return FS_FONT_ERR_SIZE;

    const uint32_t format = v1 ? FS_DATA_PCM16 : h->data_format;
    const uint32_t data_size = v1 ? h->data_frames * 2u : h->data_size;
    const uint32_t seek_count = v1 ? 0 : h->seek_count;
    const uint32_t seek_offset = v1 ? 0 : h->seek_offset;
    if (format != FS_DATA_PCM16 && format != FS_DATA_RICE) return FS_FONT_ERR_VERSION;

    const uint32_t total = h->total_size;
    if (!fs_range_ok(h->preset_offset, h->preset_count, sizeof(FsPreset), total) ||
        !fs_range_ok(h->region_offset, h->region_count, sizeof(FsRegion), total) ||
        !fs_range_ok(h->mod_offset, h->mod_count, sizeof(FsModulator), total) ||
        !fs_range_ok(h->data_offset, data_size, 1, total) ||
        !fs_range_ok(seek_offset, seek_count, sizeof(FsSeekPoint), h->data_offset) ||
        h->data_offset < h->header_size) {
        return FS_FONT_ERR_LAYOUT;
    }
    if (format == FS_DATA_PCM16 && (uint64_t)h->data_frames * 2 != data_size) return FS_FONT_ERR_LAYOUT;
    if (format == FS_DATA_RICE && seek_count == 0 && h->region_count) return FS_FONT_ERR_LAYOUT;

    if (check_table_crc &&
        fs_crc32(0, base + h->header_size, h->data_offset - h->header_size) != h->table_crc) {
        return FS_FONT_ERR_TABLE_CRC;
    }
    if (check_data_crc && fs_crc32(0, base + h->data_offset, data_size) != h->data_crc) {
        return FS_FONT_ERR_DATA_CRC;
    }

    font->base = base;
    font->header = h;
    font->presets = (const FsPreset *)(base + h->preset_offset);
    font->regions = (const FsRegion *)(base + h->region_offset);
    font->mods = (const FsModulator *)(base + h->mod_offset);
    font->data = format == FS_DATA_PCM16 ? (const int16_t *)(base + h->data_offset) : NULL;
    font->cdata = base + h->data_offset;
    font->data_format = format;
    font->data_size = data_size;
    font->seek = seek_count ? (const FsSeekPoint *)(base + seek_offset) : NULL;
    font->seek_count = seek_count;

    // Seek table: strictly increasing frames, offsets inside the data
    for (uint32_t i = 0; i < seek_count; ++i) {
        const FsSeekPoint *sp = &font->seek[i];
        if (sp->frame >= h->data_frames || sp->offset >= data_size) return FS_FONT_ERR_LAYOUT;
        if (i > 0 && sp->frame <= font->seek[i - 1].frame) return FS_FONT_ERR_LAYOUT;
    }

    // Validate references so the synth never has to bounds check at runtime
    for (uint32_t i = 0; i < h->preset_count; ++i) {
        const FsPreset *p = &font->presets[i];
        if ((uint64_t)p->region_first + p->region_count > h->region_count) return FS_FONT_ERR_PRESET;
        if (i > 0) {
            const FsPreset *q = &font->presets[i - 1];
            if (((uint32_t)q->bank << 16 | q->program) >= ((uint32_t)p->bank << 16 | p->program)) {
                return FS_FONT_ERR_PRESET; // Must be strictly sorted for binary search
            }
        }
    }
    for (uint32_t i = 0; i < h->region_count; ++i) {
        const FsRegion *r = &font->regions[i];
        if ((uint32_t)r->mod_first + r->mod_count > h->mod_count) return FS_FONT_ERR_REGION;
        if (!(r->start < r->end && r->end <= h->data_frames)) return FS_FONT_ERR_REGION;
        if (r->loop_mode != FS_LOOP_NONE &&
            !(r->start <= r->loop_start && r->loop_start < r->loop_end && r->loop_end <= r->end)) {
            return FS_FONT_ERR_REGION;
        }
        if (r->sample_rate == 0 || r->root_key > 127) return FS_FONT_ERR_REGION;
        // Compressed: decoding must be able to start where the voice starts
        // and where it loops back to
        uint32_t off;
        if (format == FS_DATA_RICE &&
            (!fs_font_seek(font, r->start, &off) ||
             (r->loop_mode != FS_LOOP_NONE && !fs_font_seek(font, r->loop_start, &off)))) {
            return FS_FONT_ERR_REGION;
        }
    }

    return FS_FONT_OK;
}

// See fs_font.h. In RAM: called at every note-on of a compressed font.
bool FS_RAM_FUNC(fs_font_seek)(const FsFont *font, uint32_t frame, uint32_t *offset) {
    uint32_t lo = 0, hi = font->seek_count;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) >> 1;
        const uint32_t f = font->seek[mid].frame;
        if (f == frame) { *offset = font->seek[mid].offset; return true; }
        if (f < frame) lo = mid + 1; else hi = mid;
    }
    return false;
}

// See fs_font.h.
const char *fs_font_result_str(FsFontResult result) {
    switch (result) {
        case FS_FONT_OK: return "ok";
        case FS_FONT_ERR_MAGIC: return "bad magic (not a compiled font)";
        case FS_FONT_ERR_VERSION: return "unsupported version";
        case FS_FONT_ERR_SIZE: return "size mismatch";
        case FS_FONT_ERR_LAYOUT: return "bad table layout";
        case FS_FONT_ERR_TABLE_CRC: return "table CRC mismatch";
        case FS_FONT_ERR_DATA_CRC: return "sample data CRC mismatch";
        case FS_FONT_ERR_PRESET: return "invalid preset table";
        case FS_FONT_ERR_REGION: return "invalid region";
        default: return "unknown error";
    }
}

// See fs_font.h. Binary search on the key bank << 16 | program, the order
// sf2conv sorts the presets in and fs_font_init verifies. In RAM because
// program changes arrive on the render core.
const FsPreset *FS_RAM_FUNC(fs_font_find_preset)(const FsFont *font, uint16_t bank, uint16_t program) {
    const uint32_t key = (uint32_t)bank << 16 | program;
    uint32_t lo = 0, hi = font->header->preset_count;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) >> 1;
        const FsPreset *p = &font->presets[mid];
        const uint32_t k = (uint32_t)p->bank << 16 | p->program;
        if (k == key) return p;
        if (k < key) lo = mid + 1; else hi = mid;
    }
    return NULL;
}
