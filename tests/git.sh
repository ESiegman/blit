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
# The ends of the file: Cmd+Up and Cmd+Down on a Mac, Ctrl+Home and Ctrl+End elsewhere.
case $(uname -s) in Darwin) doc_start=cmd+Up doc_end=cmd+Down ;; *) doc_start=ctrl+Home doc_end=ctrl+End ;; esac

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
    # (The status bar's icons beside a branch and another machine are the
    # font's, where it has them: the words are what is compared.)
    got=$(printf '%s' "$got" | sed -e 's/⎇ //g' -e 's/⇄ //g' -e 's/ //g')
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

run "$dir/ada/poem.txt" "frame;gitwait;key:cmd+alt+B;key:Down;key:End;text: changed;key:$doc_end;text:six;key:Return;key:$doc_start;gitdump"
check "edited and not saved: the changed lines show, git's own status does not" 'main | branches *main origin/main | commits 1 "Start" | lines 2~ 6+ | blame ada: Start'

run "$dir/ada/poem.txt" "frame;gitwait;key:cmd+alt+B;key:Down;key:End;text: changed;key:$doc_end;text:six;key:Return;key:cmd+S;key:$doc_start;key:Down;gitdump"
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
run "$dir/bo/poem.txt" "frame;git:pull;key:$doc_end;text:seven;key:Return;key:cmd+S;gitdump"
got="$got / $(head -1 "$dir/bo/poem.txt") ... $(tail -1 "$dir/bo/poem.txt")"
check "pulled under an open tab, then edited: nothing lost" 'main | changes M:poem.txt | branches *main origin/main | commits 3 "A line at the top" | lines 8+ | note Pull: done / zero ... seven'
git -C "$dir/bo" checkout -q -- poem.txt

# Branches: a new one, a commit on it, back, merge, delete.
run "$dir/bo/other.txt" "frame;gitwait;key:cmd+alt+B;git:branch:side;gitdump:made;key:End;text: and more;key:cmd+S;git:commit:On the side;git:switch:main;gitdump:back;git:merge:side;gitdump:merged;git:delete:side;gitdump"
check "a branch made, committed on, merged into main and deleted" 'main +1 | branches *main origin/main | commits 4 "On the side" | lines | blame bo: On the side | note Delete branch: Deleted branch side (was '"$(git -C "$dir/bo" rev-parse --short side 2>/dev/null || git -C "$dir/bo" rev-parse --short HEAD)"').'

# Switching branches changes the file under the open tab.
git -C "$dir/bo" switch -q -c old HEAD~3 2>/dev/null && git -C "$dir/bo" switch -q main
run "$dir/bo/poem.txt" "frame;git:switch:old;key:$doc_end;text:eight;key:Return;key:cmd+S;gitdump"
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

