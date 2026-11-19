#!/bin/sh
# The status bar and the window's title, through the real events: what the
# bar's parts say, what a click on each does (Ln, Col goes to a line; the
# indentation, the line ends and the language are chosen from a list for
# the file), a language whose grammar is missing ("no colours") and the
# command that gets it (grammars go in a scratch data folder, and a curl
# that fails stands in for the network), and the title.
#
#   tests/status.sh path/to/blit            run the cases
#   tests/status.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/status.sh path/to/blit [--print]}
print=${2:-}
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
W="$dir/work"
mkdir -p "$W/src" "$dir/bin" "$dir/data"
printf 'int main() {\n  return 0;\n}\n' >"$W/src/main.cpp"
printf 'one\ntwo\nthree\n' >"$W/notes.txt"
printf 'wobble\n' >"$W/thing.wob"
printf 'wibble\n' >"$W/thing.wib"
printf '#!/bin/sh\necho "curl: (6) Could not resolve host" >&2\nexit 6\n' >"$dir/bin/curl"
chmod +x "$dir/bin/curl"
cat >"$dir/init.lua" <<'EOF'
blit.opt.lsp.enabled = false
blit.opt.git.enabled = false
blit.opt.editor.modal = false
blit.opt.syntax.grammars.wobble = { repo = "nobody/tree-sitter-wobble", rev = "v1", extensions = { "wob" } }
blit.opt.syntax.grammars.wibble = { extensions = { "wib" } }
EOF
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.syntax.enabled = false\n' >"$dir/plain.lua"
export XDG_DATA_HOME="$dir/data" PATH="$dir/bin:$PATH" BLIT_RECENT_FILE="$dir/recent"

