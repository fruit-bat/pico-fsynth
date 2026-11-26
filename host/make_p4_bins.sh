#!/bin/bash
#
# Build ready-to-flash images for the ESP32-P4 port (Waveshare ESP32-P4-Pico):
# the app, the font, and everything merged into one image written at 0x0.
# Used by .github/workflows/build.yml, and works the same locally.
#
# Usage: host/make_p4_bins.sh [options]
#   --revs LIST        chip revisions, comma separated (default: both)
#                        rev1  chips before v3.0, e.g. v1.3 (most P4-Pico boards so far)
#                        rev3  chips v3.0 and later
#   --sf2 FILE         SoundFont to convert (default: GeneralUser GS 2.0.3,
#                      downloaded to build-dist/ if not there)
#   --keep LIST        keep only these presets (sf2conv --keep); default all
#   --sdkconfig LINES  more sdkconfig settings, separated by spaces or
#                      newlines, e.g. "CONFIG_FSYNTH_MAX_VOICES=96"
#   --no-merge         do not write the merged firmware + font images
#
# Needs ESP-IDF 6.1 (. export.sh first: idf.py and esptool), a host C
# compiler, cmake, python3 and curl. Builds go in build-dist/; the results
# in build-dist/out/:
#   fsynth-p4-REV+NAME.bin       everything (bootloader, partition table, app,
#                                font), write at 0x0; resets saved settings
#   fsynth-p4-REV-app.bin        the app only, write at 0x10000 to update
#                                (keeps the font and the saved settings)
#   font-NAME-p4.fsf             the font only, write at 0x110000
#   font-NAME-p4.txt             the presets in the font
#
# The firmware uses esp32p4/sdkconfig as it is, with only the chip revision
# changed for rev3, then --sdkconfig on top.
set -euo pipefail

revs=rev1,rev3
sf2=
keep=
sdk_extra=
merge=true
while [ $# -gt 0 ]; do
    case $1 in
        --revs) revs=$2; shift ;;
        --sf2) sf2=$2; shift ;;
        --keep) keep=$2; shift ;;
        --sdkconfig) sdk_extra=$2; shift ;;
        --no-merge) merge=false ;;
        *) sed -n '7,18s/^# \?//p' "$0" >&2; exit 2 ;;
    esac
    shift
done
if ! command -v idf.py >/dev/null || ! command -v esptool >/dev/null; then
    echo "idf.py and esptool not found: run . ~/esp/esp-idf-v6.1.1/export.sh first" >&2
    exit 1
fi

root=$(cd "$(dirname "$0")/.." && pwd)
dist=$root/build-dist
out=$dist/out
mkdir -p "$out"
jobs=$(nproc 2>/dev/null || echo 4)
font_offset=0x110000
font_space=$((0x1ef0000))     # the font partition, esp32p4/partitions.csv

. "$root/host/generaluser.sh"

echo "== host tools"
cmake -S "$root/host" -B "$dist/host" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$dist/host" -j "$jobs" --target sf2conv fsfinfo >/dev/null

echo "== font"
if [ -z "$sf2" ]; then
    sf2=$dist/GeneralUser-GS.sf2
    generaluser_get "$sf2" "$out"
    name=GeneralUser-GS
else
    name=$(basename "${sf2%.*}")
fi
fsf=$out/font-$name-p4.fsf
"$dist/host/sf2conv" --compress ${keep:+--keep="$keep"} "$sf2" "$fsf"
size=$(stat -c %s "$fsf")
if [ "$size" -gt "$font_space" ]; then
    echo "$fsf: $size bytes, more than the font partition ($font_space bytes)" >&2
    exit 1
fi
"$dist/host/fsfinfo" "$fsf" > "$out/font-$name-p4.txt"
echo "$fsf: $size bytes ($((size * 100 / font_space))% of the font partition)"

for r in ${revs//,/ }; do
    echo "== firmware $r"
    b=$dist/p4-$r
    mkdir -p "$b"
    # The tracked sdkconfig is the base; later files override earlier ones.
    defaults="$root/esp32p4/sdkconfig"
    case $r in
        rev1) ;;
        rev3)
            printf '%s\n' '# CONFIG_ESP32P4_SELECTS_REV_LESS_V3 is not set' \
                'CONFIG_ESP32P4_REV_MIN_300=y' > "$b/sdkconfig.rev"
            defaults+=";$b/sdkconfig.rev" ;;
        *) echo "unknown revision $r" >&2; exit 2 ;;
    esac
    if [ -n "$sdk_extra" ]; then
        printf '%s\n' $sdk_extra > "$b/sdkconfig.extra"
        defaults+=";$b/sdkconfig.extra"
    fi
    rm -f "$b/sdkconfig"            # always start again from the defaults
    idf.py -C "$root/esp32p4" -B "$b/build" -D SDKCONFIG="$b/sdkconfig" \
        -D SDKCONFIG_DEFAULTS="$defaults" build >"$b/build.log" 2>&1 ||
        { tail -40 "$b/build.log"; exit 1; }
    grep -E "^CONFIG_ESP32P4_REV_M(IN|AX)_FULL" "$b/sdkconfig" | tr '\n' ' '; echo
    cp "$b/build/fsynth_p4.bin" "$out/fsynth-p4-$r-app.bin"
    if $merge; then
        (cd "$b/build" && esptool --chip esp32p4 merge-bin -o "$out/fsynth-p4-$r+$name.bin" \
            @flash_args "$font_offset" "$fsf" >/dev/null)
    fi
done

cat > "$out/FLASHING-P4.txt" <<EOF
pico-fsynth for the ESP32-P4 (Waveshare ESP32-P4-Pico)

Plug in the Type-C port. If esptool cannot connect, hold BOOT, press and
release RST, then release BOOT. Pick rev1 for chips before v3.0 (most
P4-Pico boards so far), rev3 for v3.0 and later; esptool chip-id prints
the revision.

First time, everything (bootloader, partition table, app and font), about
4 minutes. This also clears saved settings (volume, EQ, width):
  esptool --chip esp32p4 -b 2000000 write-flash 0x0 fsynth-p4-rev1+$name.bin

Updating the app (keeps the font and saved settings):
  esptool --chip esp32p4 -b 2000000 write-flash 0x10000 fsynth-p4-rev1-app.bin

The font only (keeps the app and saved settings):
  esptool --chip esp32p4 -b 2000000 write-flash $font_offset font-$name-p4.fsf

esptool: pip install esptool (or the one in ESP-IDF).
EOF

echo "== done"
ls -l "$out"