# Staging part of a file. A file of fourteen lines, changed in two places
# far enough apart to be two hunks: the second line, and three lines added
# after the tenth.
fresh
printf 'one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\neleven\ntwelve\nthirteen\nfourteen\n' >"$dir/ada/long.txt"
git -C "$dir/ada" add long.txt && git -C "$dir/ada" commit -q -m "Long"
changed() { printf 'one\nTWO\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\nA\nB\nC\neleven\ntwelve\nthirteen\nfourteen\n' >"$dir/ada/long.txt"; git -C "$dir/ada" reset -q; }
staged() { git -C "$dir/ada" diff --cached | grep '^[-+@][^-+]' | tr '\n' ' '; }
summary() { printf '%s' "$got" | sed 's| / .*||'; }
# view FILE SCRIPT: the dump of the diff tab (gitview), the lines after the first joined with " / ".
view() {
    got=$("$blit" --offscreen 1200x700 --shot "$dir/shot.bmp" --script "$2" "$1" 2>&1 >/dev/null | grep -v '^blit: git' | awk '/^(changes|staged|other) /{on=1} on{printf "%s%s", n++ ? " / " : "", $0}')
}
changed
view "$dir/ada/long.txt" "frame;gitwait;git:diff:long.txt;gitwait;gitview"
check "a file's diff in a tab, the cursor on its first hunk" 'changes long.txt, one column: 2 hunks, cursor in 1 / diff --git a/long.txt b/long.txt / index 1d1b431..eb3ad39 100644 / --- a/long.txt / +++ b/long.txt / @@ -1,5 +1,5 @@ /  one / [--two-] / {++TWO+} /  three /  four /  five / @@ -8,6 +8,9 @@ seven /  eight /  nine /  ten / {++A+} / {++B+} / {++C+} /  eleven /  twelve /  thirteen'
view "$dir/ada/long.txt" "frame;gitwait;git:diff:long.txt;gitwait;key:Return;gitwait;gitview"
got="$(summary) / $(staged)"
check "Enter stages the hunk the cursor is in, and the cursor goes on to the next" 'changes long.txt, one column: 1 hunks, cursor in 1 / @@ -1,5 +1,5 @@ -two +TWO '
changed
view "$dir/ada/long.txt" "frame;gitwait;git:diff:long.txt;gitwait;key:];key:Down;key:Down;key:Down;key:Down;key:shift+Down;key:Return;gitwait;gitview"
got="$(summary) / $(staged)"
check "a line selected (] to the next hunk), and Enter stages that line alone" 'changes long.txt, one column: 2 hunks, cursor in 2 / @@ -8,6 +8,7 @@ seven +A '
view "$dir/ada/long.txt" "frame;gitwait;git:staged:long.txt;gitwait;key:u;gitwait;gitview"
got="$(summary) / $(staged)"
check "and from what is staged of it, u unstages it again" 'staged long.txt, one column: 0 hunks, cursor in none / '
printf 'blit.opt.lsp.enabled = false\nblit.opt.editor.modal = true\n' >"$dir/vim.lua"
got=$(BLIT_INIT="$dir/vim.lua" "$blit" --offscreen 1200x700 --shot "$dir/shot.bmp" --script "frame;gitwait;git:diff:long.txt;gitwait;key:];vim:jjjjjV;key:s;gitwait" "$dir/ada/long.txt" 2>&1 >/dev/null | grep -v '^blit: git' | tail -1)
got="$got$(staged)"
check "with Vim: Visual mode's lines, and s stages them" '@@ -8,6 +8,7 @@ seven +B '
changed
view "$dir/ada/long.txt" "frame;gitwait;git:diff:long.txt;gitwait;key:shift+\\;gitwait;gitview"
check "| shows it in two columns, each line beside the one it became" 'changes long.txt, two columns: 2 hunks, cursor in 1 / diff --git a/long.txt b/long.txt / index 1d1b431..eb3ad39 100644 / --- a/long.txt / +++ b/long.txt / @@ -1,5 +1,5 @@ /   1 one                           │   1 one /   2 [-two-]                           │   2 {+TWO+} /   3 three                         │   3 three /   4 four                          │   4 four /   5 five                          │   5 five / @@ -8,6 +8,9 @@ seven /   8 eight                         │   8 eight /   9 nine                          │   9 nine /  10 ten                           │  10 ten /                                   │  11 {+A+} /                                   │  12 {+B+} /                                   │  13 {+C+} /  11 eleven                        │  14 eleven /  12 twelve                        │  15 twelve /  13 thirteen                      │  16 thirteen'
view "$dir/ada/long.txt" "frame;gitwait;git:diff:long.txt;gitwait;git:columns;gitwait;key:];key:Down;key:Down;key:Down;key:Down;key:Down;key:shift+Down;key:Return;gitwait;git:columns;gitwait;gitview"
got="$(summary) / $(staged)"
check "a row of two columns staged; and | again, one column" 'changes long.txt, one column: 2 hunks, cursor in 2 / @@ -8,6 +8,7 @@ seven +B '
changed
printf 'int parse(const char* s, int n) {\n    return n;\n}\n' >"$dir/ada/code.c"
git -C "$dir/ada" add code.c && git -C "$dir/ada" commit -q -m "Code"
printf 'int parse(const char* text, int n) {\n    return n;\n}\n' >"$dir/ada/code.c"
view "$dir/ada/code.c" "frame;gitwait;git:diff:code.c;gitwait;git:columns;gitwait;gitview"
got=$(printf '%s' "$got" | sed 's/.* \/ @@ -1,3 +1,3 @@ \/ //; s/ \/ .*//')
check "within a line that changed, only what changed is marked" '  1 int parse(const char* [-s-], int  │   1 int parse(const char* {+text+}, i'
# In a window wide enough for the minimap, the two columns are laid out
# left of it, not under it: narrower than with no minimap.
printf 'blit.opt.lsp.enabled = false\nblit.opt.editor.modal = false\nblit.opt.editor.minimap = false\n' >"$dir/nomap.lua"
# beside FILE SCRIPT [INIT]: in a window that wide, how far in the first
# row of columns its last │ is.
beside() { BLIT_INIT="${3:-$BLIT_INIT}" "$blit" --offscreen 2000x700 --shot "$dir/shot.bmp" --script "$2" "$1" 2>&1 >/dev/null | grep '│' | head -1 | awk -F'│' '{ print length($0) - length($NF) }'; }
narrower() { if [ -n "$1" ] && [ -n "$2" ] && [ "$1" -lt "$2" ]; then echo narrower; else echo "as wide ($1, $2)"; fi; }
got=$(narrower "$(beside "$dir/ada/code.c" "frame;gitwait;git:diff:code.c;gitwait;git:columns;gitwait;gitview")" \
    "$(beside "$dir/ada/code.c" "frame;gitwait;git:diff:code.c;gitwait;git:columns;gitwait;gitview" "$dir/nomap.lua")")
check "two columns beside the minimap: laid out left of it" 'narrower'
git -C "$dir/ada" checkout -q code.c

