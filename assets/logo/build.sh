#!/usr/bin/env sh
#
# Rasterize the Hermit logo set from the SVG sources.
#
#   ./build.sh
#
# Requires rsvg-convert (librsvg) and ImageMagick 7 (`magick`). The SVGs are the
# source of truth; every PNG and the .ico are generated. Nothing here is edited
# by hand.
#
# Two tiers: the full mark down to 48px, the simplified mark at 32 and 16, where
# the crab's legs and eyestalks are sub-pixel and turn to mush.
set -eu
cd "$(dirname "$0")"

command -v rsvg-convert >/dev/null || { echo "need rsvg-convert" >&2; exit 1; }
command -v magick       >/dev/null || { echo "need ImageMagick 7" >&2; exit 1; }

r() { rsvg-convert -w "$2" -h "$2" "$1" -o "$3"; }

r hermit.svg       2048 hermit-master.png
r hermit-alpha.svg 2048 hermit-transparent.png

for s in 512 256 128 64 48; do r hermit.svg "$s" "hermit-icon-$s.png"; done
for s in 32 16;             do r hermit-small.svg "$s" "hermit-icon-$s.png"; done
for s in 512 256 128;       do r hermit-alpha.svg "$s" "hermit-icon-$s-alpha.png"; done

r hermit-badge.svg 512 hermit-badge-512.png
r hermit.svg       320 hermit-readme-320.png
r hermit.svg       200 hermit-readme-200.png

magick hermit-icon-16.png hermit-icon-32.png hermit-icon-48.png favicon.ico

# rsvg and ImageMagick both stamp their own metadata into the output. Strip it:
# this repo's own rule is that nothing ships carrying provenance it did not ask
# for. aiscrub (Jeagermeister/aiscrub) is the tool that does it, looked for in
# its two usual homes:
for c in "$HOME/Source/aiscrub/aiscrub.py" \
         "$HOME/.claude/skills/scrub-ai-metadata/scripts/aiscrub.py"; do
    [ -r "$c" ] && { python3 "$c" strip -i ./*.png ./favicon.ico >/dev/null; break; }
done

echo "built:"
ls -1 ./*.png ./favicon.ico
