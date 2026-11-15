#!/bin/sh
# Git in the editor, end to end: scratch repositories with a "server" beside
# them (a bare repository on this disk, so nothing leaves the machine), the
# editor driven by scripted keys and by the Git tab's actions, and what git
# then reports to the editor printed and compared. Where it matters the
# repositories themselves are asked too.
#
#   tests/git.sh path/to/blit            run the cases
#   tests/git.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/git.sh path/to/blit [--print]}
print=${2:-}
command -v git >/dev/null 2>&1 || { echo "skipped: no git"; exit 77; }
dir=$(mktemp -d)
dir=$(cd "$dir" && pwd -P)
trap 'rm -rf "$dir"' EXIT
printf 'blit.opt.lsp.enabled = false\nblit.opt.editor.modal = false\n' >"$dir/init.lua"
export BLIT_INIT="$dir/init.lua"
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_SYSTEM=/dev/null

# A server and two people's copies of it, with one commit.
fresh() {
    rm -rf "$dir/server.git" "$dir/ada" "$dir/bo"
    git init -q --bare -b main "$dir/server.git"
    for who in ada bo; do
        git clone -q "$dir/server.git" "$dir/$who" 2>/dev/null
        git -C "$dir/$who" config user.name "$who"
        git -C "$dir/$who" config user.email "$who@example.invalid"
        git -C "$dir/$who" config commit.gpgsign false
    done
    printf 'one\ntwo\nthree\nfour\nfive\n' >"$dir/ada/poem.txt"
    printf 'keep\n' >"$dir/ada/other.txt"
    git -C "$dir/ada" add -A
    git -C "$dir/ada" commit -q -m "Start"
    git -C "$dir/ada" push -q -u origin main 2>/dev/null
    git -C "$dir/bo" pull -q 2>/dev/null
}

