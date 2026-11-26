#!/bin/bash
#
# Build ready-to-flash UF2 files: firmware, font, and firmware + font merged.
# Used by .github/workflows/uf2.yml, and works the same locally.
#
# Usage: host/make_uf2s.sh [options]
#   --variants LIST    comma separated, from (default: all of them)
#                        pico-audio      Waveshare Pico-Audio (PCM5101A) stacked
#                        pico-audio-v21  Waveshare Pico-Audio Rev2.1 (CS4344)
#                        i2s             I2S DAC on GPIO 12 (data), 13/14 (clocks)
#                        pwm             PWM on GPIO 20/21 (needs an RC filter)
#   --board NAME       PICO_BOARD (default waveshare_rp2350_plus_16mb)
#   --flash-mb N       flash size in MB, sets the font budget (default 16)
#   --sf2 FILE         SoundFont to convert (default: GeneralUser GS 2.0.3,
#                      downloaded to build-dist/ if not there)
#   --force LIST       presets to always keep (default 0:24,0:44,0:48,0:49,0:73)
#   --keep LIST        keep exactly these presets (sf2conv --keep), no fitting
#   --cmake ARGS       more firmware cmake options, e.g. "-DFS_MAX_VOICES=64"
#   --no-merge         do not write the merged firmware + font UF2s
#   --no-tests         do not run the host tests
#
# Needs PICO_SDK_PATH (pico-sdk 2.x), arm-none-eabi-gcc, cmake, python3, curl.
# Everything goes in build-dist/; the results in build-dist/out/:
#   pico_fsynth-VARIANT.uf2          firmware only (keeps the font in flash)
#   font-NAME.uf2                    font only (keeps the firmware)
#   pico_fsynth-VARIANT+NAME.uf2     both: one drag-and-drop does it all
#   font-NAME.txt                    what the font holds and what was left out
set -euo pipefail

variants=pico-audio,pico-audio-v21,i2s,pwm
board=waveshare_rp2350_plus_16mb
flash_mb=16
sf2=
force=0:24,0:44,0:48,0:49,0:73
keep=
cmake_extra=
merge=true
tests=true
while [ $# -gt 0 ]; do
    case $1 in
        --variants) variants=$2; shift ;;
        --board) board=$2; shift ;;
        --flash-mb) flash_mb=$2; shift ;;
        --sf2) sf2=$2; shift ;;
        --force) force=$2; shift ;;
        --keep) keep=$2; shift ;;
        --cmake) cmake_extra=$2; shift ;;
        --no-merge) merge=false ;;
        --no-tests) tests=false ;;
        *) sed -n '6,22s/^# \?//p' "$0" >&2; exit 2 ;;
    esac
    shift
done

root=$(cd "$(dirname "$0")/.." && pwd)
dist=$root/build-dist
out=$dist/out
mkdir -p "$out"
jobs=$(nproc 2>/dev/null || echo 4)

. "$root/host/generaluser.sh"

echo "== host tools"
cmake -S "$root/host" -B "$dist/host" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$dist/host" -j "$jobs" >/dev/null
if $tests; then
    ctest --test-dir "$dist/host" --output-on-failure
fi

echo "== font"
if [ -z "$sf2" ]; then
    sf2=$dist/GeneralUser-GS.sf2
    generaluser_get "$sf2" "$out"
    name=GeneralUser-GM-${flash_mb}mb
else
    name=$(basename "${sf2%.*}")-${flash_mb}mb
fi
# The font goes from 2 MB to the settings sector (the last 4 KB)
size=$((flash_mb * 1024 * 1024 - 0x200000 - 0x1000))
fsf=$dist/$name.fsf
if [ -n "$keep" ]; then
    "$dist/host/sf2conv" --compress --keep="$keep" "$sf2" "$fsf"
    echo "$keep" > "$dist/$name.keep"
    if [ "$(stat -c %s "$fsf")" -gt "$size" ]; then
        echo "$fsf: $(stat -c %s "$fsf") bytes, more than the $size bytes of flash for it" >&2
        exit 1
    fi
    "$dist/host/fsfinfo" "$fsf" > "$out/font-$name.txt"
else
    python3 "$root/host/font_fit.py" --tools "$dist/host" --size "$size" --force "$force" \
        "$sf2" "$fsf" | tee "$out/font-$name.txt"
fi
python3 "$root/host/uf2_merge.py" "$out/font-$name.uf2" "$fsf@0x10200000"

for v in ${variants//,/ }; do
    echo "== firmware $v"
    case $v in
        pico-audio) opts=(-DFS_PICO_AUDIO=ON) ;;
        pico-audio-v21) opts=(-DFS_PICO_AUDIO_V21=ON) ;;
        i2s) opts=(-DFS_AUDIO_OUT=I2S) ;;
        pwm) opts=(-DFS_AUDIO_OUT=PWM) ;;
        *) echo "unknown variant $v" >&2; exit 2 ;;
    esac
    # shellcheck disable=SC2086  # cmake_extra is a list of options
    cmake -S "$root" -B "$dist/fw-$v" -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD="$board" \
        "${opts[@]}" $cmake_extra >/dev/null
    cmake --build "$dist/fw-$v" -j "$jobs" >/dev/null
    cp "$dist/fw-$v/pico_fsynth.uf2" "$out/pico_fsynth-$v.uf2"
    if $merge; then
        python3 "$root/host/uf2_merge.py" "$out/pico_fsynth-$v+$name.uf2" \
            "$dist/fw-$v/pico_fsynth.uf2" "$fsf@0x10200000"
    fi
done

echo "== done"
ls -l "$out"
