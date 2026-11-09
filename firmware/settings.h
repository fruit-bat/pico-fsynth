#pragma once
//
// settings.h - device settings kept in the last flash sector.
//
// Holds master volume and master EQ: the settings that are properties of
// the device rather than of a song, so GM/GS resets do not change them.
// Saved and erased on request (SysEx F0 7D 46 53 cmd F7, see midi_in.h and
// host/settings.sh) and loaded at power up. The font must end below the
// last sector for saving to be possible (settings_available()).
//
#include <stdbool.h>
#include "fs_synth.h"

// Apply saved settings to the synth, if a valid record (magic, version,
// size and CRC all match) is in flash. Returns true if settings were
// loaded; otherwise the synth is left unchanged. Call at start up, before
// rendering starts.
bool settings_load(FsSynth *synth);

// False if the font reaches into the settings sector (saving is then
// refused, because erasing the sector would damage the font).
bool settings_available(const FsFont *font);

// Write the synth's current settings / erase them. Flash is unavailable to
// both cores meanwhile: call from core 0 with core 1 parked in RAM (see the
// park handshake in main.c). Interrupts on core 0 are disabled for the
// erase and program (tens of ms), so USB stalls briefly.
void settings_save(const FsSynth *synth);
void settings_erase(void);