# A change's mark beside the line numbers: a click on it offers to stage
# it, and once it is, to unstage it; and the diff, at that change.
changed
run "$dir/ada/long.txt" "frame;gitwait;gutterclick:12;gitdump"
check "a click on a change's mark offers to stage it, or to show the diff there" 'main +2 | changes M:long.txt | branches *main origin/main | commits 3 "Code" | lines 2~ 11+3 | offer stage, diff'
run "$dir/ada/long.txt" "frame;gitwait;gutterclick:12;key:Return;gitwait;gitdump"
got="$got / $(staged)"
check "Enter stages it, and it is marked staged" 'main +2 | staged M:long.txt | changes M:long.txt | branches *main origin/main | commits 3 "Code" | lines 2~ 11+3s | note Stage: done / @@ -8,6 +8,9 @@ seven +A +B +C '
run "$dir/ada/long.txt" "frame;gitwait;gutterclick:11;gitdump"
check "a staged change's mark offers to unstage it" 'main +2 | staged M:long.txt | changes M:long.txt | branches *main origin/main | commits 3 "Code" | lines 2~ 11+3s | offer unstage, diff'
run "$dir/ada/long.txt" "frame;gitwait;gutterclick:11;key:Return;gitwait;gitdump"
check "and does" 'main +2 | changes M:long.txt | branches *main origin/main | commits 3 "Code" | lines 2~ 11+3 | note Unstage: done'
view "$dir/ada/long.txt" "frame;gitwait;gutterclick:12;key:Down;key:Return;gitwait;gitview"
got=$(summary)
check "or shows the diff, at that change" 'changes long.txt, one column: 2 hunks, cursor in 2'
run "$dir/ada/long.txt" "frame;gitwait;gutterclick:5;gitdump;gutterclick:2;key:Escape;gitdump"
check "a click beside an unchanged line, or Esc, offers nothing" 'main +2 | changes M:long.txt | branches *main origin/main | commits 3 "Code" | lines 2~ 11+3'
run "$dir/ada/long.txt" "frame;gitwait;key:Down;key:End;text: and more;command:Git: Stage the change at the cursor;gitwait;gitdump"
got="$got / $(git -C "$dir/ada" show :long.txt | sed -n 2p)"
check "the change at the cursor staged from the command search, as the text is, saved or not" 'main +2 | staged M:long.txt | changes M:long.txt | branches *main origin/main | commits 3 "Code" | lines 2~s 11+3 | note Stage: done / TWO and more'
# (With Vim's modes, gS.)
printf 'blit.opt.lsp.enabled = false\n' >"$dir/vim.lua"
changed
BLIT_INIT="$dir/vim.lua"
run "$dir/ada/long.txt" "frame;gitwait;vim:jA and more<Esc>gS;gitwait;gitdump"
BLIT_INIT="$dir/init.lua"
got="$got / $(git -C "$dir/ada" show :long.txt | sed -n 2p)"
check "and with gS in Normal mode" 'main +2 | staged M:long.txt | changes M:long.txt | branches *main origin/main | commits 3 "Code" | lines 2~s 11+3 | note Stage: done / TWO and more'
# A changed line that wraps: the bar beside each of its rows, its number
# on the first alone.
changed
printf 'one\nTWO%s\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\nA\nB\nC\neleven\ntwelve\nthirteen\nfourteen\n' "$(printf ' and a good many more words%.0s' 1 2 3 4 5 6 7 8)" >"$dir/ada/long.txt"
run "$dir/ada/long.txt" "frame;gitwait;command:Toggle soft wrap;frame;gutterdump"
got=$(printf '%s' "$got" | cut -d' ' -f1-9)
check "soft wrap: a change's bar beside each row of its line" 'gutter 1 2~ ~ ~ ~ 3 4 5'

# Stashes: made, listed, shown, popped, applied, dropped.
changed
run "$dir/ada/long.txt" "frame;gitwait;git:stash;gitdump"
check "stashed: the changes put away, and listed" 'main +2 | stashes "WIP on main: '"$(git -C "$dir/ada" rev-parse --short HEAD)"' Code" | branches *main origin/main | commits 3 "Code" | lines | note Stash: done'
run "$dir/ada/long.txt" "frame;gitwait;git:showstash:stash@{0};gitwait;dump"
check "a stash shown in a tab" '>[long.txt stash@{0}*]'
run "$dir/ada/long.txt" "frame;gitwait;git:pop;gitdump"
check "popped: the changes back, the stash gone" 'main +2 | changes M:long.txt | branches *main origin/main | commits 3 "Code" | lines 2~ 11+3 | note Pop stash: done'
run "$dir/ada/long.txt" "frame;gitwait;git:stash;git:apply;gitdump:applied;git:drop;asked;gitdump"
check "applied, then dropped (after asking)" 'main +2 | changes M:long.txt | branches *main origin/main | commits 3 "Code" | lines 2~ 11+3 | note Drop stash: done'
git -C "$dir/ada" checkout -q long.txt

