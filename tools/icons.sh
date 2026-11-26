#!/bin/sh
# Regenerate the 24px alpha masks from Heroicons 2.2.0.
set -eu
temp=$(mktemp -d)
trap 'rm -rf "$temp"' EXIT
out="$(dirname "$0")/../assets/icons.alpha"
for icon in computer-desktop cog-6-tooth arrow-left arrows-right-left chevron-left chevron-right play check magnifying-glass folder-open photo; do
    curl -fsSL --retry 3 "https://raw.githubusercontent.com/tailwindlabs/heroicons/v2.2.0/optimized/24/outline/$icon.svg" -o "$temp/$icon.svg"
    rsvg-convert -w 24 -h 24 "$temp/$icon.svg" | magick png:- -alpha extract -depth 8 gray:- >> "$temp/icons"
done
test "$(wc -c < "$temp/icons")" -eq 6336
cp "$temp/icons" "$out"
