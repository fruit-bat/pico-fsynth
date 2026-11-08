# pico-fsynth

SoundFont 2 MIDI synthesizer for the RP2350 (target: Waveshare RP2350-Plus, 16 MB flash).

## Layout

- `core/` portable synth: font reader, voices, effects, MIDI file player
- `firmware/` RP2350 specific: audio output (I2S / PWM), MIDI input (UART / USB), main
- `host/` PC tools: font converter, font inspector, MIDI to WAV renderer, test scripts

## Fonts

SoundFonts are compiled on the PC into a flat `.fsf` image that the synth reads
in place from flash. The converter flattens presets/instruments into resolved
regions, merges default modulators and resolves sample positions and loops.

```sh
cmake -S host -B build-host
cmake --build build-host
./build-host/sf2conv TimGM6mb.sf2 build-host/TimGM6mb.fsf
./build-host/fsfinfo build-host/TimGM6mb.fsf          # list presets
./build-host/fsfinfo build-host/TimGM6mb.fsf 0 0      # regions of bank 0 program 0
```

`sf2conv -v` lists warnings about the source font. `--vel-filter=fluid|spec|off`
chooses how the default "velocity to filter cutoff" modulator behaves:
`fluid` (default) matches FluidSynth, `spec` follows the SF2 2.01 text (soft
notes get darker), `off` removes it.

## Rendering on the PC

```sh
./build-host/fsrender build-host/TimGM6mb.fsf song.mid out.wav     # -d = no effects, -r rate, -g gain
python3 host/make_test_midi.py build-host/test                      # feature test MIDI files
fluidsynth -n -i -r 44100 -F ref.wav TimGM6mb.sf2 song.mid          # reference render
python3 host/compare_wav.py out.wav ref.wav                          # level / envelope / brightness
host/render_compare.sh [more.mid ...]   # every test file -> build-host/test/NAME.fsynth.wav + NAME.fluid.wav
```

## Firmware

```sh
cmake -S . -B build -DFS_AUDIO_OUT=I2S        # or PWM
cmake --build build
```

Options (all `-D...`): `FS_AUDIO_OUT`, `FS_SAMPLE_RATE` (44100), `FS_SYS_CLOCK_KHZ`
(250000, an overclock: the RP2350 is rated at 150 MHz but 80 voices need about
70% of both cores at 250 MHz; the core voltage is raised to 1.2 V above 150 MHz), `FS_MAX_VOICES` (80), `FS_FONT_FLASH_OFFSET` (0x200000), pin numbers
below, and `FS_DEMO_MIDI=/path/song.mid` to embed a song that plays at power up
until MIDI input arrives.

### Flashing

The font goes at 2 MB into flash, separately from the firmware, so firmware
updates do not rewrite it. With the board in BOOTSEL mode:

```sh
picotool load -t bin build-host/TimGM6mb.fsf -o 0x10200000   # once per font
picotool load -x build/pico_fsynth.uf2                        # firmware
```

or make a drag-and-drop UF2 of the font:

```sh
picotool uf2 convert build-host/TimGM6mb.fsf -t bin build-host/font.uf2 -o 0x10200000 --family rp2350-arm-s
```

### Connections (defaults)

| Function | GPIO |
|---|---|
| I2S data (DIN) | 12 |
| I2S BCLK | 13 |
| I2S LRCLK (WS) | 14 |
| PWM left / right | 20 / 21 (RC low pass on each, e.g. 1k + 10 nF) |
| MIDI in (UART1 RX, via opto-isolator) | 5 |

### Waveshare Pico-Audio stacked

The RP2350-Plus (or any Pico-pinout board) plugs straight on top of a
Waveshare Pico-Audio (PCM5101A version), USB end over the USB outline on the
board. Its I2S pins are DIN 26, BCK 27, LRCK 28; `-DFS_PICO_AUDIO=ON` selects
them:

```sh
cmake -S . -B build-plus-audio -DFS_PICO_AUDIO=ON
cmake --build build-plus-audio
```

The Pico-Audio takes its power from VSYS, so USB powering the Pico is enough.
GPIO 5 (MIDI in) is not used by the Pico-Audio.

USB shows up as a serial console plus a USB MIDI device. The console prints a
status line every second: CPU load of the render core, peak voices, audio
underruns, MIDI message counts, output samples clipped and the master volume.

Master volume follows the GM Master Volume SysEx (`F0 7F 7F 04 01 lsb msb F7`,
0..16383, default full). From the PC: `host/master_volume.sh 50` (percent).

A master EQ follows the Roland GS master EQ SysEx (address `40 02 00..03`: bass
shelf at 200 or 400 Hz, treble shelf at 3 or 6 kHz, -12..+12 dB each; flat and
bypassed by default). From the PC: `host/master_eq.sh BASS_DB TREBLE_DB [200|400
3000|6000]`, e.g. `host/master_eq.sh 0 -4`. `fsrender -E bass_db,treble_db`
does the same on the PC. Master volume and EQ survive GM/GS resets.

From the PC:
    host/master_eq.sh 0 -4          # 4 dB less treble above 3 kHz
    host/master_eq.sh 0 -6 200 6000 # gentler: only above 6 kHz
    host/master_eq.sh 3 -3          # a little bass, a little less treble
    host/master_eq.sh 0 0           # flat

Master volume and EQ can be saved to the last flash sector and are then loaded
at power up: `host/settings.sh save` (SysEx `F0 7D 46 53 01 F7`), or
`host/settings.sh erase` (`... 02 F7`) to go back to defaults. Audio drops out
for a moment while the flash is written. Reflashing the firmware or the font
does not touch saved settings.

    host/master_eq.sh 6 -6 && host/settings.sh save

Playing a file from the PC and stopping it part way leaves notes hanging;
`host/reset.sh` silences everything (All Sound Off and Reset All Controllers on
every channel), `host/reset.sh -g` also sends GM System On (programs back to
piano):

    aplaymidi -p 24:0 song.mid      # Ctrl-C to stop, then
    host/reset.sh
