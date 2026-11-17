#!/bin/sh
# Scripts in Lua, through the real key and text events: init.lua's
# settings, mappings in each mode, commands, events, timers, programs, the
# documents, pickers and boxes, plugins (one installed with :PlugInstall
# from a repository in a scratch folder, never from the network), starting
# again when init.lua is saved, and scripts that fail or never end.
#
#   tests/lua.sh path/to/blit            run the cases
#   tests/lua.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/lua.sh path/to/blit [--print]}
print=${2:-}
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
C="$dir/config"  # the settings file, init.lua, lua/ and plugin/
D="$dir/data"    # blit's data folder: plugins/ is in it
W="$dir/work"
export BLIT_INIT="$C/init.lua"
export BLIT_APPEARANCE=dark  # (the theme cases write the dark one)
export XDG_DATA_HOME="$D"
P="$D/blit/plugins"

# What every init.lua here starts with, then the case's own Lua (from
# stdin). On the same line as the case's first, so that the case's lines
# keep their numbers (errors say them).
settings() { base="blit.opt.lsp.enabled = false blit.opt.git.enabled = false ${1:-} "; }
init() { { printf '%s' "$base"; cat; } >"$C/init.lua"; }
fresh() {
    rm -rf "$W" "$C" "$D"
    mkdir -p "$W" "$C" "$D"
    printf 'one\ntwo\nthree\n' >"$W/a.txt"
    printf '# notes\n' >"$W/b.md"
    settings
}

