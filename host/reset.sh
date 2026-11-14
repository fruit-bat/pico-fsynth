#!/bin/bash
#
# Silence and reset the synth over MIDI, e.g. after stopping aplaymidi
# part way through a song (which leaves notes hanging).
#
# Usage: host/reset.sh [-g] [port]
#   (default)  All Sound Off + Reset All Controllers on all 16 channels:
#              stops every note at once and clears pedals, pitch bend,
#              modulation, volume and expression. Programs are kept.
#   -g         Also send GM System On: a full reset, programs back to piano.
#   port       hw:2,0,0, fsynth-serial... (default: found by host/midi_port.sh)
#
# Master volume, EQ and stereo width are device settings and are not changed.
#
# Bytes sent, per channel n (0..F): Bn 78 00 (CC120 All Sound Off) and
# Bn 79 00 (CC121 Reset All Controllers); with -g also F0 7E 7F 09 01 F7
# (GM System On: 7E = non-real-time, 7F = all devices, 09 01 = GM on).
#
# Works over USB MIDI or the host/serial_midi bridge (see host/midi_port.sh).
set -e
gm=false
if [ "$1" = -g ]; then gm=true; shift; fi
. "$(dirname "$0")/midi_port.sh"
midi_find_port "$1"

msg=""
for c in 0 1 2 3 4 5 6 7 8 9 A B C D E F; do
    msg+="B$c 78 00 B$c 79 00 "     # CC120 all sound off, CC121 reset all controllers
done
$gm && msg+="F0 7E 7F 09 01 F7"     # GM System On
midi_send "$msg"
echo "$port: all sound off, controllers reset$($gm && echo ', GM reset')"
