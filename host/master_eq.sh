#!/bin/bash
#
# Set the synth's master EQ (Roland GS master EQ SysEx).
#
# Usage: host/master_eq.sh BASS_DB TREBLE_DB [BASS_HZ TREBLE_HZ] [port]
#   BASS_DB, TREBLE_DB  -12..12 (whole dB), 0 0 = flat
#   BASS_HZ             200 or 400 (default 200), shelf corner
#   TREBLE_HZ           3000 or 6000 (default 3000), shelf corner
#   port                hw:2,0,0, fsynth-serial... (default: found by host/midi_port.sh)
#
# Example: host/master_eq.sh 0 -4      (4 dB less treble above 3 kHz)
#
# Sends four Roland GS "data set" (DT1) messages, one per EQ parameter:
#   F0 41 10 42 12 40 02 0p vv cs F7
#   41 = Roland, 10 = device ID, 42 = GS model, 12 = DT1 command,
#   40 02 0p = address (p: 0 low freq, 1 low gain, 2 high freq, 3 high gain),
#   vv = value (freq: 0/1 = 200/400 Hz or 3/6 kHz; gain: 0x40 + dB),
#   cs = Roland checksum, so address + value + checksum = 0 mod 128.
# The synth applies the change at once; it is lost at power off unless
# saved with host/settings.sh save.
#
# Works over USB MIDI or the host/serial_midi bridge (see host/midi_port.sh).
set -e
bass=${1:?usage: $0 BASS_DB TREBLE_DB [BASS_HZ TREBLE_HZ] [port]}
treble=${2:?usage: $0 BASS_DB TREBLE_DB [BASS_HZ TREBLE_HZ] [port]}
bass_hz=${3:-200}
treble_hz=${4:-3000}
. "$(dirname "$0")/midi_port.sh"
midi_find_port "$5"

# Print a whole number of dB limited to the GS range -12..12
clamp() { local v=$1; [ $v -lt -12 ] && v=-12; [ $v -gt 12 ] && v=12; echo $v; }
gs() {  # address low byte, value: one GS DT1 message to 40 02 xx
    local sum=$(( (0x40 + 0x02 + $1 + $2) & 0x7f ))
    printf 'F0 41 10 42 12 40 02 %02X %02X %02X F7 ' $1 $2 $(( (128 - sum) & 0x7f ))
}
msg="$(gs 0 $([ "$bass_hz" -ge 400 ] && echo 1 || echo 0))"
msg+="$(gs 1 $(( 0x40 + $(clamp $bass) )))"
msg+="$(gs 2 $([ "$treble_hz" -ge 6000 ] && echo 1 || echo 0))"
msg+="$(gs 3 $(( 0x40 + $(clamp $treble) )))"
midi_send "$msg"
echo "$port: bass $bass dB at $bass_hz Hz, treble $treble dB at $treble_hz Hz"
