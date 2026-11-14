#!/bin/bash
#
# Save the synth's current settings (master volume, EQ and stereo width) to its flash, so
# they are loaded at power up, or erase them (back to defaults).
#
# Usage: host/settings.sh save|erase [port]
#   port  hw:2,0,0, fsynth-serial... (default: found by host/midi_port.sh)
#
# Sends SysEx F0 7D 46 53 01 F7 (save) or F0 7D 46 53 02 F7 (erase):
# 7D = manufacturer ID reserved for non-commercial use, 46 53 = "FS". Audio
# drops out for a moment while the flash is written. The result is printed
# on the synth's USB serial console, not here. Saving is refused if the
# font reaches the last flash sector, where settings are kept.
#
# Works over USB MIDI or the host/serial_midi bridge (see host/midi_port.sh).
#
# Example: host/master_eq.sh 6 -6 && host/master_volume.sh 80 && host/settings.sh save
set -e
case "$1" in
    save) cmd=01 ;;
    erase) cmd=02 ;;
    *) echo "usage: $0 save|erase [port]" >&2; exit 1 ;;
esac
. "$(dirname "$0")/midi_port.sh"
midi_find_port "$2"
midi_send "F0 7D 46 53 $cmd F7"
echo "$port: settings $1 sent (the console confirms)"
