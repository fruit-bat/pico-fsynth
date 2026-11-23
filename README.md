# pico-fsynth

SoundFont 2 MIDI synthesizer for the RP2350 (target: Waveshare RP2350-Plus, 16 MB flash).

## Layout

- `core/` portable synth: font reader, voices, effects, MIDI file player
- `firmware/` RP2350 specific: audio output (I2S / PWM), MIDI input (UART / USB), main
- `esp32p4/` ESP32-P4 port (ESP-IDF); see `esp32p4/README.md` for installing ESP-IDF
- `host/` PC tools: font converter, font inspector, MIDI to WAV renderer, test scripts
- `tests/` automated tests of the core, the firmware's MIDI parsers and the host tools

## Fonts

SoundFonts are compiled on the PC into a flat `.fsf` image that the synth reads
in place from flash. The converter flattens presets/instruments into resolved
regions, merges default modulators and resolves sample positions and loops.

See https://github.com/craffel/pretty-midi/

```sh
cmake -S host -B build-host
cmake --build build-host
./build-host/sf2conv TimGM6mb.sf2 build-host/TimGM6mb.fsf
./build-host/fsfinfo build-host/TimGM6mb.fsf          # list presets
./build-host/fsfinfo build-host/TimGM6mb.fsf 0 0      # regions of bank 0 program 0
```

`sf2conv --compress` stores the samples losslessly compressed (renders are
bit-identical), in one of two block types:

| Option | TimGM6mb | GeneralUser GS 2.0.3 | Decoding (ESP32-P4, data in cache) |
|---|---|---|---|
| (none) | 5.73 MB of samples | 32.0 MB | 2 cycles per frame (a copy) |
| `--compress` or `--compress=fast` | 87.3% | 78.3% | 24 cycles per frame |
| `--compress=small` | 81.2% | 71.1% | 50 cycles per frame |

The decoder runs per frame of the original sample, so a note played above
its sample's pitch costs proportionally more. On the ESP32-P4 at 128 voices,
fast costs about 6% more CPU than uncompressed. Compression is mainly a
way to fit bigger fonts. The format is described in `core/fs_codec.h`. The
firmware still reads version 1 (uncompressed) fonts.

`sf2conv -v` lists warnings about the source font. `--vel-filter=off|fluid|spec`
chooses how the default "velocity to filter cutoff" modulator behaves:
`off` (default) removes it, which is what FluidSynth 2.4 actually plays
(renders match it only this way), `fluid` lowers the cutoff for velocities
64..126 as FluidSynth's source describes, `spec` follows the SF2 2.01 text
(soft notes get darker).

`--keep=LIST` keeps only some presets, and only the samples they use: whole
banks (`0`) and single presets (`128:0`), comma separated. A program that
is not in the font falls back to bank 0, then to 0:0 (drum kits to kit 0);
if that is missing too, the channel is silent, so keep 0:0 unless the music
sets a program on every channel it uses. For example GeneralUser GS (27 MB
with `--compress`) is too big for a 16 MB Pico, but a font of just the
instruments a game's tunes use is small:

```sh
./build-host/sf2conv --compress --keep=0,128:0 GeneralUser-GS.sf2 gm.fsf            # GM only: 23.8 MB
./build-host/sf2conv --compress --keep=0:78,0:11,0:38,128:0 GeneralUser-GS.sf2 t.fsf  # 4 presets: 2.2 MB
```

`host/font_fit.py` picks as many presets as fit in a given size; see
"Squeezing a GM font into 16 MB" under Firmware.

## Rendering on the PC

```sh
./build-host/fsrender build-host/TimGM6mb.fsf song.mid out.wav     # -d = no effects, -r rate, -g gain
python3 host/make_test_midi.py build-host/test                      # feature test MIDI files
fluidsynth -n -i -r 44100 -F ref.wav TimGM6mb.sf2 song.mid          # reference render
python3 host/compare_wav.py out.wav ref.wav                          # level / envelope / brightness
host/render_compare.sh [more.mid ...]   # every test file -> build-host/test/NAME.fsynth.wav + NAME.fluid.wav
```

## Tests

The host build also builds the test suite in `tests/` (plain C, run by CTest):

```sh
cmake -S host -B build-host && cmake --build build-host
ctest --test-dir build-host --output-on-failure

# The same under AddressSanitizer + UndefinedBehaviorSanitizer
cmake -S host -B build-host-asan -DFS_SANITIZE=ON && cmake --build build-host-asan
ctest --test-dir build-host-asan --output-on-failure
```

| Test | Covers |
|------|--------|
| `math` | unit conversions, fast exp2/sine, modulator curves, CRC-32 |
| `smf` | MIDI file player: timing, tempo, running status, SysEx, truncated files |
| `stream` | sample streamer: loop unrolling, end of sample, loop until release |
| `synth` | font reader checks, pitch, note on/off, sustain, layers, exclusive class, voice stealing, split render |
| `midi_parse` | firmware UART / USB MIDI parsers and SysEx recognition (`firmware/midi_parse.c`) |
| `tools` | runs `sf2conv` and `fsrender` on generated SoundFonts and MIDI files |

To add a test, add a `TEST(name)` function to the relevant `tests/test_*.c`
and `RUN(name)` it from `main`; a new file also needs a line in
`tests/CMakeLists.txt`.

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

Copy the firmware UF2 and the font UF2 separately: the boot ROM reboots once
it has all of the first file. Or merge them into one UF2 and flash both
in one go:

```sh
host/uf2_merge.py build-host/fsynth+font.uf2 build/pico_fsynth.uf2 build-host/font.uf2
cp build-host/fsynth+font.uf2 /media/$USER/RP2350/ && sync      # a 29 MB copy takes about 4 minutes
```

