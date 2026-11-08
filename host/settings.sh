#!/bin/bash
#
# Save the synth's current settings (master volume and EQ) to its flash, so
# they are loaded at power up, or erase them (back to defaults).
#
# Usage: host/settings.sh save|erase [amidi-port]
#   port  e.g. hw:2,0,0 (default: the first port named pico-fsynth)
#
# Sends SysEx F0 7D 46 53 01 F7 (save) or F0 7D 46 53 02 F7 (erase). Audio
# drops out for a moment while the flash is written.
#
# Example: host/master_eq.sh 6 -6 && host/master_volume.sh 80 && host/settings.sh save
set -e
case "$1" in
    save) cmd=01 ;;
    erase) cmd=02 ;;
    *) echo "usage: $0 save|erase [amidi-port]" >&2; exit 1 ;;
esac
port=${2:-$(amidi -l | awk '/pico-fsynth/ { print $2; exit }')}
[ -n "$port" ] || { echo "pico-fsynth MIDI port not found (amidi -l)" >&2; exit 1; }
amidi -p "$port" -S "F0 7D 46 53 $cmd F7"
echo "$port: settings $1 sent (the console confirms)"
