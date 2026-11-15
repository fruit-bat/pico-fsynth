#!/bin/bash
#
# Set the synth's master volume (GM Master Volume SysEx).
#
# Usage: host/master_volume.sh PERCENT [port]
#   PERCENT  0..100 (linear amplitude; 50 is about -6 dB)
#   port     hw:2,0,0, fsynth-serial... (default: found by host/midi_port.sh)
#
# Sends the universal real-time SysEx F0 7F 7F 04 01 ll mm F7
#   7F = real-time, 7F = all devices, 04 01 = device control / master
#   volume, ll mm = 14-bit volume, low 7 bits first (16383 = full).
# The change is lost at power off unless saved with host/settings.sh save.
#
# Works over USB MIDI or the host/serial_midi bridge (see host/midi_port.sh).
set -e
pct=${1:?usage: $0 PERCENT [port]}
. "$(dirname "$0")/midi_port.sh"
midi_find_port "$2"
v=$(( pct * 16383 / 100 ))
[ $v -lt 0 ] && v=0
[ $v -gt 16383 ] && v=16383
msg=$(printf 'F0 7F 7F 04 01 %02X %02X F7' $(( v & 127 )) $(( v >> 7 )))
midi_send "$msg"
echo "$port: $msg (volume $pct%)"
