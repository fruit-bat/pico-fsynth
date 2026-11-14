# pico-fsynth on the ESP32-P4

Port of the synth to the Waveshare ESP32-P4-Pico (ESP32-P4NRW32: 32 MB PSRAM,
32 MB flash). It uses the same `core/` as the RP2350 firmware. Work in
progress.

## Installing ESP-IDF (once)

ESP-IDF is Espressif's SDK. It includes the build system, the RISC-V
compiler and the flashing tool (esptool). This port uses v6.1.1.

```sh
mkdir -p ~/esp && cd ~/esp
git clone --depth 1 --branch v6.1.1 --recursive --shallow-submodules \
    https://github.com/espressif/esp-idf.git esp-idf-v6.1.1
cd esp-idf-v6.1.1
./install.sh esp32p4          # tools go to ~/.espressif, about 2 GB in all
```

In each new shell, before using `idf.py` or `esptool`, run:

```sh
. ~/esp/esp-idf-v6.1.1/export.sh
```

`export.sh` may report that `openocd-esp32` failed because `libgpiod.so.2`
is missing. OpenOCD is only needed for JTAG debugging, so the error can be
ignored. `sudo apt install libgpiod2` fixes it if you want debugging.

The serial port needs your user in the `dialout` group: `groups` lists it.
If it is missing, run `sudo usermod -aG dialout $USER` and log in again.

## Talking to the board

Plug in the Type-C port (power and flashing), not the 4-pin USB OTG header.
The board appears as `/dev/ttyACM0`; `dmesg | tail` shows the name if it
is different.

```sh
esptool --port /dev/ttyACM0 chip-id     # chip type, revision, MAC
esptool --port /dev/ttyACM0 flash-id    # flash size (expect 32 MB)
idf.py -p /dev/ttyACM0 monitor          # serial console, Ctrl+] quits
```

These commands only read from the board. If esptool cannot connect, hold
BOOT, press and release RST, then release BOOT. The chip then waits in its
ROM bootloader, which works even when the program in flash is broken.

Note the chip revision that `chip-id` prints. ESP-IDF 6.1 builds for
revision v3.1 or later by default, and refuses to boot on an older chip.
For an older chip, change this in `idf.py menuconfig`, under Component
config, then Hardware Settings, then Chip revision:

- v3.0: set "Minimum Supported ESP32-P4 Revision" to v3.0
  (`CONFIG_ESP32P4_REV_MIN_300=y`).
- v1.x or earlier: tick "Select ESP32-P4 revisions <3.0"
  (`CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`). Firmware built this way runs
  only on chips older than v3.0.

## Building and flashing

```sh
. ~/esp/esp-idf-v6.1.1/export.sh
cd esp32p4
idf.py build                                  # first build: about 2 minutes
idf.py -DFS_DEMO_MIDI=/path/to/song.mid build # optional demo song played at boot
idf.py -p /dev/ttyACM0 -b 2000000 flash monitor   # Ctrl+] quits the monitor
```

`sdkconfig.defaults` holds the board settings: chip revision < 3.0, CPU at
360 MHz, 32 MB flash, PSRAM, and the custom partition table. They are
applied only when `sdkconfig` is first created. After editing
`sdkconfig.defaults`, delete `sdkconfig` and rebuild. The synth options
(pins, voices, dual core, font in PSRAM, fused multiply-add) are under
"pico-fsynth" in `idf.py menuconfig`.

### The font

The compiled font goes in the `font` partition at 0x110000, which fills the
rest of the flash: 30.9 MB (see `partitions.csv`). Build it on the PC (see
the main README), then write it once. `idf.py flash` leaves it alone.
Without `-b` esptool runs at 115200 baud and takes about 15 times longer:

```sh
esptool --port /dev/ttyACM0 -b 2000000 write-flash 0x110000 ../build-host/TimGM6mb.fsf
```

| Font | Size | Writing at 2,000,000 baud |
|---|---|---|
| TimGM6mb | 6.1 MB | about 45 s |
| GeneralUser GS 2.0.3, `sf2conv --compress` | 27.1 MB | about 4 min |

Where the font is read from at run time:
- **Fonts ending below 16 MB of flash** (up to 14.9 MB) are read in place
  through the flash cache, like XIP on the Pico.
- **Bigger fonts** are copied into PSRAM at boot. ESP-IDF can only
  memory-map the first 16 MB of flash, but ordinary reads reach all of it.
  The copy takes about 3 s for GeneralUser GS. PSRAM has about 33 MB free,
  so fonts up to about 31 MB fit, which in practice means compressed ones.
  GeneralUser GS is 34 MB uncompressed.
