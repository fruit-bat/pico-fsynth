#!/bin/bash
#
# Set the synth's stereo width: how far apart left and right of the mix are.
#
# Usage: host/stereo_width.sh PERCENT [port]
#   PERCENT  0..100: 100 (default) = as the MIDI file pans it, 50 = hard
#            panned parts pulled halfway to the centre, 0 = mono
#   port     hw:2,0,0, fsynth-serial... (default: found by host/midi_port.sh)
#
# Sends SysEx F0 7D 46 53 03 ww F7 (7D = non-commercial ID, 46 53 = "FS",
# 03 = stereo width, ww = percent). Kept through GM/GS resets; lost at power
# off unless saved with host/settings.sh save. fsrender -W does the same.
#
# Example: host/stereo_width.sh 60 && host/settings.sh save
set -e
pct=${1:?usage: $0 PERCENT [port]}
. "$(dirname "$0")/midi_port.sh"
midi_find_port "$2"
[ "$pct" -lt 0 ] && pct=0
[ "$pct" -gt 100 ] && pct=100
msg=$(printf 'F0 7D 46 53 03 %02X F7' "$pct")
midi_send "$msg"
echo "$port: $msg (stereo width $pct%)"
