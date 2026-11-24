#!/bin/sh
# The Build tab and the start screen's New project tab: what each kind of
# folder is found to build with, init.lua's own systems (blit.build.add),
# running a task in the output terminal, and new projects made from the
# form (and init.lua's own kinds, blit.project.add). vcpkg is a stand-in
# folder here: its catalog and a port's usage notes, nothing installed.
#
#   tests/build.sh path/to/blit            run the cases
#   tests/build.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/build.sh path/to/blit [--print]}
print=${2:-}
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
export BLIT_RECENT_FILE="$dir/recent"
export HOME="$dir/home"  # (new projects go in ~/Dev by default)
mkdir -p "$HOME/Dev"
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\n' >"$dir/init.lua"

# Folders of each kind.
mkdir -p "$dir/cm" "$dir/pre" "$dir/mk" "$dir/ms" "$dir/uv/tests" "$dir/pip/.venv" "$dir/rs" "$dir/none"
printf 'project(x)\n' >"$dir/cm/CMakeLists.txt"
cp "$dir/cm/CMakeLists.txt" "$dir/pre/CMakeLists.txt"
cat >"$dir/pre/CMakePresets.json" <<'EOF'
{"version": 6,
 "configurePresets": [{"name": "base", "hidden": true, "binaryDir": "${sourceDir}/out/${presetName}"}, {"name": "dev", "inherits": "base"}],
 "buildPresets": [{"name": "dev", "configurePreset": "dev"}]}
EOF
printf 'CC := cc\nall:\n\techo built-it\nx.o: x.c\ntest install:\n\ttrue\nclean:\n\trm -f x\n%%.o: %%.c\nfail:\n\texit 2\n' >"$dir/mk/Makefile"
printf "project('x', 'c')\n" >"$dir/ms/meson.build"
printf 'print(1)\n' >"$dir/uv/main.py"
: >"$dir/uv/uv.lock"
printf '[project]\nname = "p"\n' >"$dir/pip/pyproject.toml"
printf '[package]\nname = "rs"\n' >"$dir/rs/Cargo.toml"

# A stand-in vcpkg: its catalog, and fmt's usage notes.
V="$dir/vcpkg"
mkdir -p "$V/scripts/buildsystems" "$V/ports/fmt" "$V/ports/zlib" "$V/versions"
: >"$V/scripts/buildsystems/vcpkg.cmake"
printf '{"default": {"fmt": {"baseline": "11.0.2"}, "zlib": {"baseline": "1.3.1"}}}\n' >"$V/versions/baseline.json"
printf '{"name": "fmt", "description": "Formatting"}\n' >"$V/ports/fmt/vcpkg.json"
printf '{"name": "zlib", "description": "Compression"}\n' >"$V/ports/zlib/vcpkg.json"
printf 'fmt provides CMake targets:\n\n    find_package(fmt CONFIG REQUIRED)\n    target_link_libraries(main PRIVATE fmt::fmt)\n\n    # Or\n    find_package(fmt CONFIG REQUIRED)\n    target_link_libraries(main PRIVATE fmt::fmt-header-only)\n' >"$V/ports/fmt/usage"
export VCPKG_ROOT="$V"

