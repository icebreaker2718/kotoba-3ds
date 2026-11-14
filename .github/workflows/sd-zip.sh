#!/bin/sh
# Builds Kotoba3DS-SD.zip: the plugin, the dictionary and the OCR models laid out as on the SD card,
#   luma/plugins/default.3gx
#   3ds/kotoba3ds/dict.bin
#   3ds/kotoba3ds/ocr/...            (everything in Kotoba3DS-ocr-model.zip)
# so one download, extracted at the card's root (3dsDL: Files -> the zip -> Extract -> Here), puts
# every file in place with no SD card reader.
# Usage: sd-zip.sh Kotoba3DS.3gx dict.bin Kotoba3DS-ocr-model.zip Kotoba3DS-SD.zip
set -eu
[ $# -eq 4 ] || { echo "usage: $0 plugin.3gx dict.bin ocr-model.zip out.zip" >&2; exit 2; }
out=$(realpath -m "$4")
d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT

mkdir -p "$d/luma/plugins" "$d/3ds/kotoba3ds"
cp "$1" "$d/luma/plugins/default.3gx"
cp "$2" "$d/3ds/kotoba3ds/dict.bin"
unzip -q "$3" -d "$d"
# the model zip must hold 3ds/kotoba3ds/ocr/ and nothing outside it, or the layout is wrong
test -f "$d/3ds/kotoba3ds/ocr/VOCR6.2_N_w8g2.vocr"
test -f "$d/3ds/kotoba3ds/ocr/textspot8_t64n8c16_w8.vtsp"
[ "$(ls -A "$d" | sort | tr '\n' ' ')" = "3ds luma " ]

rm -f "$out"
mkdir -p "$(dirname "$out")"
(cd "$d" && zip -qr -X "$out" luma 3ds)
unzip -l "$out"
