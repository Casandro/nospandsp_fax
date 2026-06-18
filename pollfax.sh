#!/bin/bash
# pollfax.sh - send (or serve for polling) a fax, choosing a different source
# image per negotiated mode. The direction is taken from the transport:
#   --sip-dial / --connect   -> PLACE the call and transmit the fax now, sending
#                               the best version the receiver's DIS allows.
#   --sip-answer / --listen  -> SERVE a poll: wait to be polled and send the
#                               best version the polling caller's DTC allows.
# Either way sip_fax picks the best kind+resolution at phase B.
#
# Source images (overridable via the env vars in brackets), taken from
# $POLLFAX_IMGDIR (default: ./original_images, else <script>/../original_images):
#   default.png       bilevel at the 204-dpi resolutions   [POLLFAX_DEFAULT]
#                      (standard 204x98, fine 204x196, superfine 204x391)
#   300dpi_2.png      bilevel at 300 dpi (2592 wide)       [POLLFAX_300]
#   graustufen_2.png  greyscale T.81 JPEG                  [POLLFAX_GRAY]
#   farbe.png         full colour T.42 JPEG                [POLLFAX_COLOR]
# Each is scaled up to the required fax width and (for the bilevel modes)
# Floyd-Steinberg dithered to 1 bit. The chosen rendition therefore depends on
# what the polling caller can receive: colour > greyscale > 300 dpi > superfine
# > fine > standard.
#
# The six pages come pre-rendered in poll_pages/ (committed next to this
# script), so no rendering happens at run time. Point POLLFAX_PAGES at another
# directory of {standard,fine,superfine,300,color,gray}.tiff to serve those, or
# set POLLFAX_RENDER=1 to re-render from the source images above.
#
# Usage:
#   pollfax.sh <transport/SIP args for sip_fax...>
# send a fax:
#   pollfax.sh --sip-dial '**1' --user sip:wurstuser@192.168.5.8 --password pw
#   pollfax.sh --connect 127.0.0.1:5000                       # TCP, for testing
# serve a poll:
#   pollfax.sh --sip-answer --register --user sip:wurstuser@192.168.5.8 --password pw
#   pollfax.sh --listen 5000                                  # TCP, for testing
#
# POLLFAX_KEEP=1 keeps a freshly rendered temp directory (path printed).
set -u
DIR=$(cd "$(dirname "$0")" && pwd)
SIP_FAX=$DIR/sip_fax

