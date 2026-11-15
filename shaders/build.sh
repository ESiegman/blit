#!/bin/sh
# Compiles the GLSL sources for each GPU backend (src/gpu.h):
#   SPIR-V for Vulkan, MSL source for Metal, and compiled Metal libraries
#   (.metallib) when Xcode's Metal compiler is there.
# Results land in shaders/gen and are checked in, so a normal build of blit
# needs none of these tools. A build of blit uses the .metallib files when
# they are there, and compiles the MSL when it starts otherwise.
#
# Needs: one of glslangValidator / glslc / qsb, plus spirv-cross; for
# .metallib, Xcode (xcrun metal; in newer Xcodes the Metal toolchain is a
# separate download: xcodebuild -downloadComponent MetalToolchain).
set -eu
cd "$(dirname "$0")"
# Xcode's Metal compiler, even when the system's developer folder is the
# Command Line Tools (which have none).
if ! xcrun -sdk macosx metal --version >/dev/null 2>&1 && [ -d /Applications/Xcode.app/Contents/Developer ]; then
    export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
fi
mkdir -p gen
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

compile_spv() { # stage, source, output
    if command -v glslangValidator >/dev/null 2>&1; then
        glslangValidator -V -S "$1" -o "$3" "$2" >/dev/null
    elif command -v glslc >/dev/null 2>&1; then
        glslc -fshader-stage="$1" -O -o "$3" "$2"
    else
        qsb=$(command -v qsb || echo /opt/homebrew/opt/qtshadertools/bin/qsb)
        "$qsb" -s -o "$tmp/pack.qsb" "$2"
        "$qsb" -s -x spirv,100 -o "$3" "$tmp/pack.qsb"
    fi
}

# grid.vert draws the full-screen triangle for both fragment shaders:
# grid.frag is the text view, popup.frag the boxes drawn over it.
# The fragment shaders are each built again with BLIT_FX defined, and the
# text view's once more with BLIT_DIM as well: grid_fx.frag, grid_dim.frag
# and popup_fx.frag are made here from the one source of each.
for shader in grid.frag popup.frag; do
    awk 'NR == 1 { print; print "#define BLIT_FX 1"; next } { print }' "$shader" >"${shader%.frag}_fx.frag"
done
awk 'NR == 1 { print; print "#define BLIT_FX 1"; print "#define BLIT_DIM 1"; next } { print }' grid.frag >grid_dim.frag
trap 'rm -rf "$tmp" grid_fx.frag grid_dim.frag popup_fx.frag' EXIT
for shader in grid.vert grid.frag popup.frag grid_fx.frag grid_dim.frag popup_fx.frag; do
    stage=${shader##*.}
    compile_spv "$stage" "$shader" "gen/$shader.spv"
    spirv-cross --msl --msl-version 20100 --msl-decoration-binding \
        --output "gen/$shader.msl" "gen/$shader.spv"
    if xcrun -sdk macosx metal --version >/dev/null 2>&1; then
        xcrun -sdk macosx metal -x metal -c "gen/$shader.msl" -o "$tmp/$shader.air"
        xcrun -sdk macosx metallib "$tmp/$shader.air" -o "gen/$shader.metallib"
    fi
done
ls -l gen
