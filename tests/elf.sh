#!/bin/sh
# The ELF reader and address to source (src/elf.*, src/addrsrc.*), through
# blit_elf_test beside blit, against binutils' addr2line:
# - the two fixtures in tests/elf, built from its sources by Arm GNU 15.2
#   (motor-m0.elf: Cortex-M0+, little-endian, DWARF 5, a build id;
#   motor-m0-be-dwarf4-zlib.elf: big-endian, DWARF 4, its debug sections
#   compressed with zlib; both with -fdebug-prefix-map=...=/blit-fixture),
#   whose answers are written here;
# - where arm-none-eabi-gcc and its addr2line are installed: the sources
#   built with DWARF 2, 3, 4 and 5, in both byte orders, compressed and not,
#   every function and hundreds of addresses in their code answered as
#   `addr2line -pfiaC` answers them;
# - where this machine's cc makes ELFs (Linux): the same for a host build,
#   and for blit itself (ELF64, DWARF 5, C++: names demangled, inlined
#   frames, thousands of units);
# - each ELF cut short and damaged, read without a fault.
#
#   tests/elf.sh path/to/blit
set -u
blit=${1:?usage: tests/elf.sh path/to/blit}
src=$(cd "$(dirname "$0")/.." && pwd -P)
bin="$(cd "$(dirname "$blit")" && pwd)/blit_elf_test"
[ -x "$bin" ] || { echo "FAIL: no $bin"; exit 1; }
real_home=$HOME
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
failed=0
check() {
    if [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$2" "$got"
        failed=1
    fi
}
F="$src/tests/elf"

got=$("$bin" info "$F/motor-m0.elf" | tr '\n' '|')
check "the fixture's header, sections, symbols and build id" \
    "ELF32 little-endian ARM, type 2, entry 0x803d, 19 sections, 3 segments, 14 symbols, build id f0fc20d25fc8ac728e352efec955db584f2a7aa1|2 units|"
got=$("$bin" lookup "$F/motor-m0.elf" 0x8044 0x8052 0x8028 0x0 0x9999 | tr '\n' '|')
check "its addresses: an inlined function's frame, then the function it was inlined into; nothing outside its sections" \
    "0x00008044: store at /blit-fixture/motor.c:9| (inlined by) pid_step at /blit-fixture/motor.c:15|0x00008052: motor_run at /blit-fixture/motor.c:22|0x00008028: main at /blit-fixture/main.c:14|0x00000000: ?? ??:0|0x00009999: ?? ??:0|"
got=$("$bin" info "$F/motor-m0-be-dwarf4-zlib.elf" | tr '\n' '|')
check "big-endian, DWARF 4, compressed" "ELF32 big-endian ARM, type 2, entry 0x8019, 18 sections, 2 segments, 14 symbols, build id none|2 units|"
got=$("$bin" lookup "$F/motor-m0-be-dwarf4-zlib.elf" 0x8020 0x802e 0x8004 | tr '\n' '|')
check "its addresses" \
    "0x00008020: store at /blit-fixture/motor.c:9| (inlined by) pid_step at /blit-fixture/motor.c:15|0x0000802e: motor_run at /blit-fixture/motor.c:22|0x00008004: main at /blit-fixture/main.c:14|"
printf 'not an ELF\n' >"$dir/text"
got=$("$bin" info "$dir/text" 2>&1)
check "a file that is not an ELF" "$dir/text: not an ELF"
head -c 40 "$F/motor-m0.elf" >"$dir/cut"
got=$("$bin" info "$dir/cut" 2>&1)
check "one cut short in its header" "$dir/cut: not an ELF this reads (its class or byte order)"

# same ELF ADDR2LINE NAME: every function and N addresses in its code, as
# addr2line answers them (its discriminators aside). An address answered
# otherwise is asked of addr2line again on its own, twice, and its second
# answer taken: for a C++ function the debug information gives no linkage
# name, binutils' first answer names the code with the symbol table's
# function around it (an inlined helper's frame named after the function it
# was inlined into), then keeps the debug information's name unless that
# symbol starts where the function's code does, so that what it says
# depends on what was asked before. The reader gives that second answer,
# whatever was asked before.
blocks() { awk '/^0x/ && NR > 1 { print "" } { printf "%s~", $0 } END { print "" }'; }
same() {
    "$bin" addrs "$1" 400 7 >"$dir/addrs"
    "$bin" lookup "$1" <"$dir/addrs" | blocks >"$dir/ours"
    "$2" -pfiaC -e "$1" <"$dir/addrs" | sed -E 's/ \(discriminator [0-9]+\)//' | blocks >"$dir/theirs"
    paste -d '\n' "$dir/ours" "$dir/theirs" | awk 'NR % 2 { a = $0; next } a != $0 { split(a, w, ":"); print w[1] }' >"$dir/differ"
    alone=0 wrong=0
    while read -r a; do
        alone=$((alone + 1))
        o=$("$bin" lookup "$1" "$a")
        t=$(printf '%s\n%s\n' "$a" "$a" | "$2" -pfiaC -e "$1" | sed -E 's/ \(discriminator [0-9]+\)//' | awk '/^0x/ { n++ } n == 2')
        if [ "$o" != "$t" ]; then
            wrong=$((wrong + 1))
            [ "$wrong" -le 3 ] && printf '    ours:   %s\n    theirs: %s\n' "$o" "$t"
        fi
    done <"$dir/differ"
    n=$(wc -l <"$dir/addrs" | tr -d ' ')
    if [ "$wrong" != 0 ]; then
        echo "FAIL: $3: $wrong of $n addresses not as $(basename "$2") answers them"
        failed=1
    elif [ -n "${ELF_SAY:-}" ]; then
        echo "$3: $n addresses as $(basename "$2") answers them ($alone asked again on their own)"
    fi
}
damaged() {
    "$bin" damage "$1" "$2" 3 >/dev/null || { echo "FAIL: $3 cut short and damaged"; failed=1; }
}
damaged "$F/motor-m0.elf" 3000 "the fixture"
damaged "$F/motor-m0-be-dwarf4-zlib.elf" 3000 "the compressed fixture"

# Arm's toolchain: every DWARF version, both byte orders, compressed or not.
gcc=$(command -v arm-none-eabi-gcc 2>/dev/null || ls -d "$real_home"/.local/opt/arm-gnu-toolchain-*/bin/arm-none-eabi-gcc 2>/dev/null | head -1)
if [ -n "$gcc" ] && [ -x "${gcc%gcc}addr2line" ]; then
    mkdir -p "$dir/arm" && cp "$F"/*.c "$F"/*.h "$dir/arm/"
    for v in 2 3 4 5; do
        for e in little big; do
            for z in none zlib; do
                out="$dir/arm/m0-$e-dwarf$v-$z.elf"
                (cd "$dir/arm" && "$gcc" -mcpu=cortex-m0plus -mthumb -m$e-endian -O2 -gdwarf-$v -gz=$z -nostdlib -e _start main.c motor.c ring.c -o "$out") || failed=1
                same "$out" "${gcc%gcc}addr2line" "DWARF $v, $e-endian, compressed: $z"
            done
        done
    done
    # LTO and no debug information: the local functions' FILE symbol has no
    # name (the partition's), a file binutils writes as nothing (" at :?"),
    # the global ones none (" at ??:?").
    (cd "$dir/arm" && "$gcc" -mcpu=cortex-m0plus -mthumb -O2 -flto -nostdlib -e _start main.c motor.c ring.c -o lto.elf) || failed=1
    same "$dir/arm/lto.elf" "${gcc%gcc}addr2line" "LTO, no debug information"
else
    echo "SKIP: no arm-none-eabi-gcc and addr2line here: the reader is not checked against Arm's builds"
fi

# This machine's compiler, where it makes ELFs, and blit itself.
if [ "$(uname -s)" = Linux ] && command -v cc >/dev/null && command -v addr2line >/dev/null; then
    mkdir -p "$dir/host" && cp "$F"/*.c "$F"/*.h "$dir/host/"
    for v in 4 5; do
        (cd "$dir/host" && cc -O2 -gdwarf-$v -nostdlib -static -no-pie -fno-stack-protector -e _start main.c motor.c ring.c -o "host-dwarf$v.elf") || failed=1
        same "$dir/host/host-dwarf$v.elf" addr2line "this machine's cc, DWARF $v"
    done
    same "$blit" addr2line "blit itself"
    damaged "$dir/host/host-dwarf5.elf" 2000 "this machine's build"
else
    echo "SKIP: this machine's programs are not ELFs (or it has no addr2line): blit and a host build are not checked"
fi

[ "$failed" = 0 ] && echo "elf: all passed"
exit "$failed"