### Squeezing a GM font into 16 MB

The font has the flash from 0x200000 (2 MB, after the firmware) up to the
settings sector at 0xFFF000: 0xDFF000 bytes (14.68 MB) on a 16 MB board
such as the RP2350-Plus. GeneralUser GS 2.0.3 is 27 MB with `--compress`, and
its GM set alone (bank 0 + drum kit 128:0) is still 23.8 MB. So keep as many
GM presets as fit. Many presets share samples, so the cheapest ones to add
are those whose samples are already in.

`host/font_fit.py` does this. It converts the GM set, orders the presets
greedily (0:0 and 128:0 first, then any `--force` presets, then each time
the one that adds the fewest new sample frames), then binary searches the
number of presets with real `sf2conv` runs to find the largest font that
fits. It writes the `.fsf`, its `--keep` list as `.keep`, and lists the
presets that were left out. Those play on 0:0 (piano).

The font on the RP2350-Plus + Pico-Audio (105 of 129 presets, 14.57 MB).
Nylon guitar (24), strings (44, 48, 49) and flute (73) are forced in:

```sh
host/font_fit.py --force 0:24,0:44,0:48,0:49,0:73 \
    GeneralUser-GS.sf2 build-host/GeneralUser-GM-pico.fsf
picotool uf2 convert build-host/GeneralUser-GM-pico.fsf -t bin build-host/font.uf2 -o 0x10200000 --family rp2350-arm-s
host/uf2_merge.py build-host/fsynth+font.uf2 build-plus-audio/pico_fsynth.uf2 build-host/font.uf2
cp build-host/fsynth+font.uf2 /media/$USER/RP2350/ && sync
```

Left out: 4 tine electric piano, 14 tubular bells, 15 dulcimer, 19 pipe
organ, 25/27/29/30 steel, clean, overdrive and distortion guitar, 40-43
solo strings, 52-54 voices, 60 French horns, 61 brass section, 65 alto sax,
84 charang, 93/94 pads, 102/103 FX, 110 fiddle. To swap some in, give
`--force` the ones you need, and others drop out to make room. To rebuild the
same font without the search:
`sf2conv --compress --keep=$(cat build-host/GeneralUser-GM-pico.keep) GeneralUser-GS.sf2 out.fsf`.
Other options: `--compress=small` (about 10% smaller, twice the decode cost),
or `--size` for another flash size. A run takes about 20 seconds for
GeneralUser.

The compressed font costs CPU on the Pico. Reading from flash and decoding
take about 40 cycles per voice-sample, against about 3 for uncompressed
samples. The title tune ran at about 30% of the render core
with 20 voices, so a full 80 voices may be near the limit.

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

The Rev2.1 board uses a CS4344 DAC on different pins: DIN 22, MCLK 26,
LRCK 27, SCLK 28. The CS4344 needs a master clock, so `-DFS_PICO_AUDIO_V21=ON`
selects those pins and a second PIO program (`firmware/audio_i2s_mclk.pio`).
That program runs two state machines in sync: MCLK = 256 x Fs (11.29 MHz at
44.1 kHz), and BCLK = 64 x Fs with 16-bit samples in 32-bit slots:

```sh
cmake -S . -B build-plus-audio21 -DFS_PICO_AUDIO_V21=ON
cmake --build build-plus-audio21
```

For other wiring, set `-DFS_I2S_MCLK_PIN=n` yourself. With MCLK on,
`FS_I2S_CLOCK_PIN_BASE` is the LRCLK pin and BCLK is the next GPIO. This is
the reverse of the order without MCLK.

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

Stereo width pulls hard panned parts towards the centre, e.g. for
orchestral files on headphones: `host/stereo_width.sh PERCENT` (SysEx
`F0 7D 46 53 03 ww F7`), 100 = as the file pans it (default), 50 = halfway,
0 = mono. It scales the difference between left and right of the final mix.
`fsrender -W PERCENT` does the same on the PC.

The scripts in `host/` take an optional port: a raw MIDI device such as
`hw:2,0,0` (`amidi -l`) or an ALSA sequencer port such as `fsynth-serial`
(the ESP32-P4 serial bridge). Without one they use the pico-fsynth USB MIDI
device, or the bridge if that is running (`host/midi_port.sh`).

Master volume, EQ and stereo width can be saved to the last flash sector and are then loaded
at power up: `host/settings.sh save` (SysEx `F0 7D 46 53 01 F7`), or
`host/settings.sh erase` (`... 02 F7`) to go back to defaults. Audio drops out
for a moment while the flash is written. Reflashing the firmware or the font
does not touch saved settings.

    host/master_eq.sh 6 -6 && host/stereo_width.sh 70 && host/settings.sh save

To play files from the PC, use `host/play.sh`:

    host/play.sh song.mid other.mid     # Ctrl-C stops and silences
    host/play.sh -g song.mid            # also GM System On before each file
    host/play.sh -p fsynth-serial song.mid

Before each file it resets the controllers on every channel. Most MIDI files
assume a freshly reset synth and never clear the mod wheel, pitch bend,
expression or pedals, so a file played straight after another with plain
`aplaymidi` inherits what the first one left. A leftover mod wheel, for
example, puts heavy vibrato on a piano piece.

`host/reset.sh` sends the same reset on its own (All Sound Off and Reset All
Controllers on every channel), e.g. after stopping a plain `aplaymidi` part
way, which leaves notes hanging. `host/reset.sh -g` also sends GM System On
(programs back to piano).
