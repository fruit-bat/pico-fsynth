#!/bin/bash
#
# Set the synth's master volume (GM Master Volume SysEx) over USB MIDI.
#
# Usage: host/master_volume.sh PERCENT [amidi-port]
#   PERCENT  0..100 (linear amplitude; 50 is about -6 dB)
#   port     e.g. hw:2,0,0 (default: the first port named pico-fsynth)
#
# Sends the universal real-time SysEx F0 7F 7F 04 01 ll mm F7
#   7F = real-time, 7F = all devices, 04 01 = device control / master
#   volume, ll mm = 14-bit volume, low 7 bits first (16383 = full).
# The change is lost at power off unless saved with host/settings.sh save.
#
# Requires amidi (alsa-utils) and the synth connected over USB.
set -e
pct=${1:?usage: $0 PERCENT [amidi-port]}
port=${2:-$(amidi -l | awk '/pico-fsynth/ { print $2; exit }')}
[ -n "$port" ] || { echo "pico-fsynth MIDI port not found (amidi -l)" >&2; exit 1; }
v=$(( pct * 16383 / 100 ))
[ $v -lt 0 ] && v=0
[ $v -gt 16383 ] && v=16383
msg=$(printf 'F0 7F 7F 04 01 %02X %02X F7' $(( v & 127 )) $(( v >> 7 )))
amidi -p "$port" -S "$msg"
echo "$port: $msg (volume $pct%)"
