#!/bin/sh
# Languages, through the editor: which language a file is (its ending, its
# whole name or a pattern of names), and what init.lua can say of one (a
# `parser` alias, `comment`, `filenames`, a display `name`, and a server's
# `language_id`). It runs with a scratch data folder: no grammars.
#
#   tests/langs.sh path/to/blit            run the cases
#   tests/langs.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/langs.sh path/to/blit [--print]}
print=${2:-}
fake="$(cd "$(dirname "$blit")" && pwd)/blit_fakelsp"
[ -x "$fake" ] || { echo "langs.sh: no $fake"; exit 1; }
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
W="$dir/work"
mkdir -p "$W" "$dir/data" "$dir/bin"
off='blit.opt.git.enabled = false
blit.opt.editor.restore = false'
printf '%s\nblit.opt.lsp.enabled = false\nblit.opt.editor.modal = false\n' "$off" >"$dir/init.lua"
export BLIT_RECENT_FILE="$dir/recent"

failed=0
check() {
    if [ -n "$print" ]; then
        printf '%s\n    %s\n' "$1" "$got"
    elif [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$2" "$got"
        failed=1
    fi
}
# run SCRIPT FILE: the last line the run printed, in a scratch data folder
# (no grammars). INIT, if set, is the init.lua for this run only.
INIT=
run() {
    got=$(XDG_DATA_HOME="$dir/data" BLIT_INIT="${INIT:-$dir/init.lua}" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" --script "workdir:$W;frame;$1" "$2" 2>&1 >/dev/null |
        sed -e "s#$dir#~#g" -e 's/^blit: //' | tail -1)
    INIT=
}
# language FILE: the language blit takes the file to be ("" for plain text).
language() { run "lua:print('[' .. (blit.info(0).language or '') .. ']')" "$1"; }

# ---------------------------------------------------------------- what init.lua can say

printf 'SECTIONS\n{\n  .text : { *(.text*) } > FLASH\n}\n' >"$W/link.ld"
printf 'int x;\n' >"$W/main.c"
printf 'config FOO\n\tbool "foo"\n' >"$W/Kconfig"
printf 'CONFIG_FOO=y\n' >"$W/board_defconfig"
printf 'CONFIG_FOO=y\n' >"$W/defconfig.txt"
printf '%s\nblit.opt.lsp.enabled = false\nblit.opt.editor.modal = false\n%s\n' "$off" \
    'blit.opt.syntax.grammars.ldc = { parser = "c", extensions = { "ld" }, name = "Linker script (C)" }
blit.opt.syntax.grammars.kc = { filenames = { "Kconfig", "*_defconfig" }, comment = "#" }
blit.opt.syntax.grammars.blk = { parser = "c", extensions = { "blk" }, comment = "/* %s */" }' >"$dir/own.lua"

INIT="$dir/own.lua" language "$W/Kconfig"
check "filenames: a whole name" '[kc]'
INIT="$dir/own.lua" language "$W/board_defconfig"
check "filenames: a pattern of names" '[kc]'
INIT="$dir/own.lua" language "$W/defconfig.txt"
check "a name the pattern does not fit is not that language" '[]'
INIT="$dir/own.lua" run "statusdump" "$W/link.ld"
check "an alias's display name in the status bar" 'link.ld | Ln 1, Col 1 Spaces: 2 LF Linker script (C) (no colours)'
INIT="$dir/own.lua" run "statusclick:C;text:linker script (c);key:Return;frame;statusdump" "$W/main.c"
check "and in the list of languages, to choose for a file" 'main.c | Ln 1, Col 1 Tabs LF Linker script (C) (no colours)'
INIT="$dir/own.lua" run "statusdump" "$W/Kconfig"
check "a language with no name of its own: its key, with a capital" 'Kconfig | Ln 1, Col 1 Tabs LF Kc (no colours)'

# gc: the language's comment from init.lua, both ends of it, or what the
# language whose grammar an alias uses has.
printf '%s\nblit.opt.lsp.enabled = false\nblit.opt.editor.modal = true\n%s\n' "$off" "$(grep grammars "$dir/own.lua")" >"$dir/vim.lua"
printf 'one\n' >"$W/thing.blk"
INIT="$dir/vim.lua" run "text:g;text:c;text:c;vimdump" "$W/Kconfig"
check "gc with the comment init.lua gives" 'NORMAL |  | 1:1 | # config FOO'
INIT="$dir/vim.lua" run "text:g;text:c;text:c;vimdump" "$W/thing.blk"
check "both ends of it" 'NORMAL |  | 1:1 | /* one */'
INIT="$dir/vim.lua" run "text:g;text:c;text:c;vimdump" "$W/link.ld"
check "an alias with none: its grammar's language's" 'NORMAL |  | 1:1 | // SECTIONS'

# An alias has no server of the language whose grammar it uses: a server
# for C takes main.c and not link.ld.
log="$dir/opened"
printf '%s\n%s\n' "$off" "blit.opt.lsp.servers.fakec = { command = { \"$fake\", \"--opened\", \"$log\", \"--name\", \"c\" }, languages = { \"c\" }, roots = { \".root\" } }
blit.opt.lsp.servers.fakecu = { command = { \"$fake\", \"--opened\", \"$log\", \"--name\", \"cu\" }, languages = { \"cu\", \"cuh\" }, roots = { \".root\" }, language_id = { cu = \"cuda-cpp\" } }
blit.opt.lsp.servers.fakeany = { command = { \"$fake\", \"--opened\", \"$log\", \"--name\", \"any\" }, languages = { \"anyl\" }, roots = { \".root\" }, language_id = \"plain-text\" }
blit.opt.syntax.grammars.ldc = { parser = \"c\", extensions = { \"ld\" } }
blit.opt.syntax.grammars.cu = { extensions = { \"cu\" } }
blit.opt.syntax.grammars.cuh = { extensions = { \"cuh\" } }
blit.opt.syntax.grammars.anyl = { extensions = { \"any\" } }" >"$dir/servers.lua"
: >"$W/.root"
printf '__global__ void k() {}\n' >"$W/k.cu"
printf '#pragma once\n' >"$W/k.cuh"
printf 'words\n' >"$W/x.any"
got=$(XDG_DATA_HOME="$dir/data" BLIT_INIT="$dir/servers.lua" "$blit" --grammars 2>&1 | grep '^  ldc ' | tr -s ' ')
check "an alias's grammar is the one it names" ' ldc the c grammar'
INIT="$dir/servers.lua" run "lspsettle;open:$W/link.ld;lspsettle;lspdump" "$W/main.c"
check "an alias gets no server of its grammar's language" 'fakec work 1 | main.c+ link.ld- | front not followed 0'
rm -f "$log"
INIT="$dir/servers.lua" run "lspsettle;open:$W/k.cuh;lspsettle;open:$W/x.any;lspsettle;open:$W/main.c;lspsettle;lspdump" "$W/k.cu"
got=$(sort "$log" | tr '\n' ' ')
check "language_id: by language, the protocol's name; blit's for the others; or one for every language" 'c main.c cuda-cpp k.cu cuh k.cuh plain-text x.any '

# ---------------------------------------------------------------- Objective-C
printf 'int main() {}\n' >"$W/a.m"
printf 'int main() {}\n' >"$W/a.mm"
language "$W/a.m"
check ".m is C to blit (and clangd)" '[c]'
language "$W/a.mm"
check ".mm is C++" '[cpp]'


if [ "$failed" = 0 ] && [ -z "$print" ]; then echo "langs.sh: all passed"; fi
exit "$failed"