- The "Copy the font from flash into PSRAM" menuconfig option always
  copies it.

The boot log and the status line show which. The region index (the key
and velocity ranges checked at note-on) holds 16,384 regions, in PSRAM.

## Audio: Waveshare Pico-Audio (PCM5101A) stacked

The P4-Pico's right-hand header has the Pico layout, so the Pico-Audio
plugs straight on, USB ends together:

| Pico-Audio pin | Signal | ESP32-P4-Pico |
|---|---|---|
| 39 | VSYS (5 V) | VSYS |
| 38 | GND | GND |
| 36 | 3V3 | 3V3 |
| 34 | LRCK | GPIO21 |
| 32 | BCK | GPIO22 |
| 31 | DIN | GPIO23 |

These are the menuconfig defaults. A PCM5102A module needs the same three
signals plus 3.3 V or 5 V (check the module) and GND. On the module, tie SCK
to GND, FMT low and XSMT high.

## MIDI in

- **UART MIDI** at 31250 baud on GPIO20 (physical pin 35). It needs the
  usual opto-isolator circuit, as on the Pico.
- **Serial MIDI over the Type-C port**, at 115200 baud, alongside the
  console. Typed text is plain ASCII, so the firmware takes any byte of 0x80
  or above as a MIDI status byte and reads the rest of the message after
  it. Senders must not use running status: every message must start with
  its status byte.
- **USB MIDI** on the high-speed USB port: the 4-pin V / D- / D+ / G header
  at the bottom of the P4-Pico, not the Type-C. It appears as
  "pico-fsynth", with the same VID/PID as the Pico, so the `host/*.sh` SysEx
  scripts find it. The USB code is Espressif's `esp_tinyusb` component,
  which the build downloads (see `main/idf_component.yml`). Turn it off with
  "USB MIDI device" in menuconfig.

### USB MIDI connector

The high-speed port is the small 4-pin socket at the bottom of the
P4-Pico, labelled V, D-, D+, G. It looks like the MX1.25 socket used for
the speaker (1.25 mm pitch); check yours before buying plugs. The usual way
to connect it is a short USB-A (or USB-C) lead with the far end cut off and
soldered to a matching 4-pin plug. Standard USB cable colours:

| Header pin | Signal | Usual wire colour |
|---|---|---|
| V | 5 V (VBUS) | red |
| D- | data - | white |
| D+ | data + | green |
| G | ground | black |

- High-speed USB (480 Mbit/s) is fussy about the data pair. Keep the
  unshielded part of D+ and D- short (a few cm) and twisted together, and
  keep the cable's shield or drain wire on G if it has one.
- Two 5 V supplies: if the board is also powered from the Type-C, the V
  wire joins the PC's 5 V to the board's. That is usually harmless, but it
  can be left unconnected: the firmware does not watch VBUS
  (`self_powered = false`). Check the Waveshare schematic for how V is
  wired before relying on either.
- Check with `lsusb` (expect `cafe:4f53`) and `amidi -l` (expect
  "pico-fsynth MIDI").

`host/serial_midi` (built with the host tools when the ALSA development
files are installed, `libasound2-dev`) bridges an ALSA MIDI port to the
serial port. Run it in its own terminal and leave it running:

```sh
./build-host/serial_midi                 # or: serial_midi -b 115200 /dev/ttyUSB0
aplaymidi -p fsynth-serial song.mid      # in another terminal
aconnect 'My Keyboard' fsynth-serial     # or connect a keyboard / DAW
```

The `fsynth-serial` port exists only while `serial_midi` runs. If it is not
running, `aplaymidi` reports "Invalid port fsynth-serial".

It shows the board's status lines, and lines you type are sent to the board
as console commands. It does not reset the board when it starts. Close
`idf.py monitor` first: only one program can have the port open.

The `host/*.sh` SysEx scripts use `amidi`, which needs a raw MIDI device
rather than a sequencer port. The `snd-virmidi` kernel module provides one:

```sh
sudo modprobe snd-virmidi                # adds "Virtual Raw MIDI" devices
amidi -l                                 # find one, e.g. hw:3,0
aconnect 'Virtual Raw MIDI 3-0' fsynth-serial
host/master_volume.sh 50 hw:3,0
host/settings.sh save hw:3,0
```

## Settings

Master volume and EQ are saved in NVS, in the `nvs` partition, by the same
SysEx as on the Pico (`host/settings.sh save|erase`), and loaded at boot.
The status line shows them, and the boot log says whether saved settings
were loaded. A save holds up rendering for a few milliseconds while flash is
written.

## Console commands

For testing without MIDI hardware, type these in `idf.py monitor` (or in
`serial_midi`):

