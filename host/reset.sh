#!/bin/bash
#
# Silence and reset the synth over USB MIDI, e.g. after stopping aplaymidi
# part way through a song (which leaves notes hanging).
#
# Usage: host/reset.sh [-g] [amidi-port]
#   (default)  All Sound Off + Reset All Controllers on all 16 channels:
#              stops every note at once and clears pedals, pitch bend,
#              modulation, volume and expression. Programs are kept.
#   -g         Also send GM System On: a full reset, programs back to piano.
#   port       e.g. hw:2,0,0 (default: the first port named pico-fsynth)
#
# Master volume and EQ are device settings and are not changed.
set -e
gm=false
if [ "$1" = -g ]; then gm=true; shift; fi
port=${1:-$(amidi -l | awk '/pico-fsynth/ { print $2; exit }')}
[ -n "$port" ] || { echo "pico-fsynth MIDI port not found (amidi -l)" >&2; exit 1; }

msg=""
for c in 0 1 2 3 4 5 6 7 8 9 A B C D E F; do
    msg+="B$c 78 00 B$c 79 00 "     # CC120 all sound off, CC121 reset all controllers
done
$gm && msg+="F0 7E 7F 09 01 F7"     # GM System On
amidi -p "$port" -S "$msg"
echo "$port: all sound off, controllers reset$($gm && echo ', GM reset')"
