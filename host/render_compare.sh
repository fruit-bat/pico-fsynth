#!/bin/bash
#
# Render every test MIDI file with pico-fsynth and FluidSynth for listening
# side by side.
#
# Usage: host/render_compare.sh [extra.mid ...]
#
# Output in build-host/test:  NAME.mid  NAME.fsynth.wav  NAME.fluid.wav
# Extra MIDI files given on the command line are copied in first (spaces in
# their names are replaced with underscores).
#
set -e
P=$(cd "$(dirname "$0")/.." && pwd)
OUT=$P/build-host/test
FSF=$P/build-host/TimGM6mb.fsf
SF2=$P/TimGM6mb.sf2

mkdir -p "$OUT"
[ -f "$FSF" ] || "$P/build-host/sf2conv" "$SF2" "$FSF" >/dev/null
python3 -I "$P/host/make_test_midi.py" "$OUT" >/dev/null

for f in "$@"; do
    name=$(basename "$f" .mid)
    cp "$f" "$OUT/${name// /_}.mid"
done

for mid in "$OUT"/*.mid; do
    name=$(basename "$mid" .mid)
    "$P/build-host/fsrender" "$FSF" "$mid" "$OUT/$name.fsynth.wav" | head -1
    fluidsynth -q -n -i -r 44100 -F "$OUT/$name.fluid.wav" "$SF2" "$mid" >/dev/null 2>&1
done