# Another branch's history, a commit of it cherry-picked; tags made, in
# order, pushed and deleted.
git -C "$dir/ada" switch -q -c side && printf 'side\n' >"$dir/ada/side.txt" && git -C "$dir/ada" add side.txt && git -C "$dir/ada" commit -q -m "On the side" && git -C "$dir/ada" switch -q main
run "$dir/ada/poem.txt" "frame;gitwait;git:history:side;gitdump"
check "History shows the branch picked" 'main +2 | branches *main side origin/main | commits 4 of side "On the side" | lines'
run "$dir/ada/poem.txt" "frame;gitwait;git:history:side;git:pick:$(git -C "$dir/ada" rev-parse --short side);git:history;gitdump"
check "a commit of it cherry-picked; then History is the branch checked out's again" 'main +3 | branches *main side origin/main | commits 4 "On the side" | lines | note Cherry-pick: done'
run "$dir/ada/poem.txt" "frame;gitwait;git:tag:v1.9;git:tag:v1.10;git:tag:v1.0;git:pushtag:v1.10;gitdump"
got="$got / $(git -C "$dir/server.git" tag | tr '\n' ' ')"
check "tags made, newest first; one pushed" 'main +3 | branches *main side origin/main | tags v1.10 v1.9 v1.0 | commits 4 "On the side" | lines | note Push tag: done / v1.10 '
run "$dir/ada/poem.txt" "frame;gitwait;git:deletetag:v1.0;asked;gitdump"
check "a tag deleted (after asking)" 'main +3 | branches *main side origin/main | tags v1.10 v1.9 | commits 4 "On the side" | lines | note Delete tag: Deleted tag '"'"'v1.0'"'"' (was '"$(git -C "$dir/ada" rev-parse --short HEAD)"')'

# Rebase onto a branch: it stops at a conflict, settled as a merge's are
# (the file fixed and staged), and is continued. A cherry-pick that
# conflicts is given up.
git -C "$dir/ada" switch -q side
sed -i.bak 's/three/three on the side/' "$dir/ada/poem.txt" && rm "$dir/ada/poem.txt.bak" && git -C "$dir/ada" commit -q -a -m "Side three"
git -C "$dir/ada" switch -q main
sed -i.bak 's/three/three on main/' "$dir/ada/poem.txt" && rm "$dir/ada/poem.txt.bak" && git -C "$dir/ada" commit -q -a -m "Main three"
git -C "$dir/ada" switch -q side
run "$dir/ada/poem.txt" "frame;gitwait;git:rebase:main;asked;gitdump"
got=$(printf '%s' "$got" | sed 's/ | note .*//')
check "a rebase that stops for a conflict" '(no branch) rebasing | conflicts U:poem.txt | branches main side origin/main | tags v1.10 v1.9 | commits 5 "Main three" | lines 3+ 5+3'
printf 'one\ntwo\nthree on both\nfour\nfive\n' >"$dir/ada/poem.txt"
run "$dir/ada/poem.txt" "frame;gitwait;git:stage:poem.txt;git:continue;gitdump"
got="$got / $(git -C "$dir/ada" log --format=%s | tr '\n' ',')"
check "settled, staged, continued: the branch's commits on top of the other's" 'side | branches main *side origin/main | tags v1.10 v1.9 | commits 6 "Side three" | lines | note Continue rebase: done / Side three,Main three,On the side,Code,Long,Start,'
git -C "$dir/ada" switch -q main
root=$(git -C "$dir/ada" rev-list --max-parents=0 HEAD)
git -C "$dir/ada" switch -q -c clash "$root" && sed -i.bak 's/three/three clashing/' "$dir/ada/poem.txt" && rm "$dir/ada/poem.txt.bak" && git -C "$dir/ada" commit -q -a -m "Clash" && git -C "$dir/ada" switch -q main
run "$dir/ada/poem.txt" "frame;gitwait;git:pick:$(git -C "$dir/ada" rev-parse --short clash);gitdump"
got=$(printf '%s' "$got" | sed 's/ | note .*//')
check "a cherry-pick that stops for a conflict" 'main +4 cherry-picking | conflicts U:poem.txt | branches clash *main side origin/main | tags v1.10 v1.9 | commits 5 "Main three" | lines 3+ 5+3'
run "$dir/ada/poem.txt" "frame;gitwait;git:abortpick;gitdump"
check "a cherry-pick that conflicts, given up" 'main +4 | branches clash *main side origin/main | tags v1.10 v1.9 | commits 5 "Main three" | lines | note Abort cherry-pick: done'

# The commit message: Enter makes a new line, Cmd+Enter commits. The
# field grows with the message; past the 50th column of the first line
# the text is muted (the "|"), and so past the 72nd of the others.
fresh
printf 'one\nTWO\nthree\nfour\nfive\n' >"$dir/ada/poem.txt"
long='Make the parser accept a trailing comma in lists of arguments'
run "$dir/ada/poem.txt" "frame;gitwait;key:ctrl+shift+G;frame;text:$long;key:Return;key:Return;text:A body line.;gitfield"
check "a message of three lines: the field grows, and the guide mutes the summary past 50" 'field 5 of 5: Make the parser accept a  / trailing comma in lists o|f  / arguments /  / A body line.^'
run "$dir/ada/poem.txt" "frame;gitwait;key:ctrl+shift+G;frame;text:Fix;key:Return;key:Return;text:Why.;key:Up;key:Up;key:End;text:ed it;key:cmd+Return;gitwait;gitfield"
got="$got / $(git -C "$dir/ada" log -1 --format=%B | tr '\n' '/')"
check "the caret goes up and back; Cmd+Enter commits all of it, and the field is empty again" 'field 1 of 1: ^ / Fixed it//Why.//'
got=$("$blit" --offscreen 1200x700 --shot "$dir/shot.bmp" --script "frame;gitwait;key:ctrl+shift+G;frame;text:x;key:Return;gitwait;gitfield" "$dir/ada/poem.txt" 2>&1 >/dev/null | tail -1)
got="$got / $(git -C "$dir/ada" log --format=%s | wc -l | tr -d ' ')"
check "Enter alone commits nothing" 'field 2 of 2: x / ^ / 2'