failed=0
check() {
    if [ -n "$print" ]; then
        printf '%s\n    %s\n' "$1" "$got"
    elif [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$2" "$got"
        failed=1
    fi
}
# run FOLDER SCRIPT: the last line the run printed. INIT, if set, is the
# init.lua for this run only.
INIT=
run() {
    got=$(BLIT_INIT="${INIT:-$dir/init.lua}" "$blit" --offscreen 1000x700 --shot "$dir/shot.bmp" --script "workdir:$1;frame;$2" 2>&1 >/dev/null | sed -e "s#$dir#~#g" | tail -1)
    INIT=
}

# What each folder builds with.
run "$dir/cm" "builddump"
check "CMakeLists.txt, never configured" 'CMake: >Build Configure Test'
run "$dir/pre" "builddump"
check "presets: the ones shown, a build preset's folder by its preset" 'CMake: Configure (dev) >Build (dev)'
build_dev="lua:for _, t in ipairs(blit.build.tasks()) do if t.name == 'Build (dev)' then print(t.cmd) end end"
run "$dir/pre" "$build_dev"
check "a build preset whose folder is not configured yet: configured first" 'blit: cmake --preset dev && cmake --build --preset dev'
mkdir -p "$dir/pre/out/dev"
: >"$dir/pre/out/dev/CMakeCache.txt"
run "$dir/pre" "builddump"
check "configured: its folder (an inherited \${presetName}, the preset's own name) is still its preset's" 'CMake: Configure (dev) >Build (dev)'
run "$dir/pre" "$build_dev"
check "and built alone" 'blit: cmake --build --preset dev'
# A binaryDir outside the folder: an absolute one, looked at where it is;
# one with a macro not known here ($env{}), never configured again and again.
mkdir -p "$dir/pre-abs" "$dir/pre-env"
cp "$dir/cm/CMakeLists.txt" "$dir/pre-abs/CMakeLists.txt"
cp "$dir/cm/CMakeLists.txt" "$dir/pre-env/CMakeLists.txt"
printf '{"version": 6, "configurePresets": [{"name": "dev", "binaryDir": "%s/elsewhere/${presetName}"}], "buildPresets": [{"name": "dev", "configurePreset": "dev"}]}\n' "$dir" >"$dir/pre-abs/CMakePresets.json"
printf '{"version": 6, "configurePresets": [{"name": "dev", "binaryDir": "$env{TMPDIR}/blit-never/${presetName}"}], "buildPresets": [{"name": "dev", "configurePreset": "dev"}]}\n' >"$dir/pre-env/CMakePresets.json"
run "$dir/pre-abs" "$build_dev"
check "an absolute binaryDir not configured yet: configured first" 'blit: cmake --preset dev && cmake --build --preset dev'
mkdir -p "$dir/elsewhere/dev"
: >"$dir/elsewhere/dev/CMakeCache.txt"
run "$dir/pre-abs" "$build_dev"
check "an absolute binaryDir configured: built alone" 'blit: cmake --build --preset dev'
run "$dir/pre-env" "$build_dev"
check "a binaryDir with \$env{}: built alone (where it is is not known here)" 'blit: cmake --build --preset dev'
# A build preset of CMakeUserPresets.json that inherits one of CMakePresets.json.
mkdir -p "$dir/pre-user"
cp "$dir/cm/CMakeLists.txt" "$dir/pre-user/CMakeLists.txt"
printf '{"version": 6, "configurePresets": [{"name": "base", "binaryDir": "${sourceDir}/build/${presetName}"}], "buildPresets": [{"name": "base", "hidden": true, "configurePreset": "base"}]}\n' >"$dir/pre-user/CMakePresets.json"
printf '{"version": 6, "buildPresets": [{"name": "dev", "inherits": "base"}]}\n' >"$dir/pre-user/CMakeUserPresets.json"
run "$dir/pre-user" "$build_dev"
check "a user build preset inheriting the project's: its configure preset found there" 'blit: cmake --preset base && cmake --build --preset dev'
run "$dir/mk" "builddump"
check "a Makefile: make, and its targets (not files, patterns or variables)" 'Make: >make make all make test make install make clean make fail'
run "$dir/ms" "builddump"
check "meson.build, not set up yet" 'Meson: >Build Set up'
run "$dir/uv" "builddump"
check "uv: run, test, sync" 'Python: >Run Test Sync'
run "$dir/pip" "builddump"
check "pip: its .venv's python" 'Python: >Install (editable)'
run "$dir/rs" "builddump"
check "other languages are init.lua's to add" 'nothing'

# init.lua's own.
cat >"$dir/cargo.lua" <<'EOF'
blit.opt.lsp.enabled = false
blit.opt.git.enabled = false
blit.build.add("Cargo", {detect = "Cargo.toml", tasks = {{name = "Build", cmd = "cargo build", default = true}, "cargo test"}})
EOF
INIT="$dir/cargo.lua"
run "$dir/rs" "builddump"
check "blit.build.add: files and a list" 'Cargo: >Build cargo test'
cat >"$dir/fn.lua" <<'EOF'
blit.opt.lsp.enabled = false
blit.opt.git.enabled = false
blit.build.add("Mine", {first = true,
  detect = function(root, files) for _, f in ipairs(files) do if f.name == "Makefile" then return true end end end,
  tasks = function(root, files) return {{name = "Hi", cmd = "echo hi from " .. #files}} end})
blit.build.remove("Make")
EOF
INIT="$dir/fn.lua"
run "$dir/mk" "lua:local t = blit.build.tasks() print(t[1].system .. ' ' .. t[1].cmd .. ' ' .. #t .. ' ' .. table.concat(blit.build.systems(), ','))"
check "functions, first, and a built-in taken away" 'blit: Mine echo hi from 1 1 Mine,CMake,Meson,Autotools,Ninja,Python'

# Running: in the output terminal, how it ended.
run "$dir/mk" "buildrun;buildwait;builddump"
check "the Run button's task, to its end" 'Make: >make make all make test make install make clean make fail (last: 0)'
run "$dir/mk" "buildrun:make fail;buildwait;builddump"
check "a task that fails: its status, and it is the Run button's now" 'Make: make make all make test make install make clean >make fail (last: 2)'
run "$dir/mk" "lua:print(blit.build.run('make clean'), blit.build.run('no such'));buildwait;builddump"
check "blit.build.run" 'Make: make make all make test make install >make clean make fail (last: 0)'

# Problems a build reports: a fake one printing what compilers, CMake,
# Ninja, make and Python print (GCC's colours, Ninja's progress written over
# itself, a header's warning twice), its files there to go to.
F="$dir/fb"
mkdir -p "$F/src" "$F/lib" "$F/sub" "$F/cmake"
printf 'int a;\nint b;\nint c = x;\nint d;\nint e;\nint f;\nint g = y;\n' >"$F/src/a.c"
printf '#pragma once\nint unused;\n' >"$F/src/b.h"
printf '\n\n\n  bad();\n' >"$F/lib/c.c"
printf 'one\ntwo\n' >"$F/sub/d.c"
printf 'cmake_minimum_required(VERSION 3.20)\nmessage(WARNING "look here")\n' >"$F/cmake/x.cmake"
printf 'import sys\nraise ValueError("boom")\n' >"$F/tool.py"
cat >"$F/errs.sh" <<'EOF'
sleep 0.2  # (the output split laid out first, as a window does at once: a terminal is 80 columns until then)
e=$(printf '\033')
printf '%s[01m%s[Ksrc/a.c:3:9:%s[m%s[K %s[01;31m%s[Kerror: %s[m%s[K'"'"'x'"'"' undeclared\n' $e $e $e $e $e $e $e $e
printf '    3 | int c = x;\n      |         ^\n'
printf 'src/b.h:2:5: warning: unused [-Wunused]\nsrc/b.h:2:5: warning: unused [-Wunused]\n'
printf 'src/a.c:3:9: note: each undeclared identifier is reported only once\ng++: error: no input files\n'
printf '\r[1/2] Building C object a.o\033[K\r[2/2] Linking C executable p\033[K\r\n'
printf 'ninja: Entering directory `lib'"'"'\nFAILED: c.o\nc.c:4:3: error: bad\n'
printf 'make -C sub\nd.c:2:1: warning: in a folder make went into\n'
printf 'CMake Warning at cmake/x.cmake:2 (message):\n  look here\n\n\n'
printf 'Traceback (most recent call last):\n  File "%s/tool.py", line 2, in <module>\n    raise ValueError("boom")\nValueError: boom\n' "$(pwd -P)"
printf 'src/a.c:7:9: error: second\n'
exit 2
EOF
printf 'errors:\n\tsh errs.sh\nok:\n\techo fine\n' >"$F/Makefile"
run "$F" "buildrun:make errors;buildwait;problemsdump"
check "a build's problems: a file's together, in the order the files came" 'button: ▶ make errors · 4 errors | make errors 7 · failed (exit 2) | file src/a.c | >E 3:9 '"'"'x'"'"' undeclared | E 7:9 second | file src/b.h | W 2:5 unused [-Wunused] | file lib/c.c | E 4:3 bad | file sub/d.c | W 2:1 in a folder make went into | file cmake/x.cmake | W 2 look here | file tool.py | E 2 ValueError: boom'
run "$F" "buildrun:make errors;buildwait;frame;statusdump"
check "counted in the status bar" 'NORMAL untitled | × 4 ▲ 3 Ln 1, Col 1 LF Plain text'
run "$F" "buildrun:make errors;buildwait;key:F8;key:F8;key:F8;frame;statusdump"
check "F8: the next problem, across files" 'NORMAL src/b.h | × 4 ▲ 3 Ln 2, Col 5 Tabs LF C++'
run "$F" "buildrun:make errors;buildwait;key:F8;key:F8;key:F8;problemsdump"
got="file ${got#* | file }"  # (the rows, from the first file's)
check "and the Problems tab picks it" 'file src/a.c | E 3:9 '"'"'x'"'"' undeclared | E 7:9 second | file src/b.h | >W 2:5 unused [-Wunused] | file lib/c.c | E 4:3 bad | file sub/d.c | W 2:1 in a folder make went into | file cmake/x.cmake | W 2 look here | file tool.py | E 2 ValueError: boom'
run "$F" "buildrun:make errors;buildwait;key:shift+F8;frame;statusdump"
check "Shift+F8 from the start: the last" 'NORMAL tool.py | × 4 ▲ 3 Ln 2, Col 1 LF Python'
run "$F" "buildrun:make errors;frame;buildwait;frame;termclick:c.c:4:3;frame;statusdump"
check "Cmd+click on a path:line in the output: from the folder Ninja went into" 'NORMAL lib/c.c | × 4 ▲ 3 Ln 4, Col 3 Spaces: 2 LF C'
run "$F" "buildrun:make errors;frame;buildwait;frame;termclick:tool.py\", line 2;frame;statusdump"
check "and on Python's File \"path\", line N (a long line, wrapped)" 'NORMAL tool.py | × 4 ▲ 3 Ln 2, Col 1 LF Python'
run "$F" "buildrun:make errors;frame;buildwait;frame;termclick:Linking;frame;statusdump"
check "Cmd+click on what is not a place: nothing opens (the click is the terminal's)" 'make errors | × 4 ▲ 3'
run "$F" "buildrun:make errors;buildwait;buildrun:make ok;buildwait;problemsdump"
check "the next run starts with none" 'button: ▶ make ok | file 0'
# In the text: each problem underlined in its file, from its column over
# the word there (the line's text where it gives none), the cursor's line's
# error in words after it; moved as lines go in or out above, gone when
# its own line is changed, and all gone at the next run.
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.editor.modal = false\n' >"$dir/plain.lua"
INIT="$dir/plain.lua"
run "$F" "buildrun:make errors;buildwait;open:$F/src/a.c;buildmarks"
check "a build's problems underlined in their file" '3:9-9E 7:9-9E'
INIT="$dir/plain.lua"
run "$F" "buildrun:make errors;buildwait;open:$F/src/a.c;frame;key:Down;key:Down;buildmarks"
check "the cursor's line: its error in words" '3:9-9E 7:9-9E | 3: '"'"'x'"'"' undeclared'
INIT="$dir/plain.lua"
run "$F" "buildrun:make errors;buildwait;open:$F/src/b.h;buildmarks"
check "a warning over its word" '2:5-10W'
# Problems read from a file beside the run's: the run is still open, and so are its marks.
printf 'elsewhere.c:1:1: error: not this file\n' >"$F/other.log"
INIT="$dir/plain.lua"
run "$F" "buildrun:make errors;buildwait;open:$F/src/a.c;frame;problemsread:$F/other.log;buildwait;frame;buildmarks"
check "problems read from a file: the run's marks kept" '3:9-9E 7:9-9E'
INIT="$dir/plain.lua"
run "$F" "buildrun:make errors;buildwait;open:$F/cmake/x.cmake;buildmarks"
check "no column: the line's text" '2:1-28W'
INIT="$dir/plain.lua"
run "$F" "buildrun:make errors;buildwait;open:$F/src/a.c;frame;key:Return;buildmarks"
check "a line put in above: they move down with their lines" '4:9-9E 8:9-9E'
INIT="$dir/plain.lua"
run "$F" "buildrun:make errors;buildwait;open:$F/src/a.c;frame;key:Down;key:Down;key:End;key:Return;buildmarks"
check "a line opened after one's text: it stays, the next moves" '3:9-9E 8:9-9E'
INIT="$dir/plain.lua"
run "$F" "buildrun:make errors;buildwait;open:$F/src/a.c;frame;key:Down;key:Down;key:End;key:Left;text:1;buildmarks"
check "its own line changed: that one gone" '7:9-9E'
INIT="$dir/plain.lua"
run "$F" "buildrun:make errors;buildwait;open:$F/src/a.c;frame;key:Down;key:shift+Down;key:Delete;buildmarks"
check "a line taken out above: up with their lines" '2:9-9E 6:9-9E | 2: '"'"'x'"'"' undeclared'
INIT="$dir/plain.lua"
run "$F" "buildrun:make errors;buildwait;open:$F/src/a.c;buildrun:make ok;buildwait;buildmarks"
check "the next run: none" 'none'
# Clang writing to a terminal (under make) wraps its message to the
# terminal's width: a long path leaves none on the first line.
cat >"$F/wrapped.sh" <<'EOF'
printf 'src/a.c:5:1: error: \n      use of undeclared identifier '"'"'x'"'"'\n    5 | int e;\n      | ^\n'
printf 'src/a.c:6:1: warning: no member named\n      '"'"'y'"'"' in '"'"'S'"'"'\n    6 | int f;\n'
printf 'src/a.c:7:1: error: not wrapped\n'
exit 2
EOF
printf 'wrapped:\n\tsh wrapped.sh\n' >>"$F/Makefile"
# CMake's error in a module of its own: at the first frame of the call
# stack under it that is in the project (relative, or under the folder).
cat >"$F/stack.sh" <<EOF
printf 'CMake Error at /opt/cmake/Modules/CMakeDetermineHIPCompiler.cmake:210 (message):\n  Failed to find ROCm.\nCall Stack (most recent call first):\n  /opt/cmake/Modules/CMakeOther.cmake:3 (include)\n  CMakeLists.txt:9 (project)\n\n\n'
printf 'CMake Warning at /opt/cmake/Modules/Find.cmake:7 (message):\n  First.\n\n  Second.\nCall Stack (most recent call first):\n  $F/cmake/x.cmake:2 (find_package)\n\n'
printf 'CMake Error at cmake/x.cmake:2 (message):\n  In the project.\nCall Stack (most recent call first):\n  CMakeLists.txt:4 (include)\n\n'
printf 'CMake Error at /opt/cmake/Modules/Lone.cmake:1 (message):\n  No stack.\n\n-- Configuring incomplete, errors occurred!\n'
exit 1
EOF
printf 'stack:\n\tsh stack.sh\n' >>"$F/Makefile"
printf 'phantom:\n\t@echo "program_source:26:30: error: use of undeclared identifier '"'"'pulze'"'"'"; exit 1\n' >>"$F/Makefile"
run "$F" "buildrun:make wrapped;buildwait;problemsdump"
check "clang's message wrapped onto the lines under it: joined" 'button: ▶ make wrapped · 2 errors | make wrapped 3 · failed (exit 2) | file src/a.c | >E 5:1 use of undeclared identifier '"'"'x'"'"' | W 6:1 no member named '"'"'y'"'"' in '"'"'S'"'"' | E 7:1 not wrapped'
# With no line numbers (Clang before 17, -fno-diagnostics-show-line-numbers)
# the source line is printed as it is: one six spaces in, with the carets
# under it, is not more of the message.
cat >"$F/snippet.sh" <<'EOF'
printf 'src/b.c:3:16: error: no member named '"'"'yyy'"'"' in '"'"'S'"'"'\n      return s.yyy;\n             ~ ^\n'
printf 'src/b.c:6:11: warning: unused variable '"'"'u'"'"'\n      int u = 3;\n          ^\n'
printf 'src/b.c:8:7: error: \n      wrapped onto the next line\n      f(x);\n        ^\n'
exit 2
EOF
printf 'snippet:\n\tsh snippet.sh\n' >>"$F/Makefile"
run "$F" "buildrun:make snippet;buildwait;problemsdump"
check "a source line six spaces in, with no line number: not joined" 'button: ▶ make snippet · 2 errors | make snippet 3 · failed (exit 2) | file src/b.c | >E 3:16 no member named '"'"'yyy'"'"' in '"'"'S'"'"' | W 6:11 unused variable '"'"'u'"'"' | E 8:7 wrapped onto the next line'
got=$(BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1000x700 --shot "$dir/shot.bmp" --script "workdir:$F;frame;buildrun:make phantom;buildwait;problemsdump;key:F8" 2>&1 >/dev/null | sed -e "s#$dir#~#g" | tail -3 | tr '\n' '/')
check "a problem whose file is nowhere (Metal's run-time program_source): counted and listed, F8 says it and goes nowhere" 'button: ▶ make phantom · 1 error | make phantom 1 · failed (exit 2) | file program_source | >E 26:30 use of undeclared identifier '"'"'pulze'"'"'/blit: Problem 1 of 1: use of undeclared identifier '"'"'pulze'"'"'/'
run "$F" "buildrun:make stack;buildwait;problemsdump"
check "CMake's error in its own module: at the project's frame of its call stack" 'button: ▶ make stack · 3 errors | make stack 4 · failed (exit 2) | file CMakeLists.txt | >E 9 Failed to find ROCm. (from /opt/cmake/Modules/CMakeDetermineHIPCompiler.cmake:210) | file cmake/x.cmake | W 2 First. (from /opt/cmake/Modules/Find.cmake:7) | E 2 In the project. | file /opt/cmake/Modules/Lone.cmake | E 1 No stack.'
# A compiler's notes, the include chain before its problem and GCC's
# instantiation context: the problem's notes, shown under it in the
# Problems tab once it is opened (Right; Left closes it). Real output, as
# g++-16 (Homebrew GCC 16.2.0) and Apple clang 21.0.0 print it for b.cpp
# below (on macOS, not to a terminal).
N="$dir/notes"
mkdir -p "$N/inc"
printf '#pragma once\ninline int f() { return y; }\n' >"$N/inc/a.h"
printf '#pragma once\n#include "a.h"\n' >"$N/inc/c.h"
printf '#include "inc/c.h"\nvoid g();\ntemplate <class T> void h(T t) { t.nope(); }\nint main() {\n    g(1);\n    h(3);\n}\n' >"$N/b.cpp"
cat >"$N/gcc.txt" <<'EOF'
In file included from inc/c.h:2,
                 from b.cpp:1:
inc/a.h: In function 'int f()':
inc/a.h:2:25: error: 'y' was not declared in this scope
    2 | inline int f() { return y; }
      |                         ^
b.cpp: In function 'int main()':
b.cpp:5:6: error: too many arguments to function 'void g()'
    5 |     g(1);
      |     ~^~~
b.cpp:2:6: note: declared here
    2 | void g();
      |      ^
b.cpp: In instantiation of 'void h(T) [with T = int]':
b.cpp:6:6:   required from here
    6 |     h(3);
      |     ~^~~
b.cpp:3:36: error: request for member 'nope' in 't', which is of non-class type 'int'
    3 | template <class T> void h(T t) { t.nope(); }
      |                                  ~~^~~~
EOF
cat >"$N/clang.txt" <<'EOF'
In file included from b.cpp:1:
In file included from ./inc/c.h:2:
./inc/a.h:2:25: error: use of undeclared identifier 'y'
    2 | inline int f() { return y; }
      |                         ^
b.cpp:5:5: error: no matching function for call to 'g'
    5 |     g(1);
      |     ^
b.cpp:2:6: note: candidate function not viable: requires 0 arguments, but 1 was provided
    2 | void g();
      |      ^
b.cpp:3:35: error: member reference base type 'int' is not a structure or union
    3 | template <class T> void h(T t) { t.nope(); }
      |                                  ~^~~~~
b.cpp:6:5: note: in instantiation of function template specialization 'h<int>' requested here
    6 |     h(3);
      |     ^
3 errors generated.
EOF
printf 'gcc:\n\t@cat gcc.txt; exit 1\nclang:\n\t@cat clang.txt; exit 1\n' >"$N/Makefile"
run "$N" "buildrun:make gcc;buildwait;problemsdump"
check "GCC's notes, include chain and instantiation: under their problems, shut (a triangle says so)" 'button: ▶ make gcc · 3 errors | make gcc 3 · failed (exit 2) | file inc/a.h | >▸E 2:25 '"'"'y'"'"' was not declared in this scope | file b.cpp | ▸E 5:6 too many arguments to function '"'"'void g()'"'"' | ▸E 3:36 request for member '"'"'nope'"'"' in '"'"'t'"'"', which is of non-class type '"'"'int'"'"''
open_all="key:cmd+shift+m;key:Right;key:Down;key:Down;key:Down;key:Right;key:Down;key:Down;key:Right;key:Down;key:Down"
run "$N" "buildrun:make gcc;buildwait;$open_all;problemsdump"
got="file ${got#* | file }"
check "opened (Right on each, Down through them): the header's chain, the note, the instantiation" 'file inc/a.h | ▾E 2:25 '"'"'y'"'"' was not declared in this scope |   c.h:2 included here |   b.cpp:1 included here | file b.cpp | ▾E 5:6 too many arguments to function '"'"'void g()'"'"' |   2:6 declared here | ▾E 3:36 request for member '"'"'nope'"'"' in '"'"'t'"'"', which is of non-class type '"'"'int'"'"' |   In instantiation of '"'"'void h(T) [with T = int]'"'"' | >  6:6 required from here'
run "$N" "buildrun:make clang;buildwait;$open_all;problemsdump"
got="file ${got#* | file }"
check "Clang's: its chain a line each, its instantiation a note" 'file inc/a.h | ▾E 2:25 use of undeclared identifier '"'"'y'"'"' |   b.cpp:1 included here |   c.h:2 included here | file b.cpp | ▾E 5:5 no matching function for call to '"'"'g'"'"' |   2:6 candidate function not viable: requires 0 arguments, but 1 was provided | ▾E 3:35 member reference base type '"'"'int'"'"' is not a structure or union | >  6:5 in instantiation of function template specialization '"'"'h<int>'"'"' requested here'
run "$N" "buildrun:make gcc;buildwait;key:cmd+shift+m;key:Right;key:Down;key:Return;frame;statusdump"
check "Enter on a note: its place" 'NORMAL inc/c.h | × 3 ▲ 0 Ln 2, Col 1 Tabs LF C++'
run "$N" "buildrun:make gcc;buildwait;key:cmd+shift+m;key:Right;key:Right;key:Down;key:Left;problemsdump"
got="file ${got#* | file }"
check "Right on an open problem: its first note; Left: back to the problem" 'file inc/a.h | >▾E 2:25 '"'"'y'"'"' was not declared in this scope |   c.h:2 included here |   b.cpp:1 included here | file b.cpp | ▸E 5:6 too many arguments to function '"'"'void g()'"'"' | ▸E 3:36 request for member '"'"'nope'"'"' in '"'"'t'"'"', which is of non-class type '"'"'int'"'"''
run "$N" "buildrun:make gcc;buildwait;key:cmd+shift+m;key:Right;key:Down;key:Left;key:Left;key:Down;problemsdump"
got="file ${got#* | file }"
check "and Left again shuts it: Down goes on to the next problem" 'file inc/a.h | ▸E 2:25 '"'"'y'"'"' was not declared in this scope | file b.cpp | >▸E 5:6 too many arguments to function '"'"'void g()'"'"' | ▸E 3:36 request for member '"'"'nope'"'"' in '"'"'t'"'"', which is of non-class type '"'"'int'"'"''
run "$N" "buildrun:make gcc;buildwait;$open_all;key:Escape;key:F8;key:F8;frame;statusdump"
check "F8 goes through the problems, not their notes" 'NORMAL b.cpp | × 3 ▲ 0 Ln 5, Col 6 Spaces: 4 LF C++'
# A click on a problem's triangle opens it (and again shuts it), where a
# click on the rest of its row goes to it; a click on a note goes to its place.
cell=$(BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1000x700 --info 2>/dev/null | sed -n 's/.*at scale \([0-9.]*\), cell \([0-9]*\) x \([0-9]*\) px.*/\1 \2 \3/p')
at_row() { echo "$cell" | awk -v r="$1" '{ printf "%d", (r * $3 + $3 * 3 / 4) / $1 }'; }  # (the mouse goes by points, the cells by pixels)
cw=$(echo "$cell" | awk '{ printf "%d", $2 / $1 }')
y3=$(at_row 3) y4=$(at_row 4)
run "$N" "buildrun:make gcc;buildwait;key:cmd+shift+m;frame;click:2,$y3;release;frame;problemsdump"
got="file ${got#* | file }"
got=${got%% | file b.cpp*}
check "a click on the triangle: its notes shown" 'file inc/a.h | >▾E 2:25 '"'"'y'"'"' was not declared in this scope |   c.h:2 included here |   b.cpp:1 included here'
run "$N" "buildrun:make gcc;buildwait;key:cmd+shift+m;frame;click:2,$y3;release;frame;click:2,$y3;release;frame;problemsdump"
got="file ${got#* | file }"
got=${got%% | file b.cpp*}
check "and again: hidden" 'file inc/a.h | >▸E 2:25 '"'"'y'"'"' was not declared in this scope'
run "$N" "buildrun:make gcc;buildwait;key:cmd+shift+m;frame;click:2,$y3;release;frame;click:$((cw * 12)),$y4;release;frame;statusdump"
check "a click on a note: its place" 'NORMAL inc/c.h | × 3 ▲ 0 Ln 2, Col 1 Tabs LF C++'
run "$N" "buildrun:make gcc;buildwait;key:cmd+shift+m;frame;click:$((cw * 12)),$y3;release;frame;statusdump"
check "a click on the rest of the row: the problem" 'NORMAL inc/a.h | × 3 ▲ 0 Ln 2, Col 25 Tabs LF C++'
# CMake's build type and target, chosen in the Build tab (Enter on each):
# Release, then the target app of the folder's own (CMake's help lists
# them); Build configures with the type and builds that target alone.
if command -v cmake >/dev/null 2>&1; then
    T="$dir/types"
    mkdir -p "$T"
    printf 'cmake_minimum_required(VERSION 3.20)\nproject(two C)\nadd_executable(app app.c)\nadd_executable(tool tool.c)\n' >"$T/CMakeLists.txt"
    printf 'int main(void) { return 0; }\n' >"$T/app.c"
    cp "$T/app.c" "$T/tool.c"
    (cd "$T" && cmake -S . -B build >/dev/null 2>&1)
    got=$(BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1000x700 --shot "$dir/shot.bmp" --script "workdir:$T;frame;key:cmd+shift+u;builddump;frame;key:Up;key:Up;key:Return;key:Return;key:Down;key:Return;buildwait;finddump;key:Down;key:Return;frame;buildrun:Build;buildwait;builddump" "$T/CMakeLists.txt" 2>&1 >/dev/null | tail -2 | tr '\n' '/')
    got="$got $(sed -n 's/^CMAKE_BUILD_TYPE:STRING=//p' "$T/build/CMakeCache.txt") $(ls "$T/build" | grep -E '^(app|tool)$' | tr '\n' ' ')"
    check "CMake: the build type and a target chosen, and built so" 'pick [] 3 : >all app tool/CMake: >Build Test Clean Configure (last: 0)/ Release app '
    if command -v ninja >/dev/null 2>&1; then
        TN="$dir/types-ninja"
        mkdir -p "$TN/sub"
        printf 'cmake_minimum_required(VERSION 3.20)\nproject(two C)\nadd_library(core STATIC core.c)\nadd_executable(app app.c)\nadd_subdirectory(sub)\n' >"$TN/CMakeLists.txt"
        printf 'add_executable(tool ../app.c)\n' >"$TN/sub/CMakeLists.txt"
        printf 'int main(void) { return 0; }\n' >"$TN/app.c"
        printf 'int core(void) { return 0; }\n' >"$TN/core.c"
        (cd "$TN" && cmake -S . -B build -G Ninja >/dev/null 2>&1)
        run "$TN" "key:cmd+shift+u;builddump;frame;key:Up;key:Return;buildwait;finddump"
        check "with Ninja: the executables of the top folder too (its help has phony names alone)" 'pick [] 4 : >all core tool app'
    fi
    # The targets are listed on a thread of their own (on another machine
    # the listing is a round trip): a slow cmake does not hold the window,
    # and the picker opens once they are listed.
    mkdir -p "$dir/slow"
    printf '#!/bin/sh\nsleep 1\nexec "%s" "$@"\n' "$(command -v cmake)" >"$dir/slow/cmake"
    chmod +x "$dir/slow/cmake"
    path=$PATH
    PATH="$dir/slow:$PATH"
    got=$(BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1000x700 --shot "$dir/shot.bmp" --script "workdir:$T;frame;key:cmd+shift+u;builddump;frame;key:Up;key:Return;finddump;buildwait;finddump" 2>&1 >/dev/null | tail -2 | tr '\n' '/')
    PATH=$path
    check "a slow listing of the targets: the window not held, the picker opened after" 'closed/pick [] 3 : >all app tool/'
    # No cmake to list them with (the user's shell keeps the PATH it is
    # given, which has none): said, not "CMake did not list the targets".
    case $(uname -s) in Darwin) this="this Mac" ;; *) this="this machine" ;; esac
    mkdir -p "$dir/bare"
    ln -s /usr/bin/* /bin/* "$dir/bare/" 2>/dev/null
    rm -f "$dir/bare/cmake"
    printf '#!/bin/sh\nshift\nexec /bin/sh -c "$@"\n' >"$dir/plainsh"
    chmod +x "$dir/plainsh"
    got=$(SHELL="$dir/plainsh" PATH="$dir/bare" BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1000x700 --shot "$dir/shot.bmp" --script "workdir:$T;frame;key:cmd+shift+u;builddump;frame;key:Up;key:Return;buildwait" 2>&1 >/dev/null | tail -1)
    check "no cmake to list the targets: said" "blit: cmake is not installed on $this"
    # A preset project: the target chosen is built (after the preset's name).
    TP="$dir/types-preset"
    mkdir -p "$TP"
    cp "$T/CMakeLists.txt" "$T/app.c" "$T/tool.c" "$TP/"
    printf '{"version": 3, "configurePresets": [{"name": "dev", "binaryDir": "${sourceDir}/build/${presetName}"}], "buildPresets": [{"name": "dev", "configurePreset": "dev"}]}\n' >"$TP/CMakePresets.json"
    (cd "$TP" && cmake --preset dev >/dev/null 2>&1)
    run "$TP" "key:cmd+shift+u;builddump;frame;key:Up;key:Up;key:Return;buildwait;key:Down;key:Return;frame;buildrun:Build (dev);buildwait;builddump"
    got="$got $(ls "$TP/build/dev" | grep -E '^(app|tool)$' | tr '\n' ' ')"
    check "a preset's build: the target chosen, and built so" 'CMake: Configure (dev) >Build (dev) (last: 0) app '
fi

# The sanitizers' reports, as a program run prints them: AddressSanitizer
# (its first frame the system's, the next the program's own), UBSan's
# runtime error, ThreadSanitizer's race, LeakSanitizer's leaks (each a
# problem); and a program really built with AddressSanitizer, run.
S="$dir/san"
mkdir -p "$S"
printf '#include <stdlib.h>\nint main(void)\n{\n    int* p = malloc(4 * sizeof(int));\n    return p[4];\n}\n' >"$S/a.c"
printf 'int f(void);\nint g(void);\nint h(void);\n' >"$S/t.c"
cat >"$S/reports.sh" <<EOF
sleep 0.2
printf '=================================================================\n==4242==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x602000000014 at pc 0x1 bp 0x2 sp 0x3\n'
printf 'READ of size 4 at 0x602000000014 thread T0\n    #0 0x7fff in memcpy (/usr/lib/libc.so.6+0x10)\n    #1 0x5555 in main $S/a.c:5:12\n    #2 0x5556 in start (/usr/lib/dyld+0x20)\n'
printf 'SUMMARY: AddressSanitizer: heap-buffer-overflow $S/a.c:5:12 in main\n'
printf '$S/t.c:2:5: runtime error: signed integer overflow: 2147483647 + 1 cannot be represented in type int\n'
printf '==================\nWARNING: ThreadSanitizer: data race (pid=77)\n  Write of size 4 at 0x7b04 by thread T1:\n    #0 f $S/t.c:1:5 (t+0x12)\n  Previous read of size 4 at 0x7b04 by main thread:\n    #0 g $S/t.c:2:5 (t+0x34)\nSUMMARY: ThreadSanitizer: data race $S/t.c:1:5 in f\n'
printf '==9==ERROR: LeakSanitizer: detected memory leaks\n\nDirect leak of 64 byte(s) in 1 object(s) allocated from:\n    #0 0x1 in malloc (/usr/lib/libasan.so+0x9)\n    #1 0x2 in h $S/t.c:3:5\n\nIndirect leak of 8 byte(s) in 1 object(s) allocated from:\n    #0 0x1 in malloc (/usr/lib/libasan.so+0x9)\n    #1 0x3 in main $S/a.c:3:5\nSUMMARY: AddressSanitizer: 72 byte(s) leaked in 2 allocation(s).\n'
exit 1
EOF
printf 'reports:\n\tsh reports.sh\nasan:\n\tcc -g -O0 -fsanitize=address -o a a.c && ./a\n' >"$S/Makefile"
run "$S" "buildrun:make reports;buildwait;problemsdump"
got=$(printf '%s' "$got" | sed -e 's/^button: [^|]*| [^|]*| //')
check "the sanitizers' reports: where in the program's own code, and what (each with its stack's frames, shut)" 'file a.c | >▸E 5:12 AddressSanitizer: heap-buffer-overflow: READ of size 4 | ▸E 3:5 LeakSanitizer: detected memory leaks: Indirect leak of 8 byte(s) in 1 object(s) | file t.c | E 2:5 runtime error: signed integer overflow: 2147483647 + 1 cannot be represented in type int | ▸E 1:5 ThreadSanitizer: data race: Write of size 4 | ▸E 3:5 LeakSanitizer: detected memory leaks: Direct leak of 64 byte(s) in 1 object(s)'
run "$S" "buildrun:make reports;buildwait;key:cmd+shift+m;key:Right;key:Down;key:Down;key:Down;key:Down;key:Down;key:Right;key:Down;key:Down;key:Down;key:Down;key:Right;key:Down;key:Down;key:Down;key:Down;key:Down;key:Right;problemsdump"
got=$(printf '%s' "$got" | sed -e 's/^button: [^|]*| [^|]*| //')
check "opened: every frame, a heading over each stack, a leak's own stack" 'file a.c | ▾E 5:12 AddressSanitizer: heap-buffer-overflow: READ of size 4 |   READ of size 4 thread T0 |   #0 memcpy (/usr/lib/libc.so.6+0x10) |   5:12 #1 main |   #2 start (/usr/lib/dyld+0x20) | ▾E 3:5 LeakSanitizer: detected memory leaks: Indirect leak of 8 byte(s) in 1 object(s) |   #0 malloc (/usr/lib/libasan.so+0x9) |   3:5 #1 main | file t.c | E 2:5 runtime error: signed integer overflow: 2147483647 + 1 cannot be represented in type int | ▾E 1:5 ThreadSanitizer: data race: Write of size 4 |   Write of size 4 by thread T1 |   1:5 #0 f |   Previous read of size 4 by main thread |   2:5 #0 g | >▾E 3:5 LeakSanitizer: detected memory leaks: Direct leak of 64 byte(s) in 1 object(s) |   #0 malloc (/usr/lib/libasan.so+0x9) |   3:5 #1 h'
if (cd "$S" && printf 'int main(void){return 0;}\n' | cc -fsanitize=address -x c -o /dev/null - 2>/dev/null); then
    run "$S" "buildrun:make asan;buildwait;problemsdump"
    got=$(printf '%s' "$got" | sed -e 's/^button: [^|]*| [^|]*| //' -e 's/E 5:[0-9]* /E 5 /')  # (a column, or none, as the symbolizer has it)
    check "a program built with AddressSanitizer, run: its report read" 'file a.c | >▸E 5 AddressSanitizer: heap-buffer-overflow: READ of size 4'
fi
# On macOS ASan symbolises with atos, into bare names: each the project's
# one file of that name, wherever it is.
AS="$dir/asanbare"
mkdir -p "$AS/src" "$AS/tests"
printf '1\n2\n3\n4\nvoid push(double);\n' >"$AS/src/ring.cpp"
printf '1\n2\nint main() {}\n' >"$AS/tests/test_ring.cpp"
cat >"$AS/report.sh" <<'EOF'
sleep 0.2
printf '==1==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x1 at pc 0x1 bp 0x2 sp 0x3\nWRITE of size 8 at 0x1 thread T0\n'
printf '    #0 0x1 in ring::push(double) ring.cpp:5\n    #1 0x2 in main test_ring.cpp:3\n    #2 0x3 in start (dyld:arm64e+0x1)\n'
printf 'SUMMARY: AddressSanitizer: heap-buffer-overflow ring.cpp:5 in ring::push(double)\n'
exit 1
EOF
printf 'asan:\n\tsh report.sh\n' >"$AS/Makefile"
run "$AS" "buildrun:make asan;buildwait;problemsdump"
check "a bare name: the project's file of it" 'button: ▶ make asan · 1 error | make asan 1 · failed (exit 2) | file src/ring.cpp | >▸E 5 AddressSanitizer: heap-buffer-overflow: WRITE of size 8'
run "$AS" "buildrun:make asan;buildwait;key:F8;frame;statusdump"
check "F8 goes there" 'NORMAL ring.cpp | × 1 ▲ 0 Ln 5, Col 1 Tabs LF C++'
run "$AS" "buildrun:make asan;frame;buildwait;frame;termclick:test_ring.cpp:3;frame;statusdump"
check "and Cmd+click on another frame's" 'NORMAL test_ring.cpp | × 1 ▲ 0 Ln 3, Col 1 LF C++'
# Real reports, as the programs below print them when built with clang's
# -fsanitize=thread and -fsanitize=address (Apple clang 21.0.0, macOS: bare
# names, by atos) and with GCC's (gcc 16.2.1, Arch Linux: full paths, here
# the folder's in place of the one they were built in). A race is one
# problem, its two accesses' stacks (and the threads' making) its notes.
RS="$dir/realsan"
mkdir -p "$RS"
cat >"$RS/race.c" <<'EOF'
#include <pthread.h>
#include <stdio.h>

int counter;

static void bump(void) { counter++; }

static void* worker(void* arg) {
    (void)arg;
    bump();
    return NULL;
}

int main(void) {
    pthread_t a, b;
    pthread_create(&a, NULL, worker, NULL);
    pthread_create(&b, NULL, worker, NULL);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    printf("%d\n", counter);
    return 0;
}
EOF
cat >"$RS/heap.c" <<'EOF'
#include <stdlib.h>

static int last(int* v, int n) { return v[n]; }

int main(void) {
    int* v = malloc(4 * sizeof(int));
    for (int i = 0; i < 4; ++i) v[i] = i;
    int x = last(v, 4);
    free(v);
    return x;
}
EOF
cat >"$RS/race.mac.txt" <<'EOF'
==================
WARNING: ThreadSanitizer: data race (pid=77554)
  Write of size 4 at 0x0001007ec000 by thread T2:
    #0 bump race.c:6 (race:arm64+0x100000780)
    #1 worker race.c:10 (race:arm64+0x10000073c)

  Previous write of size 4 at 0x0001007ec000 by thread T1:
    #0 bump race.c:6 (race:arm64+0x100000780)
    #1 worker race.c:10 (race:arm64+0x10000073c)

  Location is global 'counter' at 0x0001007ec000 (race+0x100008000)

  Thread T2 (tid=13477301, running) created by main thread at:
    #0 pthread_create <null> (libclang_rt.tsan_osx_dynamic.dylib:arm64e+0x33bac)
    #1 main race.c:17 (race:arm64+0x1000006a8)

  Thread T1 (tid=13477300, finished) created by main thread at:
    #0 pthread_create <null> (libclang_rt.tsan_osx_dynamic.dylib:arm64e+0x33bac)
    #1 main race.c:16 (race:arm64+0x100000690)

SUMMARY: ThreadSanitizer: data race race.c:6 in bump
==================
2
ThreadSanitizer: reported 1 warnings
EOF
cat >"$RS/race.lin.txt" <<'EOF'
==================
WARNING: ThreadSanitizer: data race (pid=153459)
  Read of size 4 at 0x562e1a301064 by thread T2:
    #0 bump @DIR@/race.c:6 (race+0x11ec) (BuildId: 4c88c9be284ec0ac009da024f791b809167a904b)
    #1 worker @DIR@/race.c:10 (race+0x1234) (BuildId: 4c88c9be284ec0ac009da024f791b809167a904b)
    #2 <null> <null> (libtsan.so.2+0x557e9) (BuildId: fad1b3f53c846e11570baa72e153cca909e3f7d2)

  Previous write of size 4 at 0x562e1a301064 by thread T1:
    #0 bump @DIR@/race.c:6 (race+0x1204) (BuildId: 4c88c9be284ec0ac009da024f791b809167a904b)
    #1 worker @DIR@/race.c:10 (race+0x1234) (BuildId: 4c88c9be284ec0ac009da024f791b809167a904b)
    #2 <null> <null> (libtsan.so.2+0x557e9) (BuildId: fad1b3f53c846e11570baa72e153cca909e3f7d2)

  Location is global 'counter' of size 4 at 0x562e1a301064 (race+0x4064)

  Thread T2 (tid=153462, running) created by main thread at:
    #0 pthread_create <null> (libtsan.so.2+0x616c9) (BuildId: fad1b3f53c846e11570baa72e153cca909e3f7d2)
    #1 main @DIR@/race.c:17 (race+0x12a5) (BuildId: 4c88c9be284ec0ac009da024f791b809167a904b)

  Thread T1 (tid=153461, finished) created by main thread at:
    #0 pthread_create <null> (libtsan.so.2+0x616c9) (BuildId: fad1b3f53c846e11570baa72e153cca909e3f7d2)
    #1 main @DIR@/race.c:16 (race+0x1288) (BuildId: 4c88c9be284ec0ac009da024f791b809167a904b)

SUMMARY: ThreadSanitizer: data race @DIR@/race.c:6 in bump
==================
2
ThreadSanitizer: reported 1 warnings
EOF
cat >"$RS/heap.mac.txt" <<'EOF'
=================================================================
==77556==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x602000000940 at pc 0x0001045746dc bp 0x00016b88a300 sp 0x00016b88a2f8
READ of size 4 at 0x602000000940 thread T0
    #0 0x0001045746d8 in last heap.c:3
    #1 0x000104574644 in main heap.c:8
    #2 0x00018cae4d78 in start+0x19f8 (dyld:arm64e+0x31d78)

0x602000000940 is located 0 bytes after 16-byte region [0x602000000930,0x602000000940)
allocated by thread T0 here:
    #0 0x000104ebd274 in malloc+0x78 (libclang_rt.asan_osx_dynamic.dylib:arm64e+0x41274)
    #1 0x00010457458c in main heap.c:6
    #2 0x00018cae4d78 in start+0x19f8 (dyld:arm64e+0x31d78)

SUMMARY: AddressSanitizer: heap-buffer-overflow heap.c:3 in last
EOF
cat >"$RS/heap.lin.txt" <<'EOF'
=================================================================
==153463==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x7b7f749e0020 at pc 0x55cd3a9e91e4 bp 0x7fff4b26af90 sp 0x7fff4b26af80
READ of size 4 at 0x7b7f749e0020 thread T0
    #0 0x55cd3a9e91e3 in last @DIR@/heap.c:3
    #1 0x55cd3a9e9272 in main @DIR@/heap.c:8
    #2 0x7f5f75c27780  (/usr/lib/libc.so.6+0x27780) (BuildId: 6961407ef2e7877eabf5ed8c5a39aea9de7194c1)
    #3 0x7f5f75c278b8 in __libc_start_main (/usr/lib/libc.so.6+0x278b8) (BuildId: 6961407ef2e7877eabf5ed8c5a39aea9de7194c1)
    #4 0x55cd3a9e90b4 in _start (@DIR@/heap+0x10b4) (BuildId: f7b92652f5329f7ee6aa7c6893319b1f54644afe)

0x7b7f749e0020 is located 0 bytes after 16-byte region [0x7b7f749e0010,0x7b7f749e0020)
allocated by thread T0 here:
    #0 0x7f5f7612c0c1 in malloc (/usr/lib/libasan.so.8+0x12c0c1) (BuildId: b8a4241051a1621937fdc46e867ba7ecb56d96ea)
    #1 0x55cd3a9e91f9 in main @DIR@/heap.c:6
    #2 0x7f5f75c27780  (/usr/lib/libc.so.6+0x27780) (BuildId: 6961407ef2e7877eabf5ed8c5a39aea9de7194c1)
    #3 0x7f5f75c278b8 in __libc_start_main (/usr/lib/libc.so.6+0x278b8) (BuildId: 6961407ef2e7877eabf5ed8c5a39aea9de7194c1)
    #4 0x55cd3a9e90b4 in _start (@DIR@/heap+0x10b4) (BuildId: f7b92652f5329f7ee6aa7c6893319b1f54644afe)

SUMMARY: AddressSanitizer: heap-buffer-overflow @DIR@/heap.c:3 in last
EOF
for f in race.lin heap.lin; do sed "s#@DIR@#$RS#g" "$RS/$f.txt" >"$RS/$f.out"; done
printf 'tsan:\n\t@cat race.mac.txt; exit 1\ntsan-gcc:\n\t@cat race.lin.out; exit 1\nasan:\n\t@cat heap.mac.txt; exit 1\nasan-gcc:\n\t@cat heap.lin.out; exit 1\n' >"$RS/Makefile"
opened="buildwait;key:cmd+shift+m;key:Right;problemsdump"
run "$RS" "buildrun:make tsan;$opened"
check "ThreadSanitizer's race (clang, macOS): one problem, both accesses' stacks and the threads' under it" 'button: ▶ make tsan · 1 error | make tsan 1 · failed (exit 2) | file race.c | >▾E 6 ThreadSanitizer: data race: Write of size 4 |   Write of size 4 by thread T2 |   6 #0 bump |   10 #1 worker |   Previous write of size 4 by thread T1 |   6 #0 bump |   10 #1 worker |   Thread T2 (tid=13477301, running) created by main thread at |   #0 pthread_create <null> (libclang_rt.tsan_osx_dynamic.dylib:arm64e+0x33bac) |   17 #1 main |   Thread T1 (tid=13477300, finished) created by main thread at |   #0 pthread_create <null> (libclang_rt.tsan_osx_dynamic.dylib:arm64e+0x33bac) |   16 #1 main'
run "$RS" "buildrun:make tsan-gcc;$opened"
check "and GCC's (Linux): its build ids left out" 'button: ▶ make tsan-gcc · 1 error | make tsan-gcc 1 · failed (exit 2) | file race.c | >▾E 6 ThreadSanitizer: data race: Read of size 4 |   Read of size 4 by thread T2 |   6 #0 bump |   10 #1 worker |   #2 <null> <null> (libtsan.so.2+0x557e9) |   Previous write of size 4 by thread T1 |   6 #0 bump |   10 #1 worker |   #2 <null> <null> (libtsan.so.2+0x557e9) |   Thread T2 (tid=153462, running) created by main thread at |   #0 pthread_create <null> (libtsan.so.2+0x616c9) |   17 #1 main |   Thread T1 (tid=153461, finished) created by main thread at |   #0 pthread_create <null> (libtsan.so.2+0x616c9) |   16 #1 main'
run "$RS" "buildrun:make asan;$opened"
check "AddressSanitizer's (clang, macOS): the access's stack, and the allocation's" 'button: ▶ make asan · 1 error | make asan 1 · failed (exit 2) | file heap.c | >▾E 3 AddressSanitizer: heap-buffer-overflow: READ of size 4 |   READ of size 4 thread T0 |   3 #0 last |   8 #1 main |   #2 start+0x19f8 (dyld:arm64e+0x31d78) |   allocated by thread T0 here |   #0 malloc+0x78 (libclang_rt.asan_osx_dynamic.dylib:arm64e+0x41274) |   6 #1 main |   #2 start+0x19f8 (dyld:arm64e+0x31d78)'
run "$RS" "buildrun:make asan-gcc;$opened"
check "and GCC's (Linux): a frame with no name, its module" 'button: ▶ make asan-gcc · 1 error | make asan-gcc 1 · failed (exit 2) | file heap.c | >▾E 3 AddressSanitizer: heap-buffer-overflow: READ of size 4 |   READ of size 4 thread T0 |   3 #0 last |   8 #1 main |   #2 (/usr/lib/libc.so.6+0x27780) |   #3 __libc_start_main (/usr/lib/libc.so.6+0x278b8) |   #4 _start (~/realsan/heap+0x10b4) |   allocated by thread T0 here |   #0 malloc (/usr/lib/libasan.so.8+0x12c0c1) |   6 #1 main |   #2 (/usr/lib/libc.so.6+0x27780) |   #3 __libc_start_main (/usr/lib/libc.so.6+0x278b8) |   #4 _start (~/realsan/heap+0x10b4)'
run "$RS" "buildrun:make tsan;buildwait;key:cmd+shift+m;key:Right;key:Down;key:Down;key:Down;key:Down;key:Down;key:Down;key:Return;frame;statusdump"
check "Enter on the other access's frame: there" 'NORMAL race.c | × 1 ▲ 0 Ln 10, Col 1 Spaces: 4 LF C'
printf 'one\ntwo\n' >"$dir/notes.txt"
export BLIT_TERM_SHELL="exec env PS1='\$ ' /bin/sh -i"
run "$dir/none" "open:$dir/notes.txt;frame;term;frame;text:echo ../notes.txt:2:3: here;key:Return;termwait:here;frame;termclick:notes.txt:2;frame;statusdump"
unset BLIT_TERM_SHELL
got=$(printf '%s' "$got" | sed 's#NORMAL [^ ]*notes.txt#NORMAL notes.txt#')
check "Cmd+click in any terminal (a shell's)" 'NORMAL notes.txt | Ln 2, Col 3 Tabs LF Plain text'

# Test frameworks' output, sizes and timing, as a task prints them: each
# failure a problem at its place, and what the run came to on its task's
# row. tests/output has the real output of each: GoogleTest 1.17.0, Catch2
# 3.8.1, doctest 2.4.12 and Unity 2.6.1 programs (one test passing, one
# failing, one skipped) built with GCC 16 on Linux (Apple clang 21 gave
# the same); CTest 4.5 on macOS; arm-none-eabi-size and ld
# --print-memory-usage (binutils 2.40, Debian 12's gcc-arm-none-eabi
# 12.2) of a firmware linked twice, the second with 120 more bytes of
# table; nextpnr-ice40 0.11.1 (hdlc/nextpnr) placing a small design at
# 12 MHz and at 400 MHz. Vivado is not on either machine: its timing
# summary below is the shape its documentation (UG906, UG835's
# report_timing_summary) gives, with made-up figures.
O="$(cd "$(dirname "$0")" && pwd)/output"
TF="$dir/tf"
mkdir -p "$TF/constrs"
for k in gtest catch2 doctest unity ctest size-arm nextpnr-pass; do printf '%s:\n\t@cat "%s/%s.txt"; exit 1\n' "$k" "$O" "$k"; done >"$TF/Makefile"
printf 'fw:\n\t@if [ -e .built ]; then cat "%s/ld-memory-bigger.txt"; else cat "%s/ld-memory.txt"; touch .built; fi\n' "$O" "$O" >>"$TF/Makefile"
printf 'once:\n\t@if [ -e .ran ]; then echo nothing to say; else cat "%s/unity.txt"; touch .ran; fi\n' "$O" >>"$TF/Makefile"
printf 'pnr:\n\t./nextpnr-ice40 --hx1k --package tq144 --json blink.json --pcf blink.pcf --freq 400 --asc blink.asc\nnopcf:\n\t@./nextpnr-ice40\nvivado:\n\t@cat vivado.txt\n' >>"$TF/Makefile"
printf '#!/bin/sh\ncat "%s/nextpnr-fail.txt"\nexit 1\n' "$O" >"$TF/nextpnr-ice40"
chmod +x "$TF/nextpnr-ice40"
cat >"$TF/vivado.txt" <<EOF
Parsing XDC File [$TF/ip/clk_wiz/clk_wiz.xdc] for cell 'clk_wiz/inst'
Parsing XDC File [$TF/constrs/top.xdc]
Finished Parsing XDC File [$TF/constrs/top.xdc]
------------------------------------------------------------------------------------------------
| Design Timing Summary
| ---------------------
------------------------------------------------------------------------------------------------

    WNS(ns)      TNS(ns)  TNS Failing Endpoints  TNS Total Endpoints      WHS(ns)      THS(ns)  THS Failing Endpoints  THS Total Endpoints     WPWS(ns)     TPWS(ns)  TPWS Failing Endpoints  TPWS Total Endpoints
    -------      -------  ---------------------  -------------------      -------      -------  ---------------------  -------------------     --------     --------  ----------------------  --------------------
     -0.842      -12.375                     31                  845        0.124        0.000                      0                  845        4.020        0.000                       0                   412


Timing constraints are not met.
EOF
last() { got=$(printf '%s' "$got" | sed 's/^.*\((last: \)/\1/'); }  # (builddump's: how the last run went, and what it came to)
run "$TF" "buildrun:make gtest;buildwait;problemsdump"
check "GoogleTest: each failure at its place, the test and its expectation" 'button: ▶ make gtest · 2 errors | make gtest 2 · failed (exit 2) | file tests/gtest_math.cpp | >E 9 Math.Breaks: Expected equality of these values: add(1, 2); Which is: 3; 4 | E 10 Math.Breaks: Value of: add(2, 2) == 5; Actual: false; Expected: true; two and two'
run "$TF" "buildrun:make gtest;buildwait;builddump"
last
check "and its count on the task's row" '(last: 2) (1 passed, 1 failed, 1 skipped)'
run "$TF" "buildrun:make catch2;buildwait;problemsdump"
check "Catch2: the expression and its expansion" 'button: ▶ make catch2 · 2 errors | make catch2 2 · failed (exit 2) | file tests/catch_math.cpp | >E 10 breaks on purpose: CHECK( x == 4 ); with expansion: 3 == 4 | E 11 breaks on purpose: REQUIRE( add(2, 2) == 5 ); with expansion: 4 == 5'
run "$TF" "buildrun:make catch2;buildwait;builddump"
last
check "and Catch2's count" '(last: 2) (1 passed, 1 failed, 1 skipped)'
run "$TF" "buildrun:make doctest;buildwait;problemsdump"
check "doctest's failures, with their values" 'button: ▶ make doctest · 2 errors | make doctest 2 · failed (exit 2) | file tests/doctest_math.cpp | >E 10 breaks on purpose: CHECK( add(1, 2) == 4 ) is NOT correct! values: CHECK( 3 == 4 ) | E 11 breaks on purpose: REQUIRE_EQ( add(2, 2), 5 ) is NOT correct! values: REQUIRE_EQ( 4, 5 )'
run "$TF" "buildrun:make doctest;buildwait;builddump"
last
check "and doctest's count" '(last: 2) (1 passed, 1 failed, 1 skipped)'
run "$TF" "buildrun:make unity;buildwait;problemsdump"
check "Unity's failure" 'button: ▶ make unity · 1 error | make unity 1 · failed (exit 2) | file tests/test_unity.c | >E 12 test_breaks: Expected 4 Was 3'
run "$TF" "buildrun:make unity;buildwait;builddump"
last
check "and Unity's count" '(last: 2) (1 passed, 1 failed, 1 skipped)'
run "$TF" "buildrun:make ctest;buildwait;builddump"
last
check "CTest's count, from its line for each test (the one skipped its own count calls passed)" '(last: 2) (1 passed, 1 failed, 1 skipped)'
got=$(BLIT_INIT="$dir/init.lua" "$blit" --offscreen 3000x700 --shot "$dir/shot.bmp" --script "workdir:$TF;frame;buildrun:make unity;buildwait;problemsdump" 2>&1 >/dev/null | tail -1)
check "on the Run button too, where it has room" 'button: ▶ make unity · 1 error · 1 passed, 1 failed, 1 skipped | make unity 1 · failed (exit 2) | file tests/test_unity.c | >E 12 test_breaks: Expected 4 Was 3'
run "$TF" "buildrun:make once;buildwait;builddump"
last
check "what a run came to" '(last: 0) (1 passed, 1 failed, 1 skipped)'
run "$TF" "buildrun:make once;buildwait;builddump"
last
check "gone when the task runs again and says nothing of it" '(last: 0)'
run "$TF" "buildrun:make size-arm;buildwait;builddump"
last
check "arm-none-eabi-size's table" '(last: 2) (text 4 KB, data 4 B, bss 3 KB)'
run "$TF" "buildrun:make fw;buildwait;buildrun:make fw;buildwait;builddump"
last
check "the linker's memory regions, and the change since the task's last run" '(last: 0) (FLASH 4.1 KB / 256 KB (1.6%) +120 B, RAM 3 KB / 64 KB (4.7%))'
run "$TF" "buildrun:make fw;buildwait;builddump"
last
check "(kept for the session: nothing to set the first run after a start against)" '(last: 0) (FLASH 4.1 KB / 256 KB (1.6%), RAM 3 KB / 64 KB (4.7%))'
run "$TF" "buildrun:make nextpnr-pass;buildwait;builddump"
last
check "nextpnr's clock, after routing, and its slack" '(last: 2) (clk 122.7 MHz, slack +75.19 ns)'
run "$TF" "buildrun:make pnr;buildwait;builddump"
last
check "a clock that misses" '(last: 2) (clk 122.7 MHz, slack -5.65 ns)'
run "$TF" "buildrun:make pnr;buildwait;problemsdump"
check "an error (nextpnr stopped for it) at the constraints file make echoed nextpnr's command with" 'button: ▶ make pnr · 1 error | make pnr 1 · failed (exit 2) | file blink.pcf | >E 1 Timing not met: clock '"'"'clk'"'"' at 122.73 MHz, 400.00 MHz asked for (slack -5.65 ns)'
run "$TF" "buildrun:make nopcf;buildwait;problemsdump"
check "with none named: under the tool's name" 'button: ▶ make nopcf · 1 error | make nopcf 1 · failed (exit 2) | file nextpnr | >E 1 Timing not met: clock '"'"'clk'"'"' at 122.73 MHz, 400.00 MHz asked for (slack -5.65 ns)'
run "$TF" "buildrun:make vivado;buildwait;problemsdump"
check "Vivado's: at the project's constraints file (not an IP's)" 'button: ▶ make vivado · 1 warning | make vivado 1 · done | file constrs/top.xdc | >W 1 Timing constraints are not met: WNS -0.842 ns, TNS -12.375 ns (31 failing endpoints)'
run "$TF" "buildrun:make vivado;buildwait;builddump"
last
check "and its summary" '(last: 0) (WNS -0.842 ns, TNS -12.375 ns, WHS +0.124 ns)'

# New projects.
P="$dir/projects"
mkdir -p "$P/taken"
: >"$P/taken/x"
run "$dir/none" "newproject;startdump"
check "the New project tab: C++ first, with its fields" 'new project: kind=C++ name= where=~/Dev git=on | target=Program std=23 libraries= tests=CTest warnings=Standard sanitizers=Address + undefined tuning=Portable lto=false generator=Ninja presets=true clangd=true tidy=false format=None readme=true configure=true | at 0'
run "$dir/none" "newproject;key:Down;key:Down;key:Down;key:Down;key:Right;key:Right;projectdump"
check "keys: down to Builds, a choice along" 'kind=C++ name= where=~/Dev git=on | target=Shared library std=23 libraries= tests=CTest warnings=Standard sanitizers=Address + undefined tuning=Portable lto=false generator=Ninja presets=true install=true clangd=true tidy=false format=None readme=true configure=true | at 4'
run "$dir/none" "newproject;key:Down;key:Down;key:Down;key:Down;key:Left;projectdump"
check "a field shown only for some values: Install rules, for a library" 'kind=C++ name= where=~/Dev git=on | target=Header-only library std=23 libraries= tests=CTest warnings=Standard sanitizers=Address + undefined tuning=Portable lto=false generator=Ninja presets=true install=true clangd=true tidy=false format=None readme=true configure=true | at 4'
run "$dir/none" "newproject;projectcreate;projectwait;projectdump"
check "no name" 'kind=C++ name= where=~/Dev git=on | target=Program std=23 libraries= tests=CTest warnings=Standard sanitizers=Address + undefined tuning=Portable lto=false generator=Ninja presets=true clangd=true tidy=false format=None readme=true configure=true | at 0 | error: It needs a name first.'
run "$dir/none" "newproject;project:where=$P;project:name=taken;projectcreate;projectwait;startdump"
check "a folder that is there" 'new project: kind=C++ name=taken where=~/projects git=on | target=Program std=23 libraries= tests=CTest warnings=Standard sanitizers=Address + undefined tuning=Portable lto=false generator=Ninja presets=true clangd=true tidy=false format=None readme=true configure=true | at 0 | error: ~/projects/taken is there already.'
run "$dir/none" "newproject;project:where=$P;project:libraries=fmt zlib;project:configure=false;project:git=false;project:name=app;projectcreate;projectwait;frame;statusdump"
check "made: the start screen goes, its main file is open" 'NORMAL src/main.cpp | Ln 1, Col 1 Spaces: 4 LF C++'
got=$(cd "$P/app" && find . -type f | LC_ALL=C sort | tr '\n' ' ')
check "the files: headers in include/, sources in src/" './.clangd ./CMakeLists.txt ./CMakePresets.json ./README.md ./include/app/app.hpp ./src/app.cpp ./src/main.cpp ./tests/test_app.cpp ./vcpkg.json '
got=$(grep -E 'fmt|zlib|TOOLCHAIN_FILE "' "$P/app/CMakeLists.txt" | tr '\n' '|')
check "vcpkg: the toolchain, fmt by its usage notes (the first way), zlib by none" '  set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "vcpkg'"'"'s toolchain")|find_package(fmt CONFIG REQUIRED)|target_link_libraries(app_core PUBLIC fmt::fmt)|# zlib: vcpkg says what to find_package and link (target_link_libraries(app_core PUBLIC ...)) when it installs it.|'
got=$(tr -d ' \n' <"$P/app/vcpkg.json")
check "vcpkg.json" '{"name":"app","version-string":"0.1.0","dependencies":["fmt","zlib"]}'
run "$dir/none" "newproject;project:where=$P;project:kind=C;project:build=Make;project:name=cm;projectcreate;projectwait;buildwait;builddump"
check "C with make: made, git init run, and the Build tab has it" 'Make: >make make all make run make test make release make clean (last: 0)'
[ -d "$P/cm/.git" ] && got=git || got='no .git'
check "git init" 'git'
run "$P/cm" "buildrun:make test;buildwait;builddump"
check "and it builds, and its test passes" 'Make: make make all make run >make test make release make clean (last: 0)'
run "$dir/none" "newproject;project:where=$P;project:kind=Python;project:manager=pip;project:type=Script;project:install=false;project:name=tool;projectcreate;projectwait;projectdump"
check "Python: a script with pip" 'kind=Python name=tool where=~/projects git=on | manager=pip type=Script python=3.13 backend_pip=hatchling deps= ruff=true types=mypy readme=true install=false | at 0 | made ~/projects/tool'
run "$dir/none" "newproject;project:where=$P;project:kind=HIP;project:configure=false;project:git=false;project:name=stencil;projectcreate;projectwait"
run "$P/stencil" "builddump"
got="$got | $(sed -n 's/^ *CompilationDatabase: //p' "$P/stencil/.clangd")"
check "HIP: the Run button builds release, the preset it configures and .clangd reads" 'CMake: Configure (release) Configure (debug) Configure (ci) >Build (release) Build (debug) Build (ci) Test (release) Test (debug) Test (ci) | build/release'

printf 'notes\n' >"$dir/notes.txt"
run "$dir/none" "open:$dir/notes.txt;frame;key:cmd+alt+n;frame;startdump"
check "New project (Cmd+Option+N) once a file is open: the form, in a tab of its own" 'new project: kind=C++ name= where=~/Dev git=on | target=Program std=23 libraries= tests=CTest warnings=Standard sanitizers=Address + undefined tuning=Portable lto=false generator=Ninja presets=true clangd=true tidy=false format=None readme=true configure=true | at 0'
run "$dir/none" "open:$dir/notes.txt;frame;key:cmd+alt+n;frame;key:Escape;frame;startdump"
got=${got%% o:*}
check "and Esc there: the start screen's Start tab" 'start >n:New file p:New project…'

# init.lua's own kinds, and one taken away.
cat >"$dir/kind.lua" <<'EOF'
blit.opt.lsp.enabled = false
blit.opt.git.enabled = false
blit.project.add("Notes", {description = "Markdown notes",
  fields = {{key = "title", label = "Title", default = "untitled"}, {key = "toc", label = "Contents", kind = "switch", default = true}},
  create = function(dir, v)
    blit.fs.write(dir .. "/notes.md", "# " .. v.title .. (v.toc and "\n\n[TOC]\n" or "\n"))
    return "notes.md"
  end})
blit.project.remove("Python")
EOF
INIT="$dir/kind.lua"
run "$dir/none" "newproject;project:where=$P;project:kind=Python;projectdump"
check "a kind taken away is not there" 'kind=C++ name= where=~/projects git=on | target=Program std=23 libraries= tests=CTest warnings=Standard sanitizers=Address + undefined tuning=Portable lto=false generator=Ninja presets=true clangd=true tidy=false format=None readme=true configure=true | at 0'
INIT="$dir/kind.lua"
run "$dir/none" "newproject;project:where=$P;project:kind=Notes;project:title=Ideas;project:git=false;project:name=notes;projectcreate;projectwait;frame;statusdump"
check "blit.project.add: its create made it, and its file is open" 'NORMAL notes.md | Ln 1, Col 1 Tabs LF Markdown'
got=$(tr '\n' '|' <"$P/notes/notes.md")
check "with the values set" '# Ideas||[TOC]|'

# Run configurations: .blit/run.json beside the project, run by the Run
# button (its build first), and what Debug and Profile run.
R="$dir/rc"
mkdir -p "$R/.blit" "$R/build"
cat >"$R/Makefile" <<'EOF'
all:
	@printf '#!/bin/sh\necho args: "$$*" greeting: "$$GREETING" in: "$$(pwd)"\n' > build/hello && chmod +x build/hello && echo built
EOF
cat >"$R/.blit/run.json" <<'EOF'
{ "configurations": [
  { "name": "hello", "program": "build/hello", "args": ["one", "two words"], "env": { "GREETING": "hi there" }, "cwd": "build", "build": "make" },
  { "name": "plain", "program": "build/hello", "args": "x y" } ] }
EOF
INIT=
run "$R" "builddump"
check "run configurations, before what the folder builds with; the first is the Run button's" 'Run: >hello plain | Make: make make all'
run "$R" "runconfdump"
check "Debug and Profile take its program, arguments, folder and environment" "hello | debug: hello one 'two words' in build GREETING=hi there | profile: hello one 'two words' in build GREETING=hi there"
run "$R" "buildrun;buildwait;builddump"
check "Run: its build, then it" 'Run: >hello plain | Make: make make all (last: 0)'
if test -x "$R/build/hello"; then got=built; else got=missing; fi
check "(the build ran first)" 'built'
run "$R" "buildrun:plain;buildwait;runconfdump"
check "another chosen: theirs too" 'plain | debug: hello x y in rc | profile: hello x y in rc'
run "$R" "builddump;buildpanel;frame;key:Home;key:Down;key:space;frame;runconfdump"
check "and chosen with Space, not run: theirs too" 'plain | debug: hello x y in rc | profile: hello x y in rc'
printf '{ "configurations": [ { "name": "x" } ] }\n' >"$R/.blit/run.json"
got=$(BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1000x700 --shot "$dir/shot.bmp" --script "workdir:$R;frame;builddump" 2>&1 >/dev/null | grep '^blit: ' | tail -1)
check "a configuration without its program: said" 'blit: .blit/run.json: a configuration needs a "name" and a "program"'
mkdir -p "$dir/fresh"
printf 'all:\n\ttrue\n' >"$dir/fresh/Makefile"
run "$dir/fresh" "command:Edit the run configurations;frame;lsptext"
check "Edit the run configurations: made, with one to fill in" '{ /   "configurations": [ /     { /       "name": "fresh", /       "program": "build/fresh", /       "args": [], /       "env": {}, /       "cwd": "." /     } /   ] / } / '

# A file's assembly beside it: from its own command in a build folder's
# compile_commands.json (its -D is needed), linked line by line, and made
# again when the file is saved; with no such command, the plain compiler's.
A="$dir/asm"
mkdir -p "$A/build" "$A/plain"
printf '// sums\nint sum(const int* a, int n) {\n    int s = 0;\n    for (int i = 0; i < n; ++i) s += a[i];\n    return s;\n}\n#ifndef STEP\n#error no STEP\n#endif\n' >"$A/sum.c"
printf '[{"directory": "%s", "command": "cc -O0 -DSTEP=1 -c sum.c -o build/sum.o", "file": "sum.c"}]\n' "$A" >"$A/build/compile_commands.json"
run "$A" "open:$A/sum.c;command:Show the assembly beside the file;asmdump"
check "a comment's line: none of the assembly" '1:'
first=$(BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1000x700 --shot "$dir/shot.bmp" --script "workdir:$A;open:$A/sum.c;command:Show the assembly beside the file;key:Down;key:Down;key:Down;asmdump;vim:O<Esc>j;save;asmdump" 2>&1 >/dev/null | tail -2 | tr '\n' '|')
loop=${first%%|*}
case $loop in "4: "[0-9]*) got=lines ;; *) got=$loop ;; esac
check "the loop's line: its instructions" 'lines'
moved=${first#*|}
got=${moved%|}
check "a line put above it and saved: made again, the same instructions now the next line's" "5:${loop#4:}"
AP="$dir/asmpre"
mkdir -p "$AP/build/debug" "$AP/build/release"
printf '// sums\nint sum(const int* a, int n) {\n    int s = 0;\n    for (int i = 0; i < n; ++i) s += a[i];\n    return s;\n}\n#ifndef STEP\n#error no STEP\n#endif\n' >"$AP/sum.c"
printf '[{"directory": "%s", "command": "cc -O0 -DSTEP=1 -c sum.c -o build/debug/sum.o", "file": "sum.c"}]\n' "$AP" >"$AP/build/debug/compile_commands.json"
run "$AP" "open:$AP/sum.c;command:Show the assembly beside the file;key:Down;key:Down;key:Down;asmdump"
case $got in "4: "[0-9]*) got=lines ;; esac
check "a preset's database, one level into build (build/debug)" 'lines'
printf '[{"directory": "%s", "command": "cc -O0 -c sum.c -o build/release/sum.o", "file": "sum.c"}]\n' "$AP" >"$AP/build/release/compile_commands.json"
mv "$AP/build/debug" "$AP/build/zdebug"
printf 'CompileFlags:\n  CompilationDatabase: build/zdebug\n' >"$AP/.clangd"
run "$AP" "open:$AP/sum.c;command:Show the assembly beside the file;key:Down;key:Down;key:Down;asmdump"
case $got in "4: "[0-9]*) got=lines ;; esac
check "of two, the one .clangd names" 'lines'
printf 'int twice(int x) { return 2 * x; }\n' >"$A/plain/two.c"
run "$A/plain" "open:$A/plain/two.c;command:Show the assembly beside the file;asmdump"
case $got in "1: "[0-9]*) got=lines ;; esac
check "no compile_commands.json: the plain compiler's" 'lines'
printf 'int broken( {\n' >"$A/plain/bad.c"
run "$A/plain" "open:$A/plain/bad.c;command:Show the assembly beside the file;asmdump"
case $got in *error*) got=said ;; esac
check "a file that does not compile: the compiler's errors in the tab" 'said'
run "$A/plain" "command:Show the assembly beside the file"
check "a file not saved yet: said" 'blit: The assembly is made from the file on disk: save it first'

exit $failed
