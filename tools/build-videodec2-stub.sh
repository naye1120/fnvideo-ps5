#!/usr/bin/env bash
# ps5-homebrew-ui - import stub for libSceVideodec2.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The payload SDK ships no stub for the hardware video decoder, so this builds
# one from tooling/native/stubs/videodec2.c: a shared object whose only export
# list is the ten sceVideodec2* names. The packaging converter turns each of
# them into an import of the console's own module (loaded at run time by
# src/fnvideo/hwdec_ps5.cpp). The bodies never run.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
sdk=${PS5_PAYLOAD_SDK:-$root/.deps/native/ps5-payload-sdk}
compiler=${PS5_CLANG:-$sdk/bin/prospero-clang}
source_file=$root/tooling/native/stubs/videodec2.c
output=$root/build/stubs/libSceVideodec2.so

if [[ ! -x $compiler ]]; then
    echo "build-videodec2-stub: $compiler not found; run make deps" >&2
    exit 2
fi

mkdir -p "$(dirname "$output")"
# -nostdlib: a stub imports nothing itself, so it must not drag in the default
# runtime libraries the PS5 driver would otherwise add.
PS5_PAYLOAD_SDK=$sdk PS5_CLANG=$compiler USE_CCACHE=0 \
    sh "$root/tooling/prospero-clang18" -shared -fPIC -nostdlib \
    -Wl,-soname,libSceVideodec2.sprx \
    -o "$output" "$source_file"

echo "==> [stub] $output"
