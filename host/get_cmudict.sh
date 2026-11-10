#!/bin/sh
#
# get_cmudict.sh [DIR] - download the CMU Pronouncing Dictionary for
# "fsspeak -t" (English text input) into DIR (default: build-host).
#
# The dictionary is BSD licensed (the licence is saved next to it) and is
# about 3.5 MB; it is not stored in this repository.
#
set -e
DIR=${1:-build-host}
URL=https://raw.githubusercontent.com/cmusphinx/cmudict/master
mkdir -p "$DIR"
curl -fsSL -o "$DIR/cmudict.dict" "$URL/cmudict.dict"
curl -fsSL -o "$DIR/cmudict.LICENSE" "$URL/LICENSE"
echo "$DIR/cmudict.dict: $(wc -l < "$DIR/cmudict.dict") words"