failed=0
# check NAME EXPECTED: compares $got with it.
check() {
    if [ -n "$print" ]; then
        printf '%s\n    %s\n' "$1" "$got"
    elif [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$2" "$got"
        failed=1
    fi
}
# run FILE SCRIPT: the last line printed by the script's dumps.
run() {
    got=$("$blit" --offscreen 1200x700 --shot "$dir/shot.bmp" --script "$2" "$1" 2>&1 >/dev/null | grep -v '^blit: git' | tail -1)
}

fresh
run "$dir/ada/poem.txt" "frame;gitdump"
check "a clean copy, and no blame until it is asked for" 'main | branches *main origin/main | commits 1 "Start" | lines'

run "$dir/ada/poem.txt" "frame;gitwait;key:cmd+alt+B;gitwait;gitdump"
check "Cmd+Option+B shows blame (and is not taken for Cmd+B, which hides the side panel)" 'main | branches *main origin/main | commits 1 "Start" | lines | blame ada: Start'
run "$dir/ada/poem.txt" "frame;gitwait;key:cmd+alt+B;gitwait;key:cmd+alt+B;gitwait;gitdump"
check "and again takes it away" 'main | branches *main origin/main | commits 1 "Start" | lines'
run "$dir/ada/poem.txt" "frame;git:blame;gitdump"
check "so does the Git tab's Blame" 'main | branches *main origin/main | commits 1 "Start" | lines | blame ada: Start'
printf 'blit.opt.lsp.enabled = false\nblit.opt.editor.modal = false\nblit.opt.git.blame = true\n' >"$dir/init.lua"
run "$dir/ada/poem.txt" "frame;gitdump"
check "blame is no setting: init.lua cannot turn it on everywhere" 'main | branches *main origin/main | commits 1 "Start" | lines'
printf 'blit.opt.lsp.enabled = false\nblit.opt.editor.modal = false\n' >"$dir/init.lua"

run "$dir/ada/poem.txt" "frame;gitwait;key:cmd+alt+B;key:Down;key:End;text: changed;key:cmd+Down;text:six;key:Return;key:cmd+Up;gitdump"
check "edited and not saved: the changed lines show, git's own status does not" 'main | branches *main origin/main | commits 1 "Start" | lines 2~ 6+ | blame ada: Start'

run "$dir/ada/poem.txt" "frame;gitwait;key:cmd+alt+B;key:Down;key:End;text: changed;key:cmd+Down;text:six;key:Return;key:cmd+S;key:cmd+Up;key:Down;gitdump"
check "saved: a change, and the changed line is nobody's yet" 'main | changes M:poem.txt | branches *main origin/main | commits 1 "Start" | lines 2~ 6+ | blame Not committed yet'

run "$dir/ada/poem.txt" "frame;gitwait;key:cmd+alt+B;git:stage:poem.txt;gitdump:a;git:unstage:poem.txt;gitdump:b;git:stage:poem.txt;git:commit:Second verse;gitdump"
check "staged, unstaged, staged, committed: one commit ahead of the server" 'main +1 | branches *main origin/main | commits 2 "Second verse" | lines | blame ada: Start | note Commit: done'

run "$dir/ada/poem.txt" "frame;gitwait;key:cmd+alt+B;git:push;gitdump"
check "pushed" 'main | branches *main origin/main | commits 2 "Second verse" | lines | blame ada: Start | note Push: done'
got=$(git -C "$dir/server.git" log --format=%s | tr '\n' ',')
check "the server has it" 'Second verse,Start,'

# Bo has not pulled yet; then does, with the file open: it is read again.
run "$dir/bo/poem.txt" "frame;gitwait;key:cmd+alt+B;git:fetch;gitdump:fetched;git:pull;gitdump"
check "fetched (one behind), then pulled" 'main | branches *main origin/main | commits 2 "Second verse" | lines | blame ada: Start | note Pull: done'
run "$dir/bo/poem.txt" "frame;gitwait;key:cmd+alt+B;git:fetch;gitdump"
check "nothing new to fetch" 'main | branches *main origin/main | commits 2 "Second verse" | lines | blame ada: Start | note Fetch: done'

# A file changed under an open tab by git is read again: Ada commits, Bo
# pulls with the file open, types at the end and saves. Both changes must
# be in the file.
printf 'zero\n' >"$dir/ada/poem.new" && cat "$dir/ada/poem.txt" >>"$dir/ada/poem.new" && mv "$dir/ada/poem.new" "$dir/ada/poem.txt"
git -C "$dir/ada" commit -q -a -m "A line at the top" && git -C "$dir/ada" push -q 2>/dev/null
run "$dir/bo/poem.txt" "frame;git:pull;key:cmd+Down;text:seven;key:Return;key:cmd+S;gitdump"
got="$got / $(head -1 "$dir/bo/poem.txt") ... $(tail -1 "$dir/bo/poem.txt")"
check "pulled under an open tab, then edited: nothing lost" 'main | changes M:poem.txt | branches *main origin/main | commits 3 "A line at the top" | lines 8+ | note Pull: done / zero ... seven'
git -C "$dir/bo" checkout -q -- poem.txt

# Branches: a new one, a commit on it, back, merge, delete.
run "$dir/bo/other.txt" "frame;gitwait;key:cmd+alt+B;git:branch:side;gitdump:made;key:End;text: and more;key:cmd+S;git:commit:On the side;git:switch:main;gitdump:back;git:merge:side;gitdump:merged;git:delete:side;gitdump"
check "a branch made, committed on, merged into main and deleted" 'main +1 | branches *main origin/main | commits 4 "On the side" | lines | blame bo: On the side | note Delete branch: Deleted branch side (was '"$(git -C "$dir/bo" rev-parse --short side 2>/dev/null || git -C "$dir/bo" rev-parse --short HEAD)"').'

# Switching branches changes the file under the open tab.
git -C "$dir/bo" switch -q -c old HEAD~3 2>/dev/null && git -C "$dir/bo" switch -q main
run "$dir/bo/poem.txt" "frame;git:switch:old;key:cmd+Down;text:eight;key:Return;key:cmd+S;gitdump"
got="$got / $(head -1 "$dir/bo/poem.txt") ... $(tail -1 "$dir/bo/poem.txt")"
check "another branch checked out under an open tab" 'old | changes M:poem.txt | branches main *old origin/main | commits 1 "Start" | lines 6+ | note Switch: done / one ... eight'
git -C "$dir/bo" checkout -q -- poem.txt && git -C "$dir/bo" switch -q main

# A conflict: both change line three; the second to pull has a merge to sort out.
fresh
sed -i.bak 's/three/three by ada/' "$dir/ada/poem.txt" && rm "$dir/ada/poem.txt.bak"
git -C "$dir/ada" commit -q -a -m "Ada's three" && git -C "$dir/ada" push -q 2>/dev/null
sed -i.bak 's/three/three by bo/' "$dir/bo/poem.txt" && rm "$dir/bo/poem.txt.bak"
git -C "$dir/bo" commit -q -a -m "Bo's three"
git -C "$dir/bo" config pull.rebase false
run "$dir/bo/poem.txt" "frame;gitwait;key:cmd+alt+B;git:pull;gitdump"
got=$(printf '%s' "$got" | sed 's/ | note .*//')
check "a pull that conflicts: a merge under way, the file in conflict, its markers as added lines" 'main +1 -1 merging | conflicts U:poem.txt | branches *main origin/main | commits 2 "Bo'"'"'s three" | lines 3+ 5+3 | blame ada: Start'
run "$dir/bo/poem.txt" "frame;gitwait;key:cmd+alt+B;git:abort;gitdump"
check "the merge given up" 'main +1 -1 | branches *main origin/main | commits 2 "Bo'"'"'s three" | lines | blame ada: Start | note Abort merge: done'

# Throwing changes away; a file git does not know; a diff in a tab.
printf 'draft\n' >"$dir/ada/draft.txt"
run "$dir/ada/other.txt" "frame;gitwait;key:cmd+alt+B;key:End;text: scribble;key:cmd+S;gitdump:dirty;git:discard:other.txt;gitdump"
check "changes discarded (the open tab reads the file again); an untracked file stays" 'main | changes ?:draft.txt | branches *main origin/main | commits 2 "Ada'"'"'s three" | lines | blame ada: Start | note Discard: done'
run "$dir/ada/other.txt" "frame;key:End;text: scribble;key:cmd+S;git:diff:other.txt;gitwait;dump"
check "a diff opens in a tab of its own" '>[other.txt other.txt (changes)*]'
run "$dir/ada/other.txt" "frame;gitwait;key:cmd+alt+B;git:stageall;git:commit:Everything;gitdump"
check "everything staged and committed" 'main +1 | branches *main origin/main | commits 3 "Everything" | lines | blame ada: Everything | note Commit: done'

# Git follows the working directory (the folder the side panel shows), not
# the file in front: a folder chosen there is the repository looked at,
# whatever is open, and with nothing open at all.
printf 'alone\n' >"$dir/alone.txt"
run "$dir/alone.txt" "frame;gitdump:before;workdir:$dir/ada;frame;gitdump"
check "a repository's folder chosen as the working directory, a file from elsewhere open" 'main +1 | branches *main origin/main | commits 3 "Everything" | lines'
got=$("$blit" --offscreen 1200x700 --shot "$dir/shot.bmp" --script "frame;workdir:$dir/bo;frame;gitdump" 2>&1 >/dev/null | grep -v '^blit: git' | tail -1)
check "the same with no file open at all" 'main +1 -1 | branches *main origin/main | commits 2 "Bo'"'"'s three"'
run "$dir/ada/other.txt" "frame;gitdump:own;workdir:$dir/bo;frame;gitdump:other;workdir:$dir;frame;gitdump"
check "another repository chosen, then a folder that is in none" 'no repository'
run "$dir/ada/other.txt" "frame;workdir:$dir/bo;frame;gitdump"
check "a file open from outside the working directory's repository has no marks" 'main +1 -1 | branches *main origin/main | commits 2 "Bo'"'"'s three" | lines'

# Outside any repository.
printf 'alone\n' >"$dir/alone.txt"
run "$dir/alone.txt" "frame;gitdump"
check "a file in no repository" 'no repository'

# The status bar along the bottom: the branch and how it stands, this
# file's changed lines, the files changed, the file, the cursor.
fresh
run "$dir/ada/poem.txt" "frame;gitwait;frame;statusdump"
check "status bar: a clean copy" 'main poem.txt | Ln 1, Col 1 Tabs LF Plain text'
run "$dir/ada/poem.txt" "frame;gitwait;key:Down;text:inserted;key:Return;key:Down;key:Down;key:shift+Down;key:Backspace;frame;gitwait;frame;statusdump"
check "lines added and taken out, not saved" 'main +1 −1 poem.txt ● | Ln 5, Col 1 Tabs LF Plain text'
run "$dir/ada/poem.txt" "frame;gitwait;key:End;text: more;key:cmd+S;frame;gitwait;frame;statusdump"
check "a line changed and saved: one file changed" 'main ~1 1 file changed poem.txt | Ln 1, Col 9 Tabs LF Plain text'
fresh
run "$dir/ada/poem.txt" "frame;gitwait;key:Down;key:shift+Down;key:shift+Right;frame;statusdump"
check "lines selected" 'main poem.txt | Ln 3, Col 2 (2 lines) Tabs LF Plain text'
run "$dir/ada/poem.txt" "frame;gitwait;key:shift+Right;key:shift+Right;frame;statusdump"
check "characters selected" 'main poem.txt | Ln 1, Col 3 (2 selected) Tabs LF Plain text'
printf 'x\n' >>"$dir/ada/other.txt"
git -C "$dir/ada" commit -qam "Ahead"
run "$dir/ada/poem.txt" "frame;gitwait;frame;statusdump"
check "a commit not pushed" 'main ↑1 poem.txt | Ln 1, Col 1 Tabs LF Plain text'
printf 'blit.opt.lsp.enabled = false\nblit.opt.editor.modal = false\nblit.opt.editor.status_bar = false\n' >"$dir/nobar.lua"
got=$(BLIT_INIT="$dir/nobar.lua" "$blit" --offscreen 1200x700 --shot "$dir/shot.bmp" --script "frame;statusdump" "$dir/ada/poem.txt" 2>&1 >/dev/null | tail -1)
check "status_bar = false: none" 'no status bar'
# A click on the branch opens the Git tab of the side panel; one on an
# empty part of the bar does nothing.
shot() { "$blit" --offscreen 1200x700 --scale 1 --shot "$dir/$1.bmp" --script "frame;gitwait;$2;frame;gitwait;frame" "$dir/ada/poem.txt" >/dev/null 2>&1; }
shot none ""
shot branch "click:20,690;release"
shot middle "click:600,690;release"
got="$(cmp -s "$dir/none.bmp" "$dir/branch.bmp" && echo same || echo different) $(cmp -s "$dir/none.bmp" "$dir/middle.bmp" && echo same || echo different)"
check "a click on the branch opens the Git tab; one on nothing in the bar does nothing" 'different same'

[ -n "$print" ] || [ $failed = 1 ] || echo "git ok"
exit $failed