# A file's history (through a rename), and the history of the lines chosen:
# each commit with its change, in a diff tab; Enter on one shows all of it.
fresh
printf 'one\ntwo\nTHREE\nfour\nfive\n' >"$dir/ada/poem.txt"
git -C "$dir/ada" commit -qam "Shout three"
git -C "$dir/ada" mv poem.txt verse.txt
git -C "$dir/ada" commit -qm "Call it a verse"
printf 'one\ntwo\nTHREE\nfour\nFIVE\n' >"$dir/ada/verse.txt"
printf 'x\n' >>"$dir/ada/other.txt"
git -C "$dir/ada" commit -qam "Shout five"
# (Its commits by their messages, the tab's title, and the lines added.)
history() { "$blit" --offscreen 1200x700 --shot "$dir/shot.bmp" --script "$2" "$1" 2>&1 >/dev/null | grep -v '^blit: git' |
    awk 'NR == 1 { t = $0; next } /^ *    [A-Z]/ { sub(/^ +/, ""); m = m (m ? ", " : "") $0 } /^ *\+[^+]/ { sub(/^ +/, ""); a = a " " $0 } END { print t " | " m " |" a }'; }
got=$(history "$dir/ada/verse.txt" "frame;gitwait;command:Git: The file's history;gitwait;docdump:200")
check "a file's history, through its rename" 'verse.txt (history) | Shout five, Call it a verse, Shout three, Start | +FIVE +THREE +one +two +three +four +five'
got=$(BLIT_INIT="$dir/vim.lua" history "$dir/ada/verse.txt" "frame;gitwait;vim:gL;gitwait;docdump:200")
check "the same with gL in Normal mode" 'verse.txt (history) | Shout five, Call it a verse, Shout three, Start | +FIVE +THREE +one +two +three +four +five'
got=$(history "$dir/ada/verse.txt" "frame;gitwait;key:Down;key:Down;command:Git: The history of the lines chosen;gitwait;docdump:200")
check "the history of a line" 'verse.txt:3 (history) | Shout three, Start | +THREE +three'
got=$("$blit" --offscreen 1200x700 --shot "$dir/shot.bmp" --script "frame;gitwait;command:Git: The file's history;gitwait;key:Down;key:Return;gitwait;docdump:0" "$dir/ada/verse.txt" 2>&1 >/dev/null | grep -v '^blit: git' | head -2 | tr '\n' '|')
got="$got $(git -C "$dir/ada" log -1 --format=%H | cut -c1-10)"
case "$got" in "$(git -C "$dir/ada" log -1 --format=%H | cut -c1-10)|"*) got="its commit, all of it" ;; esac
check "Enter on a commit in it: all of that commit" 'its commit, all of it'

# History as a graph: a branch made, worked on beside main, merged back and
# tagged (each commit a second after the one before, so that the order is
# the same each time); what points at each commit beside it. Searched by
# message, by author and by a path.
fresh
# (After the first commit, made now.)
step() { n=$1; shift; GIT_AUTHOR_DATE="2099-01-01T00:00:0$n" GIT_COMMITTER_DATE="2099-01-01T00:00:0$n" git -C "$dir/ada" "$@" >/dev/null 2>&1; }
step 1 switch -q -c feature
printf 'new\n' >"$dir/ada/feature.txt"
step 1 add feature.txt
step 1 commit -qm "Feature work"
step 2 switch -q main
printf 'x\n' >>"$dir/ada/other.txt"
step 2 commit -qam "Main work"
step 3 merge -q --no-ff -m "Merge feature" feature
git -C "$dir/ada" tag v1
got=$("$blit" --offscreen 1200x700 --shot "$dir/shot.bmp" --script "frame;gitwait;gitgraph" "$dir/ada/poem.txt" 2>&1 >/dev/null | grep -v '^blit: git' | tr '\n' '/')
check "History as a graph: a branch merged back, and what points at each commit" '●─╮  Merge feature [main] [tag v1]/● │  Main work/│ ●  Feature work [feature]/●─╯  Start [origin/main]/'
search() { "$blit" --offscreen 1200x700 --shot "$dir/shot.bmp" --script "frame;gitwait;command:Git: Search the history…;text:$1;key:Return;gitwait;gitgraph" "$dir/ada/poem.txt" 2>&1 >/dev/null | grep -v '^blit: git' | tr '\n' '/'; }
got=$(search feature)
check "searched by message (any case): no lanes" 'Merge feature [main] [tag v1]/Feature work [feature]/'
got=$(search ADA)
check "by author" 'Merge feature [main] [tag v1]/Main work/Feature work [feature]/Start [origin/main]/'
got=$(search feature.txt)
check "by a path" 'Feature work [feature]/'

