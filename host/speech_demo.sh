#!/bin/sh
#
# speech_demo.sh [OUTDIR] - render speech and singing examples with fsspeak
# into OUTDIR (default build-host/speech-demo).
#
# Needs the host build and the dictionary:
#   cmake -S host -B build-host && cmake --build build-host
#   host/get_cmudict.sh
#
set -e
BIN=$(dirname "$0")/../build-host
OUT=${1:-$BIN/speech-demo}
SPEAK=$BIN/fsspeak
mkdir -p "$OUT"

say() {     # say FILE OPTIONS... "TEXT"
    f=$1; shift
    printf '%-14s ' "$f"
    "$SPEAK" "$@" "$OUT/$f.wav"
}

say hello -t "Hello. I am the pico synthesizer. Can you understand me?"
say sentences -t "The birch canoe slid on the smooth planks. Glue the sheet to the dark blue background. \
It's easy to tell the depth of a well. Four hours of steady work faced us."
say numbers -t "Please call 0 1 2 3, extension 4,567, before 9 o'clock."
say slow -s 0.75 -t "This is the same voice, speaking more slowly."
say fast -s 1.5 -t "And this is the same voice, speaking rather quickly."
say high -p 190 -f 1.15 -t "With the formants raised and a higher pitch, the voice sounds younger."
say low -p 80 -f 0.92 -t "And with them lowered, it sounds bigger."
say question -t "Did you know that it can ask questions? It can."

# Singing: NOTE:MS on each vowel. "Twinkle twinkle little star, how I
# wonder what you are" (C C G G A A G, F F E E D D C)
say twinkle "T W IH1@60:400 NG K AH0@60:400 L T W IH1@67:400 NG K AH0@67:400 L \
L IH1@69:400 T AH0@69:400 L S T AA1@67:800 R _:150 \
HH AW1@65:400 AY1@65:400 W AH1@64:400 N D ER0@64:400 W AH1@62:400 T Y UW1@62:400 AA1@60:1000 R"
say twinkle-high -p 220 -f 1.15 -g 0.85 "T W IH1@72:400 NG K AH0@72:400 L T W IH1@79:400 NG K AH0@79:400 L \
L IH1@81:400 T AH0@81:400 L S T AA1@79:800 R _:150 \
HH AW1@77:400 AY1@77:400 W AH1@76:400 N D ER0@76:400 W AH1@74:400 T Y UW1@74:400 AA1@72:1000 R"

echo "Written to $OUT"