failed=0
check() {
    if [ -n "$print" ]; then
        printf '%s\n    %s\n' "$1" "$got"
    elif [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$2" "$got"
        failed=1
    fi
}
# run SCRIPT [FILE]: the last line the run printed. INIT, if set, is the
# init.lua for this run only; FOLDER, the project folder (none: the side
# panel has none, and the one the file would open at is used).
INIT=
FOLDER=$W
run() {
    got=$(BLIT_INIT="${INIT:-$dir/init.lua}" "$blit" --offscreen 1400x700 --shot "$dir/shot.bmp" --script "${FOLDER:+workdir:$FOLDER;}frame;$1" ${2+"$2"} 2>&1 >/dev/null |
        sed -e "s#$dir#~#g" -e 's/^blit: //' | tail -1)
    INIT=
    FOLDER=$W
}

# What it says.
run "statusdump" "$W/src/main.cpp"
check "a language whose grammar is not installed: no colours" 'src/main.cpp | Ln 1, Col 1 Spaces: 2 LF C++ (no colours)'
run "statusdump" "$W/notes.txt"
check "plain text has none to miss" 'notes.txt | Ln 1, Col 1 Tabs LF Plain text'
INIT="$dir/plain.lua"
run "statusdump" "$W/src/main.cpp"
check "nor has a file with colours turned off (syntax.enabled = false)" 'NORMAL src/main.cpp | Ln 1, Col 1 Spaces: 2 LF C++'
# Where the cursor is, a key at a time (the bar made again only where it
# says so, when nothing else changed), and what is drawn of it.
run "text:x;frame;text:y;frame;key:Down;frame;key:Left;frame;key:shift+Right;frame;key:shift+Right;frame;statusdump" "$W/notes.txt"
check "typed, moved and selected a frame at a time" 'notes.txt ● | Ln 2, Col 4 (2 selected) Tabs LF Plain text'
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.editor.modal = false\nblit.opt.editor.minimap = false\nblit.opt.editor.spell_check = false\nblit.opt.look.animations = false\nblit.opt.look.cursor_glide_ms = 0\n' >"$dir/still.lua"
barshot() {
    BLIT_INIT="$dir/still.lua" "$blit" --offscreen 1400x700 --shot "$dir/$1.bmp" --script "workdir:$W;$2" "$W/notes.txt" >/dev/null 2>&1
}
barshot steps "frame;text:x;frame;text:y;frame;key:Down;frame;key:Left;frame;key:shift+Right;frame;key:shift+Right;frame"
barshot once "text:x;text:y;key:Down;key:Left;key:shift+Right;key:shift+Right"
cmp -s "$dir/steps.bmp" "$dir/once.bmp" || { echo "FAIL: the bar drawn a key at a time, as drawn once at the end"; failed=1; }

# The command that gets a grammar, with the installer blit has.
run "command:Install the grammar" "$W/notes.txt"
check "the command, for a file with no language" 'This file has no language'
run "command:Install the grammar" "$W/thing.wib"
check "a language nobody knows where to get a grammar for" 'No grammar is known for Wibble'
run "command:Install the grammar;wait:3000" "$W/thing.wob"
check "one fetched (here, a curl that fails): the bottom line says how it went" 'The Wobble grammar was not installed: could not fetch https://github.com/nobody/tree-sitter-wobble/archive/v1.tar.gz: curl: (6) Could not resolve host'

# A click on each part.
run "statusclick:Ln;finddump" "$W/src/main.cpp"
check "Ln, Col: Go to file, for a line" 'names [:] 0 of 4 :'
run "statusclick:Ln;text:3;key:Return;frame;statusdump" "$W/src/main.cpp"
check "a number and Enter: that line" 'src/main.cpp | Ln 3, Col 1 Spaces: 2 LF C++ (no colours)'
run "statusclick:Spaces;finddump" "$W/src/main.cpp"
check "the indentation: a list, what the file has chosen" 'pick [] 5 : Tabs >Spaces: 2 Spaces: 3 Spaces: 4 Spaces: 8'
run "statusclick:Spaces;key:Up;key:Return;frame;statusdump" "$W/src/main.cpp"
check "tabs chosen, for the file" 'src/main.cpp | Ln 1, Col 1 Tabs LF C++ (no colours)'
run "statusclick:LF;key:Down;key:Return;frame;statusdump" "$W/notes.txt"
check "CRLF chosen: the line ends made so, not saved" 'notes.txt ● | Ln 1, Col 1 Tabs CRLF Plain text'
run "statusclick:LF;key:Down;key:Return;key:End;key:Return;text:new;lua:print(((blit.text(0):gsub(string.char(13), '<CR>')):gsub(string.char(10), '|')))" "$W/notes.txt"
check "and Enter puts in the same" 'one<CR>|new<CR>|two<CR>|three<CR>|'
run "statusclick:LF;key:Down;key:Return;frame;statusclick:CRLF;finddump" "$W/notes.txt"
check "the list then has CRLF chosen" 'pick [] 2 : LF >CRLF'
run "statusclick:C++;finddump" "$W/src/main.cpp"
check "the language: getting its grammar first, then every one, this one chosen" 'pick [] 15 : Install the C++ grammar Plain text C >C++ CMake JSON'
run "statusclick:C++;text:python;key:Return;frame;statusdump" "$W/src/main.cpp"
check "another chosen: for this file, and it says so" 'src/main.cpp | Ln 1, Col 1 Spaces: 2 LF Python (no colours)'
run "statusclick:Plain;text:lua;key:Return;frame;lua:print(blit.info(0).language)" "$W/notes.txt"
check "and the scripts hear of it" 'lua'
run "statusclick:C++;key:Up;key:Up;key:Up;key:Return;wait:3000" "$W/src/main.cpp"
got=$(printf '%s' "$got" | sed -e 's#/archive/.*#/archive/...#')
check "the first row gets the grammar (and first the one it builds on)" 'The C++ grammar was not installed: needs c first: could not fetch https://github.com/tree-sitter/tree-sitter-c/archive/...'

# The window's title.
run "title" "$W/src/main.cpp"
check "the file, and the project folder" 'main.cpp — blit (~/work)'
run "text:x;title" "$W/src/main.cpp"
check "with a dot while it has unsaved changes" '● main.cpp — blit (~/work)'
FOLDER=
run "title" "$W/src/main.cpp"
check "no project folder yet: the one the file would open at" 'main.cpp — blit (~/work/src)'
run "title"
check "the start screen" 'blit (~/work)'

[ -n "$print" ] || [ $failed = 1 ] || echo "status ok"
exit $failed
