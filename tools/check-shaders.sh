#!/usr/bin/env bash
# ps5-homebrew-ui - compile the embedded GLSL with glslangValidator.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Pulls the four shader sources out of src/gfx/gl_batch.cpp and compiles them
# the way gl_program.cpp does - with the #version line it prepends.
#
# A type error in the fragment shader does not fail the build: it fails on the
# console, as "fatal: renderer init failed" behind the splash screen, and costs
# a deploy round trip to find. This catches it in a second.
set -u

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root" || exit 1

if ! command -v glslangValidator >/dev/null 2>&1; then
    echo "glslangValidator not found; skipping shader check" >&2
    exit 0
fi

python3 - <<'PY'
import os
import re
import subprocess
import sys
import tempfile

src = open("src/gfx/gl_batch.cpp", encoding="utf-8").read()
program = open("src/gfx/gl_program.cpp", encoding="utf-8").read()

version = re.search(r'g_glsl_prefix\s*=\s*"([^"]*)"', program)
version = version.group(1).encode().decode("unicode_escape") if version else "#version 460 core"
if not version.endswith("\n"):
    version += "\n"

# constexpr const char *kName = R"(... )";
pattern = re.compile(r'constexpr const char \*(\w+)\s*=\s*R"\((.*?)\)";', re.S)
failed = 0
found = 0

for name, body in pattern.findall(src):
    stage = "vert" if "Vertex" in name else "frag"
    found += 1
    with tempfile.NamedTemporaryFile("w", suffix="." + stage, delete=False) as fh:
        fh.write(version + body)
        path = fh.name
    proc = subprocess.run(["glslangValidator", "-S", stage, path], capture_output=True, text=True)
    if proc.returncode != 0:
        failed += 1
        print("FAIL %s (%s)" % (name, stage))
        print(proc.stdout.rstrip())
        print(proc.stderr.rstrip())
    else:
        print("ok   %s (%s)" % (name, stage))
    os.unlink(path)

if not found:
    print("no shader sources found - did the embedding change?")
    sys.exit(1)
sys.exit(1 if failed else 0)
PY
