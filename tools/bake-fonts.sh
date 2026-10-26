#!/usr/bin/env bash
# ps5-homebrew-ui - Rebuild the baked SDF fonts in assets/fonts.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Each line of the table is: source, output name, pixel size, SDF range,
# atlas size, glyph set. The two last columns are optional and default to the
# 1024 `basic` set this repository ships; `2048 european` covers accented
# Latin, Greek and Cyrillic. The Chinese face is baked from the whole `cjk`
# set (kana, the full U+4E00..U+9FFF Han block, compatibility ideographs and
# the full-width forms) rather than a curated GB2312 table: bilingual
# Chinese-Japanese subtitles use traditional-form Han (語, 國, ...) that GB2312
# does not carry, and a missing code point draws as '?'. wqy-microhei.ttc holds
# the block (only ~300 of 20992 are absent), so the full set resolves them all.
# To add a typeface, drop its TTF and licence into third_party/fonts, add a
# line here, run `make fonts`, and load the new .huifont where the others are
# loaded (src/main.cpp and host/snapshot_main.cpp).

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cxx=$(command -v "${HOST_CXX:-clang++}")
mkdir -p "$root/build/host" "$root/assets/fonts"
"$cxx" -std=c++20 -O2 -w "$root/tools/font-baker/bake_font.cpp" -o "$root/build/host/bake_font"
while read -r source output size range atlas glyphs; do
    [[ -n $source && $source != \#* ]] || continue
    [[ -n $atlas ]] || atlas=1024
    [[ -n $glyphs ]] || glyphs=basic
    "$root/build/host/bake_font" "$root/third_party/fonts/$source" \
        "$root/assets/fonts/$output.huifont" "$size" "$range" "$atlas" "$glyphs"
done <<'FONTS'
Inter-Regular.ttf inter-regular 56 8
Inter-SemiBold.ttf inter-semibold 56 8
Montserrat-Medium.ttf montserrat-medium 56 8
DejaVuSansMono.ttf dejavu-sans-mono 52 8
PressStart2P-Regular.ttf press-start-2p 32 4
PatrickHand-Regular.ttf patrick-hand 56 8
# The Chinese face is baked a touch smaller than the Latin ones: 42 pixels at
# range 7 keeps the whole 22000-sign CJK set inside one 8192-wide atlas (the
# square is its own height limit). SDF text scales cleanly, so it is still
# crisp at the sizes this app draws at. The atlas is ~63 MB, so every reader
# of it raises its size cap well above the old 48 MiB (src/main.cpp,
# host/snapshot_main.cpp, tests/unit/font_test.cpp).
wqy-microhei.ttc wenquanyi-cjk 42 7 8192 cjk
FONTS
cp "$root"/third_party/fonts/*-LICENSE.txt "$root/assets/fonts/"
