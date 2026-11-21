#!/bin/sh
# Generates src/emoji_font.c: the emoji in Google's monochrome Noto Emoji font (OFL) as a 20 px,
# 2 bpp LVGL font. ui.cpp makes it the fallback of the Montserrat fonts that show names and messages
# from other nodes, so emoji take the text color. One size and 2 bpp keep it to about 100 KB of
# flash, which suits every text size from 18 to 32 px; color emoji would not fit in the app partition.
# Needs uv (for fontTools) and npx (for lv_font_conv).
#
#     tools/make_emoji_font.sh
set -eu

APP=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

curl -sSL -o "$TMP/NotoEmoji.ttf" "https://github.com/google/fonts/raw/main/ofl/notoemoji/NotoEmoji%5Bwght%5D.ttf"
# The variable font's default weight is too thin at this size; render the bold weight.
uv run -q --with fonttools python -c "
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer
instancer.instantiateVariableFont(TTFont('$TMP/NotoEmoji.ttf'), {'wght': 700}).save('$TMP/NotoEmoji-700.ttf')
"
# U+2000 and up: everything in the font but ASCII digits, '#', '*', (c) and (R), which Montserrat has.
npx -y lv_font_conv@1.5.3 --font "$TMP/NotoEmoji-700.ttf" -r 0x2000-0x1FFFF --size 20 --bpp 2 --no-compress \
    --format lvgl --lv-include lvgl.h -o "$APP/src/emoji_font.c"
sed -i "s|$TMP/||; s|$APP/||" "$APP/src/emoji_font.c"  # keep temporary paths out of its Opts line