if [ $# -lt 1 ]; then
    sed -n '2,36p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
fi

# Locate the image directory.
[ -x "$SIP_FAX" ] || { echo "error: $SIP_FAX not built (run make)" >&2; exit 2; }

# The six fax pages may be pre-rendered (committed in poll_pages/, or a dir
# given by POLLFAX_PAGES) - serve those directly; otherwise render them from
# the source images now. POLLFAX_RENDER=1 forces a fresh render.
PAGES=""
if [ "${POLLFAX_RENDER:-0}" != 1 ]; then
    if [ -n "${POLLFAX_PAGES:-}" ]; then PAGES=$POLLFAX_PAGES
    elif [ -d "$DIR/poll_pages" ]; then  PAGES=$DIR/poll_pages
    fi
fi

if [ -n "$PAGES" ]; then
    for p in standard fine superfine 300 color gray; do
        [ -r "$PAGES/$p.tiff" ] || { echo "error: pre-rendered $PAGES/$p.tiff missing "\
"(set POLLFAX_RENDER=1 to render from source)" >&2; exit 2; }
    done
    echo "serving pre-rendered pages from $PAGES" >&2
else
    # Render from the source images into a temp dir.
    if [ -n "${POLLFAX_IMGDIR:-}" ]; then IMGDIR=$POLLFAX_IMGDIR
    elif [ -d original_images ]; then     IMGDIR=original_images
    elif [ -d "$DIR/../original_images" ]; then IMGDIR=$DIR/../original_images
    else echo "error: cannot find image directory (set POLLFAX_IMGDIR)" >&2; exit 2; fi

    IMG_DEFAULT=${POLLFAX_DEFAULT:-$IMGDIR/default.png}
    IMG_300=${POLLFAX_300:-$IMGDIR/300dpi_2.png}
    IMG_GRAY=${POLLFAX_GRAY:-$IMGDIR/graustufen_2.png}
    IMG_COLOR=${POLLFAX_COLOR:-$IMGDIR/farbe.png}
    for f in "$IMG_DEFAULT" "$IMG_300" "$IMG_GRAY" "$IMG_COLOR"; do
        [ -r "$f" ] || { echo "error: cannot read source image $f" >&2; exit 2; }
    done
    command -v convert >/dev/null || { echo "error: ImageMagick (convert) not found" >&2; exit 2; }

    PAGES=$(mktemp -d /tmp/pollfax.XXXXXX)
    if [ "${POLLFAX_KEEP:-0}" = 1 ]; then echo "keeping rendered pages in $PAGES" >&2
    else trap 'rm -rf "$PAGES"' EXIT; fi

    # Bilevel: scale to the exact fax width, flatten transparency over white,
    # Floyd-Steinberg dither to 1 bit, Group-4, with the mode's resolution tags.
    bilevel() {  # <src> <width> <xdpi> <ydpi> <out>
        convert "$1" -resize "$2"x -background white -alpha remove \
                -colorspace Gray -dither FloydSteinberg -monochrome \
                -compress Group4 -density "$3"x"$4" -units PixelsPerInch "$5" \
            || { echo "error: render failed ($1 -> $5)" >&2; exit 1; }
    }

    echo "rendering poll alternatives from $IMGDIR ..." >&2
    bilevel "$IMG_DEFAULT" 1728 204 98  "$PAGES/standard.tiff"
    bilevel "$IMG_DEFAULT" 1728 204 196 "$PAGES/fine.tiff"
    bilevel "$IMG_DEFAULT" 1728 204 391 "$PAGES/superfine.tiff"
    bilevel "$IMG_300"     2592 300 300 "$PAGES/300.tiff"
    # Continuous-tone JPEG kinds at the T.42/T.81 fine/200 dpi anchor.
    convert "$IMG_GRAY"  -resize 1728x -background white -alpha remove \
            -colorspace Gray -type Grayscale -compress LZW \
            -density 200x200 -units PixelsPerInch "$PAGES/gray.tiff" \
        || { echo "error: render failed (greyscale)" >&2; exit 1; }
    convert "$IMG_COLOR" -resize 1728x -background white -alpha remove \
            -type TrueColor -compress LZW \
            -density 200x200 -units PixelsPerInch "$PAGES/color.tiff" \
        || { echo "error: render failed (colour)" >&2; exit 1; }
fi

# Same per-mode image set, two directions: dial/connect TRANSMITS the fax now
# (best version per the receiver's DIS); answer/listen SERVES a poll (best
# version per the polling caller's DTC). Pick by the transport in the args.
serve=1
for a in "$@"; do
    case "$a" in
        --sip-dial|--connect)  serve=0 ;;
        --sip-answer|--listen) serve=1 ;;
    esac
done
if [ "$serve" = 1 ]; then
    echo "waiting to be polled (answer mode)" >&2
    mode=(--poll-serve)
else
    echo "placing the call and sending the best version the receiver supports" >&2
    mode=()
fi

"$SIP_FAX" "${mode[@]}" \
    --send-alt standard:"$PAGES/standard.tiff" \
    --send-alt fine:"$PAGES/fine.tiff" \
    --send-alt superfine:"$PAGES/superfine.tiff" \
    --send-alt 300:"$PAGES/300.tiff" \
    --send-color "$PAGES/color.tiff" \
    --send-gray "$PAGES/gray.tiff" \
    "$@"
exit $?