failed=0
check() {
    if [ -n "$print" ]; then
        printf '%s\n    %s\n' "$1" "$got"
    elif [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$2" "$got"
        failed=1
    fi
}
# run SCRIPT: what the run said (messages, without "blit: " and without
# tracebacks, and dumps), one after another with " ; " between.
run() {
    got=$("$blit" --offscreen 1000x600 --shot "$dir/shot.bmp" --script "frame;$1" "$W/a.txt" 2>&1 >/dev/null |
        grep -v -e '^stack traceback:' -e '^	' -e '^guard' | sed -e 's/^blit: //' -e "s#$dir#~#g" | awk 'BEGIN { ORS = "" } NR > 1 { print " ; " } { print }')
}

# ---------------------------------------------------------------- settings
fresh
run "luadump"
check "no init.lua, no plugins: no Lua at all" 'no scripts'
fresh
run "text::lua print(1 + 1);key:Return;luadump"
check ":lua makes a Lua state when there is none" '2 ; lua | maps 0 | commands 4 | handlers 0 | timers 0 | floats 0 | toasts 1'
fresh
init <<'EOF'
blit.opt.editor.tab_width = 2
blit.opt["font.size"] = 20
blit.opt.font.features = { "ss01", "zero" }
EOF
run "lua:print(blit.opt.editor.tab_width, blit.opt.font.size, table.concat(blit.opt.font.features, ','))"
check "settings from init.lua" '2	20	ss01,zero'
fresh
init <<'EOF'
blit.opt.lsp.servers.mine = { command = { "mine-ls" }, languages = { "cpp" } }
blit.opt.themes.night = { background = "#000000", syntax = { keyword = "#ff0000 bold" } }
blit.opt.theme = "night"
print(blit.opt.theme.dark, blit.opt.editor.nonsense)
EOF
run ""
check "tables of settings: a server, a theme" 'night	nil'
fresh
init <<'EOF'
blit.opt.editor.tab_width = "wide"
EOF
run "lua:print(blit.opt.editor.tab_width)"
check "a wrong setting: an error that says where" 'init.lua:1: editor.tab_width should be a number from 1 to 32 ; 4'
fresh
init <<'EOF'
blit.opt.editor.nope = 1
EOF
run ""
check "an unknown setting" 'init.lua:1: unknown setting editor.nope'
fresh
run "text::set tab_width=3 nolsp.enabled;key:Return;text::set ts? ligatures?;key:Return;vimdump"
check ":set, with the settings file's names and Vim's" 'NORMAL | editor.tab_width=3  font.ligatures=false | 1:1 | one'
fresh
run "text::set nornu;key:Return;text::set rnu?;key:Return;vimdump"
check ":set nornu: Vim's name for relative numbers" 'NORMAL | editor.relative_numbers=false | 1:1 | one'
fresh
run "text::set nonsense=1;key:Return;vimdump"
check ":set with a name that is no setting's" 'NORMAL | E518: Unknown option: nonsense | 1:1 | one'
fresh
run "text::set tab_w;key:Tab;vimdump;text:=;key:Tab;vimdump;key:Return;text::set ts?;key:Return;vimdump"
got=$(printf '%s' "$got" | sed -e 's/ | 1:1 | one//g')
check ":set and Tab: the name completed, then what it is now; Enter sets it" 'COMMAND | :set tab_width ; COMMAND | :set tab_width=4 ; NORMAL | editor.tab_width=4'
fresh
run "text::set tab;key:Tab;key:Tab;vimdump;key:shift+Tab;vimdump;key:Escape;text::set noli;key:Tab;vimdump"
got=$(printf '%s' "$got" | sed -e 's/ | 1:1 | one//g')
check "Tab again: the next name that fits, Shift+Tab the one before; no before a switch" 'COMMAND | :set tab_width ; COMMAND | :set tab_bar ; COMMAND | :set noligatures'
fresh
run "text::set ligatures=;key:Tab;vimdump;key:Tab;vimdump;key:Escape;text::set dark=rose-pine-;key:Tab;vimdump;key:Tab;vimdump;key:Tab;vimdump"
got=$(printf '%s' "$got" | sed -e 's/ | 1:1 | one//g')
check "values: true or false for a switch, the themes' names for a theme" 'COMMAND | :set ligatures=true ; COMMAND | :set ligatures=false ; COMMAND | :set dark=rose-pine-dawn ; COMMAND | :set dark=rose-pine-moon ; COMMAND | :set dark=rose-pine-dawn'

# ---------------------------------------------------------------- choosing a theme
# Every theme in a list; each is put on as the choice comes to it, Esc puts
# the one there was back, Enter keeps it and writes it into init.lua (in
# its tab, not saved): over the line that sets it, else over the template's
# commented-out one, else at the end.
fresh
run "command:Choose a theme;finddump;key:Down;key:Down;lua:print(blit.opt.theme.dark);key:Escape;lua:print(blit.opt.theme.dark)"
check "the list, the one in use chosen; each put on as it is come to, Esc puts it back" 'pick [] 5 : >blit-dark blit-light rose-pine rose-pine-dawn rose-pine-moon ; rose-pine ; blit-dark'
fresh
init <<'EOF'

  blit.opt.theme.dark = "blit-light" -- for now
EOF
run "command:Choose a theme;key:Down;key:Return;frame;dump;vimdump;lua:print(blit.opt.theme.dark, blit.info(0).modified)"
check "Enter: put on, and the line that sets it changed, in init.lua's tab, unsaved" '>[a.txt init.lua*] ; NORMAL |  | 2:3 |   blit.opt.theme.dark = "rose-pine" -- for now ; rose-pine	true'
fresh
run "command:Choose a theme;text:moon;key:Return;vimdump"
got="$got / $(grep -c 'theme.dark = "blit-dark"' "$C/init.lua")"
check "no init.lua: made from the template, the commented-out line taken, not saved" 'NORMAL |  | 29:1 | blit.opt.theme.dark = "rose-pine-moon" / 1'
fresh
init <<'EOF'
print("hello")
EOF
run "command:Choose a theme;text:dawn;key:Return;vimdump"
check "an init.lua without it: a line at the end" 'hello ; NORMAL |  | 2:1 | blit.opt.theme.dark = "rose-pine-dawn"'

# ---------------------------------------------------------------- blit's helpers, settings set by tables
fresh
run "lua:print(blit.inspect({ 1, 'two', x = true, [3.5] = { y = 'z' } }));lua:local t = {} t.self = t print(blit.inspect(t));lua:print(blit.inspect({ a = { b = { c = 1 } } }, 2))"
check "blit.inspect: a list, names, a key that is not a name, a cycle, a depth" '{ 1, "two", [3.5] = { y = "z" }, x = true } ; { self = <cycle> } ; { a = { b = {...} } }'
run "lua:print(blit.inspect({ alpha = 'aaaaaaaaaaaaaaaa', beta = 'bbbbbbbbbbbbbbbbbbbb', gamma = { 'cccccccccccccccccccc' } }))"
check "blit.inspect: on lines of their own when one would be long" '{ ;   alpha = "aaaaaaaaaaaaaaaa", ;   beta = "bbbbbbbbbbbbbbbbbbbb", ;   gamma = { "cccccccccccccccccccc" } ; }'
run "lua:print(table.concat(blit.split(' a  b c '), '|'));lua:print(table.concat(blit.split('a.b..c', '.', true), '|'));lua:print(table.concat(blit.split('k1=v1,k2=v2', '[=,]'), '|'));lua:print('[' .. blit.trim('  hi there ') .. ']')"
check "blit.split (white space, plain text, a pattern) and blit.trim" '|a|b|c| ; a|b||c ; k1|v1|k2|v2 ; [hi there]'
run "lua:print(1, nil, true, setmetatable({}, { __tostring = function() return 'T' end }))"
check "print: each as tostring writes it, tabs between" "$(printf '1\tnil\ttrue\tT')"
init <<'EOF'
blit.opt.font = { size = 18, ligatures = false }
blit.opt.editor = { tab_width = "wide" }
EOF
run "lua:print(blit.opt.font.size, blit.opt.font.ligatures)"
check "blit.opt: a table of fields sets each; an error in one says where" 'init.lua:2: editor.tab_width should be a number from 1 to 32 ; 18	false'

# ---------------------------------------------------------------- keys
fresh
init <<'EOF'
blit.opt.keys.leader = " "
blit.map("n", "<leader>w", function() print("w with count " .. blit.count()) end)
blit.map("i", "jk", "<Esc>")
EOF
run "text: w;text:3 w;text:ihijk;vimdump"
check "a leader mapping with a count, and jk for Esc" 'w with count 0 ; w with count 3 ; NORMAL |  | 1:2 | hione'
run "text:ij;luawait:1100;vimdump"
check "the first key of a mapping goes in after the time has passed" 'INSERT | -- INSERT -- | 1:2 | jone'
run "text:ijx;vimdump"
check "and when the next key is not the mapping's" 'INSERT | -- INSERT -- | 1:3 | jxone'
fresh
init <<'EOF'
blit.map("n", "X", "dd")
blit.map("n", "Y", "X", { remap = true })
blit.map("n", "Z", "X")
EOF
run "text:Y;lua:print(blit.info().lines);text:llZ;vimdump"
check "remap: keys that are mappings; not: Vim's own" '2 ; NORMAL |  | 1:2 | to'
fresh
init <<'EOF'
blit.map("n", "g1", function() print("g1") end)
blit.map("n", "g12", function() print("g12") end)
EOF
run "text:g12;text:g1;luawait:1100;text:g1x;vimdump"
check "a mapping that is the start of a longer one" 'g12 ; g1 ; g1 ; NORMAL |  | 1:1 | ne'
fresh
init <<'EOF'
blit.map("n", "<D-k>", function() print("cmd k") end)
blit.map("n", "<C-j>", function() print("ctrl j") end)
blit.map("n", "<F5>", function() print("f5") end)
blit.map("n", "<S-Tab>", function() print("shift tab") end)
EOF
run "key:cmd+K;key:ctrl+J;key:F5;key:shift+Tab"
check "Cmd, Ctrl, a function key, Shift with Tab" 'cmd k ; ctrl j ; f5 ; shift tab'
fresh
init <<'EOF'
blit.map("i", "<M-j>", function() print("alt j") end)
EOF
run "text:i;key:alt+J;text:∆;vimdump"
check "Option and a letter: what it types goes with it" 'alt j ; INSERT | -- INSERT -- | 1:1 | one'
fresh
init <<'EOF'
blit.map("n", "Q", "dd")
blit.unmap("n", "Q")
blit.map("x", "<leader>y", function() print(blit.selection()) end)
blit.map("o", "q", "w")
EOF
run "text:Q;text:ve\\y;key:Escape;text:0dq;vimdump"
check "unmap; Visual mode; an operator waiting" 'one	1	1	1	4 ; NORMAL |  | 1:1 | '
fresh
run "text::nnoremap Q dd;key:Return;text:Q;text::nmap;key:Return;luadump;vimdump"
check ":nnoremap and :nmap" 'lua | maps 1 | commands 4 | handlers 0 | timers 0 | floats 1 [n      Q              dd] | toasts 0 ; NORMAL |  | 1:1 | two'
fresh
settings 'blit.opt.editor.modal = false'
init <<'EOF'
blit.map("i", "<D-k>", function() print("cmd k, no modes") end)
blit.map("i", "ab", "X")
EOF
run "key:cmd+K;text:ab;text:a;luawait:1100;lua:print(blit.lines(0, 1, 1)[1])"
check "without Vim's modes the Insert mode mappings are the ones" 'cmd k, no modes ; Xaone'
fresh
init <<'EOF'
blit.map("n", "<leader>h", function() blit.feed("ihi<Esc>") blit.normal("j") end)
EOF
run "text:\\h;vimdump"
check "blit.feed and blit.normal" 'NORMAL |  | 2:2 | two'

# ---------------------------------------------------------------- commands
fresh
init <<'EOF'
blit.command("Hello", function(a) print("Hello " .. a.args .. " " .. #a.fargs .. " " .. tostring(a.bang)) end, { bang = true })
blit.command("Quiet", function() end, { nargs = 0 })
blit.command("Second", "2d")
EOF
run "text::Hello! a  b;key:Return;text::Quiet x;key:Return;vimdump;text::Second;key:Return;lua:print(blit.lines(0)[2])"
check "a command: its arguments, !, nargs; one that is a command line" 'Hello a  b 2 true ; NORMAL | E471: Quiet takes no arguments | 1:1 | one ; three'
run "text::Quiet!;key:Return;vimdump;text::Nothing;key:Return;vimdump"
check "! where none is allowed; a command nobody has" 'NORMAL | E477: No ! allowed | 1:1 | one ; NORMAL | E492: Not an editor command: Nothing | 1:1 | one'
run "text::=1 + 2;key:Return;text::lua =blit.opt.editor.tab_width;key:Return;lua:blit.cmd('Nothing')"
check ":= and :lua =" '3 ; 4 ; script:1: E492: Not an editor command: Nothing'
fresh
printf 'print("from a file " .. blit.buf())\n' >"$W/s.lua"
run "text::luafile $W/s.lua;key:Return;text::source $W/s.lua;key:Return"
check ":luafile and :source" 'from a file 1 ; from a file 1'

# ---------------------------------------------------------------- events
fresh
init <<'EOF'
blit.on("BufEnter", function(e) print("enter " .. e.file:match("[^/]+$")) end)
blit.on("BufLeave", function(e) print("leave " .. e.file:match("[^/]+$")) end)
blit.on("FileType", function(e) print("filetype " .. e.match) end)
blit.on("BufEnter", { pattern = "*.md" }, function() print("a markdown file") end)
EOF
run "text::e $W/b.md;key:Return"
check "BufEnter, BufLeave, FileType, a pattern" 'filetype  ; enter a.txt ; filetype markdown ; leave a.txt ; enter b.md ; a markdown file'
fresh
init <<'EOF'
blit.on("ModeChanged", function(e) print(e.match) end)
blit.on("InsertLeave", function() print("left insert") end)
blit.on("TextChanged", function() print("changed") end)
local id = blit.on("CursorMoved", function() print("moved") end)
blit.on("CursorMoved", { once = true }, function() print("moved once") blit.off(id) end)
EOF
run "text:ihi;key:Escape;text:j;text:j"
check "modes, Insert mode left, text changed, the cursor, once, off" 'n:i ; i:n ; left insert ; changed ; moved ; moved once'
fresh
init <<'EOF'
blit.on("BufWritePre", function(e) blit.set_lines(e.buf, 1, 1, { "ONE" }) blit.cmd("w") end)
blit.on("BufWritePost", function(e) print("written " .. e.file:match("[^/]+$")) end)
EOF
run "text::w;key:Return"
got="$got / $(head -1 "$W/a.txt")"
check "BufWritePre changes the text first (and a save it makes fires nothing)" 'written a.txt / ONE'
fresh
init <<'EOF'
local g = blit.group("mine")
blit.on("User", { pattern = "Ping", group = g }, function(e) print("ping " .. e.data.n) end)
blit.on("User", { pattern = "Pong" }, function() print("pong") end)
EOF
run "lua:blit.emit('Ping', { n = 1 }) blit.emit('Pong') blit.group('mine') blit.emit('Ping', { n = 2 })"
check "User events, and a group cleared" 'ping 1 ; pong'
fresh
init <<'EOF'
blit.on("Nonsense", function() end)
EOF
run ""
check "an event that is not one" 'init.lua:1: blit.on: unknown event "Nonsense"'

# ---------------------------------------------------------------- timers and programs
fresh
init <<'EOF'
local n = 0
blit.defer(30, function() print("deferred") end)
blit.schedule(function() print("scheduled") end)
local t
t = blit.every(10, function() n = n + 1 if n == 3 then blit.stop(t) print("every, 3 times") end end)
EOF
run "luawait:150"
check "defer, schedule, every and stop" 'scheduled ; deferred ; every, 3 times'
fresh
init <<'EOF'
local said = {}
local function done(what) said[#said + 1] = what if #said == 3 then table.sort(said) print(table.concat(said, " | ")) end end
blit.system({ "sh", "-c", "printf out; printf err >&2; exit 3" }, {}, function(r) done(r.code .. " " .. r.stdout .. " " .. r.stderr) end)
local parts = {}
blit.system({ "sh", "-c", "echo a; sleep 0.05; echo b" }, { stdout = function(s) parts[#parts + 1] = s end }, function(r)
  done("streamed " .. table.concat(parts):gsub("\n", " ") .. "exit " .. r.code)
end)
local r = blit.system({ "cat" }, { stdin = "fed in" })
print("waited " .. r.code .. " " .. r.stdout)
local id = blit.system({ "sleep", "5" }, {}, function(r2) done("killed " .. r2.code) end)
blit.kill(id)
EOF
run "luawait"
check "programs: their output, as it comes, waited for, killed" 'waited 0 fed in ; 3 out err | killed 143 | streamed a b exit 0'
fresh
init <<'EOF'
blit.opt.script.timeout_ms = 200
blit.system({ "sleep", "5" })
EOF
run ""
check "a program waited for longer than it may be" 'init.lua:2: blit.system: sleep did not finish in time (it was stopped)'

# ---------------------------------------------------------------- the documents
fresh
init <<'EOF'
blit.map("n", "<leader>t", function()
  blit.set_lines(0, 2, 2, { "TWO", "two and a half" })
  blit.set_lines(0, 1, 0, { "zero" })
  blit.set_lines(0, -1, -1, {})
  blit.set_lines(0, 5, 4, { "last" })
  print((blit.text():gsub("\n", "|")))
  local i = blit.info()
  print(i.name, i.lines, i.modified, i.language)
end)
EOF
run "text:\\t;text:u;lua:print((blit.text():gsub('\\n', '|')))"
check "set_lines: replace, before, take away, after; one undo step" 'zero|one|TWO|two and a half|last| ; a.txt	5	true	 ; one|two|three|'
fresh
run "lua:blit.set_cursor(2, 3) print(blit.cursor());lua:print(blit.lines(0, 9, 9)[1], #blit.lines(0, 2, 3), blit.lines(0, -2, -2)[1]);lua:local b = blit.open('$W/b.md') print(#blit.bufs(), blit.info(b).language, blit.buf() == b)"
check "the cursor, lines that are not there, another file" '2	3 ; nil	2	two ; 2	markdown	true'
fresh
run "lua:blit.set_cursor(3, 4);lua:blit.set_lines(0, 1, 3, { 'one', 'TWO', 'three' }) print(blit.cursor());lua:blit.set_lines(0, 1, 3, { 'one', 'TWO', 'three' }) blit.cmd('w') blit.set_lines(0, 1, -1, { 'one', 'TWO', 'three' }) print(blit.info().modified)"
check "set_lines changes only what differs: the cursor stays; the same lines again are no change" '3	4 ; false'
fresh
run "lua:blit.set_cursor(9);lua:blit.set_lines(0, 7, 7, {});lua:blit.info(99)"
check "lines and buffers that are not there" 'script:1: blit.set_cursor: there is no line 9 ; script:1: blit.set_lines: lines 7 to 7 are not in a text of 3 lines ; script:1: blit.info: no such buffer'

# ---------------------------------------------------------------- pickers and boxes
fresh
init <<'EOF'
blit.map("n", "<leader>p", function()
  blit.ui.select({ { n = "apple" }, { n = "banana" }, { n = "cherry" } }, { prompt = "Fruit", format = function(x) return x.n end },
    function(item, i) print(item and (item.n .. " " .. i) or "nothing") end)
end)
blit.map("n", "<leader>i", function()
  blit.ui.input({ prompt = "Name", default = "ab" }, function(s) print("typed " .. tostring(s)) end)
end)
EOF
run "text:\\p;text:an;finddump;key:Return;text:\\p;key:Escape;text:\\i;text:c;key:Return;text:\\i;key:cmd+P;key:Escape"
check "select (items of any kind), Esc, input, a picker another box replaced" 'pick [an] 1 : >banana ; banana 2 ; nothing ; typed abc ; typed nil'
fresh
init <<'EOF'
blit.map("n", "<leader>f", function()
  blit.ui.float("a box\nof two lines", { title = "Box" })
  blit.ui.float({ "stays" }, { close = "never", at = "top-right" })
  blit.ui.float({ "goes" }, { timeout = 50, at = "center" })
end)
EOF
run "text:\\f;luadump;key:Escape;luadump;lua:blit.ui.close(1);luadump"
check "boxes: shut by Esc, never, by the clock" 'lua | maps 1 | commands 4 | handlers 0 | timers 0 | floats 3 [a box] [stays] [goes] | toasts 0 ; lua | maps 1 | commands 4 | handlers 0 | timers 0 | floats 1 [stays] | toasts 0 ; lua | maps 1 | commands 4 | handlers 0 | timers 0 | floats 1 [stays] | toasts 0'
fresh
init <<'EOF'
blit.ui.float({ "goes" }, { timeout = 30, close = "never" })
blit.ui.float({ "moves" }, { close = "move" })
EOF
run "luawait:80;text:j;luadump"
check "a box with a time, and one that goes when the cursor moves" 'lua | maps 0 | commands 4 | handlers 0 | timers 0 | floats 0 | toasts 0'
fresh
init <<'EOF'
blit.on("VimEnter", function()
  local ns = blit.ns("mine")
  blit.mark(0, ns, 1, 1, 3, { kind = "error", note = "a note" })
  blit.mark(0, ns, 2, 1, 2)
  blit.mark(0, blit.ns("other"), 3, 1, 1, { kind = "hint" })
  blit.clear_marks(0, ns)
end)
blit.ui.status("on the right")
blit.notify("a warning", "warn")
blit.notify("an error", "error")
EOF
run "luadump"
check "marks, the bottom line, messages" 'a warning ; an error ; lua | maps 0 | commands 4 | handlers 1 | timers 0 | floats 0 | toasts 2'
fresh
init <<'EOF'
blit.ui.status("on the right")
EOF
run "workdir:$W;frame;statusdump"
check "blit.ui.status: in the status bar (kept over the indentation, where there is not room for all)" 'NORMAL a.txt | on the right Ln 1, Col 1 LF Plain text'
# Marks put in out of order are drawn as the same marks put in in order.
fresh
init <<'EOF'
blit.on("VimEnter", function()
  local ns = blit.ns("mine")
  blit.mark(0, ns, 1, 1, 3, { kind = "error", note = "first" })
  blit.mark(0, ns, 2, 1, 2, { kind = "match" })
  blit.mark(0, ns, 3, 2, 4, { kind = "hint", note = "third" })
end)
EOF
run ""
cp "$dir/shot.bmp" "$dir/in_order.bmp"
init <<'EOF'
blit.on("VimEnter", function()
  local ns = blit.ns("mine")
  blit.mark(0, ns, 3, 2, 4, { kind = "hint", note = "third" })
  blit.mark(0, ns, 2, 1, 2, { kind = "match" })
  blit.mark(0, ns, 1, 1, 3, { kind = "error", note = "first" })
end)
EOF
run ""
got=$(cmp -s "$dir/shot.bmp" "$dir/in_order.bmp" && echo same || echo different)
check "marks put in out of order are drawn the same" 'same'
init <<'EOF'
blit.on("VimEnter", function() blit.mark(0, blit.ns("x"), 1, 1, 3, { kind = "error" }) end)
EOF
run ""
got=$(cmp -s "$dir/shot.bmp" "$dir/in_order.bmp" && echo same || echo different)
check "(and other marks are drawn differently)" 'different'
fresh
init <<'EOF'
blit.on("VimEnter", function() blit.mark(0, blit.ns("x"), 1, 1, 1, { kind = "wobbly" }) end)
EOF
run ""
check "a mark of a kind there is not" 'VimEnter: init.lua:1: blit.mark: unknown kind "wobbly" (match, select, error, warning, info, hint)'

fresh
init <<EOF
blit.cmd("2")
blit.open("$W/b.md")
blit.on("VimEnter", function() print(blit.info().name, #blit.bufs()) end)
EOF
run "text::bn;key:Return;vimdump"
check "init.lua opening a file and running a command: once blit has started" 'b.md	2 ; NORMAL |  | 2:1 | two'

# ---------------------------------------------------------------- modules and plugins
fresh
mkdir -p "$C/lua/mine" "$C/plugin" "$P/thing/lua" "$P/thing/plugin"
printf 'return { hi = function() return "module" end }\n' >"$C/lua/mine/init.lua"
printf 'print("config plugin/ ran after init.lua: " .. tostring(INIT_DONE))\n' >"$C/plugin/after.lua"
printf 'return { setup = function(o) print("thing set up with " .. o.x) end }\n' >"$P/thing/lua/thing.lua"
printf 'print("the plugin ran")\n' >"$P/thing/plugin/go.lua"
init <<'EOF'
print(require("mine").hi())
require("thing").setup({ x = 7 })
INIT_DONE = true
EOF
run ""
check "require from lua/, a plugin's modules, plugin/ files after init.lua" 'module ; thing set up with 7 ; config plugin/ ran after init.lua: true ; the plugin ran'
fresh
R="$dir/repo.git"
rm -rf "$R" "$dir/src"
git init --quiet --bare "$R"
git init --quiet "$dir/src"
mkdir -p "$dir/src/plugin"
printf 'print("installed and loaded")\n' >"$dir/src/plugin/p.lua"
git -C "$dir/src" add -A
git -C "$dir/src" -c user.name=test -c user.email=test@example.invalid commit --quiet -m one
git -C "$dir/src" push --quiet "$R" HEAD:refs/heads/main 2>/dev/null
git --git-dir="$R" symbolic-ref HEAD refs/heads/main
init <<EOF
blit.plug("$R")
EOF
run "text::PlugList;key:Return;luadump;key:Escape;text::PlugInstall;key:Return;luawait;text::PlugInstall;key:Return"
got="$got / $(ls "$P")"
check ":PlugList, :PlugInstall (from a folder), and once it is there" 'lua | maps 0 | commands 4 | handlers 0 | timers 0 | floats 1 [- repo  ~/repo.git] | toasts 0 ; installed and loaded ; Installed repo ; Every plugin named with blit.plug() is installed / repo'
rm -rf "$R" "$dir/src"

# ---------------------------------------------------------------- starting again, and failing
fresh
init <<'EOF'
print("init.lua ran")
EOF
run "text::e $C/init.lua;key:Return;text::w;key:Return;text::Reload;key:Return;luawait:20"
check "saving init.lua runs it again (and says so), and so does :Reload" 'init.lua ran ; init.lua ran ; Settings applied ; init.lua ran'
fresh
init <<'EOF'
blit.opt.editor.tab_width = 2
EOF
run "text::e $C/init.lua;key:Return;vim:oblit.opt.editor.tabbar = false<Esc>;text::w;key:Return;lua:print(blit.opt.editor.tab_width)"
check "init.lua saved with a line that is wrong: that line, and nothing applied said" 'init.lua:2: unknown setting editor.tabbar ; 2'
fresh
init <<'EOF'
print("before"
EOF
run "text::lua print('still here');key:Return"
check "init.lua that does not parse" "init.lua:2: ')' expected (to close '(' at line 1) near <eof> ; still here"
fresh
settings 'blit.opt.script.timeout_ms = 200'
init <<'EOF'
blit.map("n", "L", function() print("mapped anyway") end)
while true do end
EOF
run "text:L"
check "init.lua that never ends is stopped" 'init.lua:2: stopped: a script ran for longer than script.timeout_ms ; mapped anyway'
fresh
init <<'EOF'
blit.map("n", "E", function() error("on purpose") end)
blit.map("n", "R", function() blit.feed("R", true) end)
EOF
run "text:E;text:R;text::messages;key:Return;luadump"
check "a mapping that fails, one that types itself" 'mapping E: init.lua:1: on purpose ; mapping R: init.lua:2: blit.feed: mappings that type mappings, more than 20 deep ; lua | maps 2 | commands 4 | handlers 0 | timers 0 | floats 1 [mapping E: init.lua:1: on purpose] | toasts 2'

# Commands named like :map commands, <args> in what a command runs, a
# command that runs itself, lines counted as Vim does (a final newline is no
# line of its own, and stays), a wrong argument that fails cleanly, and a
# script that catches its own timeout.
fresh
init <<'EOF'
blit.opt.script.timeout_ms = 300
blit.command("Heatmap", function() print("hot") end)
blit.command("Say", "lua print('<args>')", { nargs = "*" })
blit.command("Again", "Again")
EOF
run "lua:blit.cmd('Heatmap');lua:blit.cmd('Say <args>');lua:print(pcall(blit.cmd, 'Again'))"
got=$(printf '%s' "$got" | sed -e 's/	.*E169.*/	E169/')
check "a command named like a mapping, <args> in its argument, one that runs itself" 'hot ; <args> ; false	E169'
fresh
run "lua:print(blit.info(0).lines, #blit.lines(0, 1, -1));lua:blit.set_lines(0, 2, -1, { 'x' });lua:print((blit.text(0):gsub(string.char(10), '|')))"
check "lines as Vim counts them; the final newline kept" '3	3 ; one|x|'
fresh
run "lua:print((pcall(blit.fs.read, '$W/a.txt', 'many')))"
check "a wrong argument fails, it does not crash" 'false'
fresh
printf 'blit.opt.lsp.enabled = false blit.opt.script.timeout_ms = 300\n' >"$C/init.lua"
run "lua:while true do pcall(function() while true do end end) end;lua:print('after')"
got=$(printf '%s' "$got" | sed -e 's/.*stopped: .*script.timeout_ms/stopped/')
check "a script that catches its own timeout is stopped all the same" 'stopped ; after'

[ -n "$print" ] || [ $failed = 1 ] || echo "lua ok"
exit $failed
