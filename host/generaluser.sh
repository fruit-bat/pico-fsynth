# shellcheck shell=bash
#
# generaluser.sh - download GeneralUser GS 2.0.3 by S. Christian Collins
# (https://www.schristiancollins.com/generaluser), pinned to a commit of its
# GitHub repository and checked against its SHA-256. Sourced by
# host/make_uf2s.sh and host/make_p4_bins.sh.
#
#   generaluser_get SF2 LICENCE_DIR
#     downloads the font to SF2 unless it is already there (and correct),
#     and its licence to LICENCE_DIR/GeneralUser-GS-LICENSE.txt

gu_url=https://raw.githubusercontent.com/mrbumpy409/GeneralUser-GS/684543d5e5efaef08d02be50dcda8d552478fa60
gu_sha256=9575028c7a1f589f5770fccc8cff2734566af40cd26ed836944e9a5152688cfe

generaluser_get() {
    local sf2=$1 dir=$2
    if ! echo "$gu_sha256  $sf2" | sha256sum -c --status 2>/dev/null; then
        mkdir -p "$(dirname "$sf2")"
        curl -fsSL -o "$sf2" "$gu_url/GeneralUser-GS.sf2"
        echo "$gu_sha256  $sf2" | sha256sum -c --quiet
    fi
    mkdir -p "$dir"
    [ -f "$dir/GeneralUser-GS-LICENSE.txt" ] ||
        curl -fsSL -o "$dir/GeneralUser-GS-LICENSE.txt" "$gu_url/documentation/LICENSE.txt"
}
