#pragma once
//
// Settings kept in the last flash sector: master volume and master EQ.
// Saved and erased on request (SysEx F0 7D 46 53 cmd F7, see midi_in.h),
// loaded at power up.
//
#include <stdbool.h>
#include "fs_synth.h"

// Apply saved settings to the synth, if there are valid ones.
// Returns true if settings were loaded.
bool settings_load(FsSynth *synth);

// False if the font reaches into the settings sector (saving is then refused)
bool settings_available(const FsFont *font);

// Write the synth's current settings / erase them. Flash is unavailable to
// both cores meanwhile: call from core 0 with core 1 parked in RAM.
// Interrupts on core 0 are disabled for the erase (tens of ms).
void settings_save(const FsSynth *synth);
void settings_erase(void);