# The merge tool: a merge that stopped for conflicts, its file in three
# columns. Both sides changed line two (a conflict); one changed line four
# and the other line five (git's conflict too, but a simple one: each can
# be taken). Theirs taken for the first, each side's for the simple ones,
# then saved: the file as merged, and staged.
fresh
git -C "$dir/ada" switch -q -c feature
printf 'one\nTWO\nthree\nfour!\nfive\n' >"$dir/ada/poem.txt"
git -C "$dir/ada" commit -qam "Theirs"
git -C "$dir/ada" switch -q main
printf 'one\ntwo.\nthree\nfour\nFIVE\n' >"$dir/ada/poem.txt"
git -C "$dir/ada" commit -qam "Yours"
git -C "$dir/ada" merge -q feature >/dev/null 2>&1
merge() { "$blit" --offscreen 1200x700 --shot "$dir/shot.bmp" --script "frame;gitwait;command:Git: Merge the file's conflicts;gitwait;$1" "$dir/ada/poem.txt" 2>&1 >/dev/null | grep -v '^blit: git' | tr '\n' '/'; }
got=$(merge "mergedump;docdump:1")
check "a conflicted file in three columns: two conflicts, one simple; the cursor at the first" 'conflict:open conflict(simple):open | open 2/poem.txt (merge)/  one                   │ one                   │ one/> |TWO                   │ <<<<<<< yours         │ two./                        │ two.                  │ /'
got=$(merge "key:t;mergedump;key:];key:x;key:a;mergedump;key:]")
check "theirs taken, then each side's in the simple one; ] past the last stays" 'conflict:theirs conflict(simple):open | open 1/conflict:theirs conflict(simple):each | open 0/'
got=$(narrower "$(beside "$dir/ada/poem.txt" "frame;gitwait;command:Git: Merge the file's conflicts;gitwait;docdump:1")" \
    "$(beside "$dir/ada/poem.txt" "frame;gitwait;command:Git: Merge the file's conflicts;gitwait;docdump:1" "$dir/nomap.lua")")
check "the three columns beside the minimap: laid out left of it" 'narrower'
got="$(merge "key:t;key:a;key:cmd+s;gitwait;gitdump" | sed 's/ | branches.*//') $(tr '\n' '/' <"$dir/ada/poem.txt")"
check "saved: the file as merged, and staged" 'blit: Saved and staged poem.txt/main +1 merging | staged M:poem.txt one/TWO/three/four!/FIVE/'

# Interactive rebase: four commits since the server's, each in a file of
# its own. The second squashed into the first, the third reworded (its body
# kept), the fourth dropped; and, again, one moved up.
fresh
four() {
    for n in 1 2 3 4; do
        printf '%s\n' "$n" >"$dir/ada/f$n.txt"
        git -C "$dir/ada" add "f$n.txt"
        git -C "$dir/ada" commit -qm "C$n" -m "Body $n"
    done
}
four
rebase() { "$blit" --offscreen 1200x700 --shot "$dir/shot.bmp" --script "frame;gitwait;command:Git: Rebase interactively onto…;key:ctrl+U;text:origin/main;key:Return;gitwait;$1" "$dir/ada/poem.txt" 2>&1 >/dev/null | grep -v '^blit: git' | tr '\n' '/'; }
got=$(rebase "rebasedump")
check "the commits since the base, oldest first" 'pick C1; pick C2; pick C3; pick C4/'
got=$(rebase "key:Down;key:s;key:Down;key:r;key:ctrl+U;text:Third, better;key:Return;key:Down;key:d;rebasedump;key:cmd+s;gitwait")
got="$got $(git -C "$dir/ada" log --format='%s|%b' origin/main..HEAD | tr '\n' '/') $(ls "$dir/ada" | tr '\n' ' ')"
check "squashed, reworded (its body kept) and dropped" 'pick C1; squash C2; reword Third, better; drop C4/ Third, better|Body 3//C1|Body 1//C2//Body 2// f1.txt f2.txt f3.txt other.txt poem.txt '
fresh
four
got=$(rebase "key:Down;key:Down;key:alt+Up;key:alt+Up;key:alt+Up;rebasedump;key:Up;key:s;rebasedump;key:cmd+s;gitwait")
got="$got $(git -C "$dir/ada" log --format=%s origin/main..HEAD | tr '\n' ' ')"
check "one moved up (not past the first); a first one cannot be squashed" 'pick C3; pick C1; pick C2; pick C4/blit: Nothing before it to fold it into/pick C3; pick C1; pick C2; pick C4/ C4 C2 C1 C3 '
fresh
four
got=$("$blit" --offscreen 1200x700 --shot "$dir/shot.bmp" --script "frame;gitwait;command:Git: Rebase interactively onto…;key:ctrl+U;text:origin/main;key:Return;gitwait;key:Down;key:e;key:cmd+s;gitwait;gitdump;git:continue;gitwait;gitdump" "$dir/ada/poem.txt" 2>&1 >/dev/null |
    grep -v '^blit: git' | sed -e 's/ | .*//' | tr '\n' '/')
