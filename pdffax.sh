#!/bin/bash
# pdffax.sh - fax a PDF, sending the highest-quality version the receiver
# supports.
#
# The PDF is rendered with Ghostscript into a full set of alternatives:
#   - colour JPEG     (T.30 Annex E / T.42, RGB, 200 dpi)
#   - greyscale JPEG  (T.81 single component, 200 dpi)
#   - bilevel Group-4 at 300 dpi, superfine (204x391), fine (204x196) and
#     standard (204x98)
# and all of them are offered in ONE call; sip_fax picks the best kind and
# resolution from the receiver's DIS at negotiation time
# (colour > greyscale > 300 dpi > superfine > fine > standard).
#
# Usage:
#   pdffax.sh <file.pdf> <transport/SIP args for sip_fax...>
# e.g.
#   pdffax.sh doc.pdf --sip-dial '**1' --user sip:user@host --password pw
#   pdffax.sh doc.pdf --connect 127.0.0.1:5000
#
# Extra sip_fax options pass straight through; notably --require-color makes
# the call fail (DCN) instead of falling back when the receiver cannot take
# colour.
#
# Polling: add --poll-serve (with an answering transport) to make the rendered
# document available for a caller to PULL, sending the best version the caller
# supports:
#   pdffax.sh doc.pdf --poll-serve --sip-answer --user sip:fax@host
#   pdffax.sh doc.pdf --poll-serve --listen 5000        # TCP
# A caller pulls it with:  sip_fax --poll --receive out.tiff --sip-dial ...
#
# PDFFAX_KEEP=1 keeps the rendered temp directory (path printed).
set -u
DIR=$(cd "$(dirname "$0")" && pwd)
SIP_FAX=$DIR/sip_fax

if [ $# -lt 2 ]; then
    sed -n '2,29p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
fi
PDF=$1; shift

[ -r "$PDF" ] || { echo "error: cannot read $PDF" >&2; exit 2; }
head -c 5 "$PDF" | grep -q '^%PDF-' || { echo "error: $PDF is not a PDF" >&2; exit 2; }
command -v gs >/dev/null || { echo "error: ghostscript (gs) not found" >&2; exit 2; }
[ -x "$SIP_FAX" ] || { echo "error: $SIP_FAX not built (run make)" >&2; exit 2; }

TMP=$(mktemp -d /tmp/pdffax.XXXXXX)
if [ "${PDFFAX_KEEP:-0}" = 1 ]; then
    echo "keeping rendered pages in $TMP" >&2
else
    trap 'rm -rf "$TMP"' EXIT
fi

render() {  # <device> <resolution> <outfile> [extra gs args...]
    local dev=$1 res=$2 out=$3; shift 3
    gs -dBATCH -dNOPAUSE -dSAFER -dQUIET \
       -sDEVICE="$dev" -r"$res" -sPAPERSIZE=a4 -dPDFFitPage "$@" \
       -sOutputFile="$out" "$PDF" \
        || { echo "error: ghostscript failed ($dev @ $res)" >&2; exit 1; }
}

echo "rendering $PDF ..." >&2
# bilevel Group-4; AdjustWidth snaps the 204 dpi modes to 1728, but the
# 300 dpi mode needs its 2592-pixel line width forced explicitly
render tiffg4   204x98   "$TMP/standard.tiff"
render tiffg4   204x196  "$TMP/fine.tiff"
render tiffg4   204x391  "$TMP/superfine.tiff"
render tiffg4   300x300  "$TMP/300.tiff" -g2592x3508 -dPDFFitPage
# continuous-tone, at the T.42 fine/200 dpi anchor
render tiff24nc 200x200  "$TMP/color.tiff"
render tiffgray 200x200  "$TMP/gray.tiff"

"$SIP_FAX" \
    --send-alt standard:"$TMP/standard.tiff" \
    --send-alt fine:"$TMP/fine.tiff" \
    --send-alt superfine:"$TMP/superfine.tiff" \
    --send-alt 300:"$TMP/300.tiff" \
    --send-color "$TMP/color.tiff" \
    --send-gray "$TMP/gray.tiff" \
    "$@"
exit $?
