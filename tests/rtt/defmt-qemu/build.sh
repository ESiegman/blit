#!/bin/sh
# Builds tests/rtt/defmt-qemu.elf, the defmt firmware tests/rtt.sh runs on
# QEMU's mps2-an385 (Cortex-M3): defmt 1 over semihosting
# (defmt-semihosting), one line at each level, a target timestamp, then a
# semihosting exit. Needs rustup's thumbv7m-none-eabi target and Arm's
# objcopy; the paths in its debug information are made relative to here
# (and the crates' to /cargo), so that the ELF does not carry this machine's.
#
#   tests/rtt/defmt-qemu/build.sh
set -eu
here=$(cd "$(dirname "$0")" && pwd -P)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cargo_home=${CARGO_HOME:-$HOME/.cargo}
objcopy=$(command -v arm-none-eabi-objcopy || ls -d "$HOME"/.local/opt/arm-gnu-toolchain-*/bin/arm-none-eabi-objcopy | tail -1)
cd "$here"
RUSTFLAGS="--remap-path-prefix=$here=. --remap-path-prefix=$cargo_home=/cargo -C link-arg=--nmagic -C link-arg=-Tlink.x -C link-arg=-Tdefmt.x" \
    cargo build --release --target-dir "$out"
# (What defmt-print needs: the .defmt section and the symbols, and the
# debug information it finds each log's file and line in; not the rest.)
"$objcopy" -R .debug_frame -R .debug_aranges -R .debug_ranges -R .debug_rnglists -R .debug_loc -R .debug_loclists -R .comment \
    "$out/thumbv7m-none-eabi/release/fwdefmt" "$here/../defmt-qemu.elf"
