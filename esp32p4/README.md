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

The compiled font goes in the `font` partition at 0x200000 (14 MB, see
`partitions.csv`). Build it on the PC (see the main README), then write it
once. `idf.py flash` leaves it alone. Without `-b` esptool runs at 115200
baud, and the font takes about 7 minutes instead of 45 seconds:

```sh
esptool --port /dev/ttyACM0 -b 2000000 write-flash 0x200000 ../build-host/TimGM6mb.fsf
```

By default the font is read in place from flash through the cache, like
XIP on the Pico. The "Copy the font from flash into PSRAM" option copies
it into PSRAM at boot instead. The status line shows which.

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

UART MIDI at 31250 baud on GPIO20 (physical pin 35). It needs the usual
opto-isolator circuit, as on the Pico. USB MIDI is not ported yet.

## Not ported yet

USB MIDI, saving settings (`host/settings.sh`), the micro benchmarks.