check "an edit stops it there, as a rebase's stops are; Continue goes on" '(no branch) rebasing/main +4/'

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

# Compare the file with a revision chosen from a list (branches, tags,
# commits): the diff of the file now against it, in a tab.
fresh
git -C "$dir/ada" tag v1
printf 'one\ntwo\nthree\nfour\nfive\nsix\n' >"$dir/ada/poem.txt"
git -C "$dir/ada" commit -q -am "Six"
printf 'one\nTWO\nthree\nfour\nfive\nsix\n' >"$dir/ada/poem.txt"
run "$dir/ada/poem.txt" "frame;gitwait;command:Git: Compare the file with;frame;finddump"
got=$(printf '%s' "$got" | sed -E -e 's/[0-9a-f]{7,}/H/g' -e 's/ada, (just now|now|[0-9]+ [a-z]+ ago)/ada/g')
check "the branches, the tags, the commits" "pick [] 5 : >origin/main  · remote branch v1  · tag H  Six  · ada H  Start  · ada HEAD  · the last commit"
view "$dir/ada/poem.txt" "frame;gitwait;command:Git: Compare the file with;frame;text:v1;key:Return;gitwait;gitview"
check "one chosen: the file now against it" "other poem.txt, one column: 1 hunks, cursor in 1 / diff --git a/poem.txt b/poem.txt / index b2f931a..9642008 100644 / --- a/poem.txt / +++ b/poem.txt / @@ -1,5 +1,6 @@ /  one / [--two-] / {++TWO+} /  three /  four /  five / {++six+}"
view "$dir/ada/poem.txt" "frame;gitwait;command:Git: Compare the file with;frame;text:HEAD~1;key:Return;gitwait;gitview"
check "a revision typed that is none of the rows: compared with as it is" "other poem.txt, one column: 1 hunks, cursor in 1 / diff --git a/poem.txt b/poem.txt / index b2f931a..9642008 100644 / --- a/poem.txt / +++ b/poem.txt / @@ -1,5 +1,6 @@ /  one / [--two-] / {++TWO+} /  three /  four /  five / {++six+}"
run "$dir/ada/poem.txt" "frame;gitwait;command:Git: Compare the file with;frame;key:Escape;command:Choose a theme;frame;text:HEAD~1;key:Return;frame;finddump"
check "(the next list of blit's own: Enter with no row that matches does nothing)" "pick [HEAD~1] 0 :"
for c in "Compare the file with" "The file's history" "Merge the file's conflicts"; do
    run "$dir/ada/poem.txt" "frame;gitwait;command:New file;command:Git: $c;frame"
    check "$c, from a new file not saved: said" "blit: Not a file in the repository"
done