| Command | What it does |
|---|---|
| `stress VOICES [PROGRAM]` | Holds sustained notes until that many voices play (default program 16, Organ 1) |
| `off` | All Sound Off on every channel |
| `reset` | GM reset |
| `help` | Lists the commands |

## Performance

Measured on 2026-11-14 with `stress`: chip revision v1.3 at 360 MHz, both
cores, font read from flash, TimGM6mb.

| Voices | Fused multiply-add | Effects | Core 1 | Core 0 | Cycles per voice-sample |
|---|---|---|---|---|---|
| 80 | off | internal RAM | 61% | 56% | 108 |
| 112 | off | internal RAM | 82% | 77% | 108 |
| 112 | on | internal RAM | 72% | 67% | 94 |
| 112 | on | PSRAM | 72% | 67% | 94 |
| **128** (default) | on | PSRAM | 81% | 76% | 94 |
| 144 | on | PSRAM | 91% | 85% | 94 |

There were no underruns in any of these runs. All Sound Off briefly peaks
higher: 85% at 112 voices, and 107% at 144, which the output buffers
absorbed. That is why the default is 128.

- **Voice limit:** each voice needs about 2 KB of stream ring in internal
  RAM. With the reverb and chorus in internal RAM, 112 voices is the most
  that links. Moving them to PSRAM (the default) frees 70 KB, enough for
  144 voices, and costs no measurable time. 128 is then a CPU limit.
- **USB MIDI cost:** about 9 KB of internal RAM (2.3 KB fixed at build
  time, 6.8 KB allocated at boot) and 38 KB of flash. It makes no
  measurable CPU difference with nothing plugged in: 81.4% / 76% at 128
  voices with or without it. It has not yet been measured with a host
  sending MIDI.
- **Flash speed:** the flash runs at 80 MHz. ESP-IDF's default for
  chips older than v1.0 is 40 MHz, so `sdkconfig.defaults` sets the
  minimum chip revision to v1.0. Reading samples from cold flash (not in
  cache) takes 25.7 cycles per frame at 80 MHz, against 46.7 at 40 MHz.
  The `codec` console command measures this.
- **Compressed fonts** (`sf2conv --compress`): measured with `codec`, at
  80 MHz flash, in cycles per frame:

  | Font | Cold | Warm (in cache) |
  |---|---|---|
  | TimGM6mb, uncompressed | 25.7 | 2.0 |
  | TimGM6mb, `--compress=fast` | 36.2 | 24.1 |
  | TimGM6mb, `--compress=small` | about 60 | 49.6 |

  Compressed data cuts flash traffic by about half, but decoding costs
  more than that saves. At 128 voices, fast compression needs 99.8 cycles
  per voice-sample (86% load) against 94.5 (82%) uncompressed. The demo
  song needs about 116 against 102.
- **GeneralUser GS 2.0.3** (fast compressed, copied to PSRAM):
  - 128 voices: 103.9 cycles per voice-sample, 89.7% / 84% load.
  - Demo song: about 125 cycles. Its voices cost more than TimGM6mb's in
    the sample loop itself (74.7 against 69.5 cycles), which is the font,
    not the compression.
  - Reading compressed data from PSRAM costs 28.2 cycles per frame cold
    and 25.0 warm, so PSRAM adds almost nothing over the decoding.
- **Voice sample loop:** two options, both on by default, speed it up on
  the P4. They are measured with the `bench` console command (micro
  benchmarks of the loop, `main/bench.c`) and with `stress`:
  - The P4's FPU takes about 4 cycles to deliver a result to a dependent
    operation, but runs independent operations about 1.5 cycles apart.
    The cubic interpolation was a chain of about 6 dependent steps.
  - "Two-pass voice sample loop" (`FS_RENDER_TWO_PASS`) interpolates a
    block first, then filters and mixes. Output is bit-identical. It saves
    9% in `bench`, but only about 1.5% in the synth, where fetching each
    voice's ring from internal RAM stalls the in-order core.
  - "Interpolation weight table" (`FS_INTERP_TABLE_BITS`, 10 bits, 16 KB)
    looks up the four cubic weights instead of evaluating the polynomial.
    Rendered audio differs from the polynomial by at most 1 LSB (about
    73 dB below the music, on GeneralUser GS).
  - Together, measured with GeneralUser GS at 128 voices: organ notes 89.7%
    to 82.7% load (sample loop 74.7 to 65.6 cycles), string notes 92.1% to
    87.5%, demo song about 125 to 117 cycles per voice-sample.
- **Font in PSRAM:** reading samples from flash costs under 1 cycle per
  voice-sample, so copying the font to PSRAM has little to gain.

## Not ported yet

The micro benchmarks.
