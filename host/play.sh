#!/bin/bash
#
# Play MIDI files on the synth, each from a clean state.
#
# Usage: host/play.sh [-g] [-p port] song.mid...
#   -g        Also send GM System On before each file (programs back to piano).
#   -p port   An ALSA sequencer port: pico-fsynth, fsynth-serial, 128:0...
#             Default: the pico-fsynth USB MIDI device if connected, else the
#             fsynth-serial bridge (host/serial_midi) if it is running.
#
# Before each file it sends what host/reset.sh sends (All Sound Off + Reset
# All Controllers on all 16 channels). Most MIDI files assume a freshly reset
# synth and never clear the mod wheel, pitch bend, expression or pedals, so
# without this a file inherits whatever the previous one left behind, e.g.
# heavy vibrato from a leftover mod wheel. Ctrl+C stops playback and resets
# again, so no notes are left hanging.
#
# Needs aplaymidi and aseqsend (alsa-utils, aseqsend 1.2.11 or later).
set -e
gm=false
port=
while getopts gp: opt; do
    case $opt in
        g) gm=true ;;
        p) port=$OPTARG ;;
        *) sed -n '5,10s/^# \?//p' "$0" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
if [ $# -eq 0 ]; then sed -n '5,10s/^# \?//p' "$0" >&2; exit 2; fi

. "$(dirname "$0")/midi_port.sh"

# aplaymidi needs a sequencer port, so look for the USB device by its
# sequencer client name rather than its raw (hw:) device.
if [ -z "$port" ]; then
    for p in pico-fsynth fsynth-serial; do
        if aplaymidi -l 2>/dev/null | awk -v p="$p" '$2 == p { f = 1 } END { exit !f }'; then
            port=$p
            break
        fi
    done
fi
if [ -z "$port" ]; then
    echo "synth not found: no pico-fsynth USB MIDI device and no fsynth-serial bridge (aplaymidi -l)" >&2
    exit 1
fi

reset_synth() {
    local msg=""
    for c in 0 1 2 3 4 5 6 7 8 9 A B C D E F; do
        msg+="B$c 78 00 B$c 79 00 "     # CC120 all sound off, CC121 reset all controllers
    done
    $gm && msg+="F0 7E 7F 09 01 F7"     # GM System On
    midi_send "$msg"
}

trap 'reset_synth; echo; exit 130' INT TERM

for f in "$@"; do
    reset_synth
    $gm && sleep 0.1                    # give the GM reset time to settle
    echo "$port: $f"
    aplaymidi -p "$port" "$f"
done