# Worktrees: listed (this one first), made for a branch beside the
# repository, and opened as the folder worked in (its branch in the tab).
fresh
git -C "$dir/ada" worktree add -q -b feature "$dir/ada-feature" 2>/dev/null
run "$dir/ada/poem.txt" "frame;gitwait;gitdump"
got=$(printf '%s' "$got" | sed -n 's/.*| \(worktrees [^|]*\).*/\1/p' | sed 's/ $//')
check "the worktrees, this one first" "worktrees main@ada feature@ada-feature"
run "$dir/ada/poem.txt" "frame;gitwait;command:Git: New worktree;text:topic;key:Return;gitwait;gitdump"
got=$(git -C "$dir/ada" worktree list --porcelain | grep -c '^worktree .*ada-topic$')
check "one made for a new branch, beside the repository" "1"
run "$dir/ada/poem.txt" "frame;gitwait;git:opentree:$dir/ada-feature;gitwait;frame;gitwait;gitdump"
got=$(printf '%s' "$got" | sed -n 's/.*| branches \([^|]*\).*/\1/p' | tr ' ' '\n' | grep '^\*')
check "opened: the folder worked in, its branch the one checked out" "*feature"
# Opened as a folder is: the file in front is then that tree's (at its
# place there), not this one's under the other's name; one that tree does
# not have goes. With sessions kept, each tree's own tabs come back to it.
# tree SCRIPT [FILE]: what the run printed, its lines joined by "/".
tree() { "$blit" --offscreen 1200x700 --shot "$dir/shot.bmp" --script "$1" ${2:+"$2"} 2>&1 >/dev/null | grep -v '^blit: git' | sed "s#$dir#~#g" | tr '\n' '/'; }
got=$(tree "frame;gitwait;git:opentree:$dir/ada-feature;gitwait;frame;dump;statusdump" "$dir/ada/poem.txt")
check "another tree opened: the file in front is that tree's" '>[poem.txt*]/feature poem.txt | Ln 1, Col 1 Tabs LF Plain text/'
printf 'notes\n' >"$dir/ada/notes.txt"
got=$(tree "frame;gitwait;git:opentree:$dir/ada-feature;gitwait;frame;dump" "$dir/ada/notes.txt")
check "one the other tree does not have: closed" '>[untitled*]/'
rm "$dir/ada/notes.txt"
got=$(BLIT_RECENT_FILE="$dir/recent" tree "folder:$dir/ada;open:$dir/ada/other.txt;frame;gitwait;git:opentree:$dir/ada-feature;gitwait;frame;open:$dir/ada-feature/poem.txt;frame;dump;git:opentree:$dir/ada;gitwait;frame;dump;git:opentree:$dir/ada-feature;gitwait;frame;dump")
check "with sessions: each tree's tabs back when it is opened again" '>[other.txt poem.txt*]/>[other.txt*]/>[other.txt poem.txt*]/'
# Submodules: listed, one not checked out marked, and updated.
git init -q -b main "$dir/lib"
git -C "$dir/lib" -c user.name=x -c user.email=x@example.invalid commit -q --allow-empty -m "Lib"
git -C "$dir/ada" config protocol.file.allow always
git -C "$dir/ada" -c protocol.file.allow=always submodule -q add "$dir/lib" lib 2>/dev/null
git -C "$dir/ada" commit -q -m "Lib"
run "$dir/ada/poem.txt" "frame;gitwait;gitdump"
got=$(printf '%s' "$got" | sed -n 's/.*| \(submodules [^|]*\).*/\1/p' | sed 's/ $//')
check "the submodules" "submodules lib"
git -C "$dir/ada" submodule -q deinit -f lib 2>/dev/null
got=$("$blit" --offscreen 1200x700 --shot "$dir/shot.bmp" --script "frame;gitwait;gitdump;command:Git: Update the submodules;gitwait;gitdump" "$dir/ada/poem.txt" 2>&1 >/dev/null | sed -n 's/.*| \(submodules [^|]*\).*/\1/p' | sed 's/ $//' | tr '\n' ',')
check "one not checked out, updated: checked out" "submodules -lib,submodules lib,"
got=$(git -C "$dir/ada" submodule status | cut -c1)
check "(git says so too)" " "

# Pull requests, through a stand-in for gh: listed; one's comments on its
# lines (threads), checked out (its comments after their lines), its
# changes, approved and answered (each asked first), as gh was told.
fresh
export BLIT_GH="$(cd "$(dirname "$0")" && pwd)/fake-gh.sh" GH_LOG="$dir/gh.log"
: >"$GH_LOG"
prs="frame;gitwait;command:Git: Pull requests;luawait;frame"
run "$dir/ada/poem.txt" "$prs;finddump"
check "the open pull requests" "pick [] 2 : >#12  Louder poem  · bo, loud #9  Draft idea  · cy, idea  · draft"
run "$dir/ada/poem.txt" "$prs;key:Return;frame;key:Down;key:Down;key:Return;luawait;frame;finddump"
check "the comments on its lines, as threads (one on a line taken out, so said)" "pick [] 4 : >poem.txt:2  ada: Why shout?     poem.txt:2  bo: For effect. poem.txt:4  ada: Nice. poem.txt (taken out)  cy: Keep it."
run "$dir/ada/poem.txt" "$prs;key:Return;frame;key:Return;luawait;luawait;frame;prdump"
check "checked out: its comments after their lines (not one on a line taken out)" "#12: poem.txt:2 ada poem.txt:2 bo(answer) poem.txt:4 ada poem.txt:3 cy(taken out) | notes 2"
got=$(grep -c '^pr checkout 12$' "$GH_LOG")
check "(gh checked it out)" "1"
run "$dir/ada/poem.txt" "$prs;key:Return;frame;key:Down;key:Return;luawait;frame;lsptext"
check "its changes in a tab" "diff --git a/poem.txt b/poem.txt / index 1111111..2222222 100644 / --- a/poem.txt / +++ b/poem.txt / @@ -1,3 +1,3 @@ /  one / -two / +TWO /  three"
: >"$GH_LOG"
run "$dir/ada/poem.txt" "$prs;key:Return;frame;key:Down;key:Down;key:Down;key:Return;luawait;asked"
check "approved, asked first" "Approve #12 on GitHub?"
got=$(grep '^pr review' "$GH_LOG")
check "(gh approved it)" "pr review 12 --approve"
: >"$GH_LOG"
run "$dir/ada/poem.txt" "$prs;key:Return;frame;key:Down;key:Down;key:Return;luawait;frame;key:Down;key:Return;frame;key:Down;key:Return;frame;text:Fair enough;key:Return;luawait"
got=$(grep '^api -X POST' "$GH_LOG")
check "an answer, to its thread" "api -X POST repos/{owner}/{repo}/pulls/12/comments/101/replies -f body=Fair enough"
unset BLIT_GH GH_LOG

[ -n "$print" ] || [ $failed = 1 ] || echo "git ok"
exit $failed
