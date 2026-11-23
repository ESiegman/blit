#!/bin/sh
# Updates, against a release made here: its JSON, as GitHub's API answers,
# and its files beside it, all file:// URLs. A newer release is offered,
# fetched, its SHA-256 checked and put in place of a stand-in blit (an
# AppImage, and on a Mac a blit.app); one with the wrong sum is not.
#
#   tests/update.sh path/to/blit
set -u
blit=${1:?usage: tests/update.sh path/to/blit}
command -v curl >/dev/null 2>&1 || { echo "skipped: no curl"; exit 77; }
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
printf 'blit.opt.lsp.enabled = false\nblit.opt.editor.modal = false\nblit.opt.git.enabled = false\n' >"$dir/init.lua"
export BLIT_INIT="$dir/init.lua"
export XDG_DATA_HOME="$dir/data"
echo hello >"$dir/a.txt"
if command -v sha256sum >/dev/null 2>&1; then sum() { sha256sum "$1"; }; else sum() { shasum -a 256 "$1"; }; fi
# path_without PROGRAM...: a PATH of /usr/bin's and /bin's programs but not
# those named (a machine they are not installed on), and what blit calls
# this machine.
path_without() {
    p="$dir/without-$(printf '%s' "$*" | tr ' ' -)"
    mkdir -p "$p"
    ln -s /usr/bin/* "$p/" 2>/dev/null
    ln -s /bin/* "$p/" 2>/dev/null
    for n in "$@"; do rm -f "$p/$n"; done
    printf '%s' "$p"
}
case $(uname -s) in Darwin) this="this Mac" ;; *) this="this machine" ;; esac
failed=0
fail() {
    printf 'FAIL: %s\n%s\n' "$1" "$(printf '%s\n' "$2" | grep 'blit:\|asked\|script' | sed 's/^/    /')"
    failed=1
}
# release VERSION: the release's JSON and files, the AppImage's "new".
release() {
    rm -rf "$dir/rel"
    mkdir -p "$dir/rel/stage"
    echo "new $1" >"$dir/rel/blit-$1-x86_64.AppImage"
    mkdir -p "$dir/rel/stage/blit.app/Contents/MacOS"
    echo "new $1" >"$dir/rel/stage/blit.app/Contents/MacOS/blit"
    if command -v ditto >/dev/null 2>&1; then
        (cd "$dir/rel/stage" && ditto -c -k --keepParent blit.app "$dir/rel/blit-$1-macos-arm64.zip")
    else
        echo "no zip here" >"$dir/rel/blit-$1-macos-arm64.zip"
    fi
    for f in "blit-$1-x86_64.AppImage" "blit-$1-macos-arm64.zip"; do
        (cd "$dir/rel" && sum "$f" >"$f.sha256")
    done
    u="file://$dir/rel"
    cat >"$dir/rel/latest.json" <<JSON
{"tag_name": "v$1", "name": "blit $1", "assets": [
 {"name": "blit-$1-macos-arm64.zip", "browser_download_url": "$u/blit-$1-macos-arm64.zip"},
 {"name": "blit-$1-macos-arm64.zip.sha256", "browser_download_url": "$u/blit-$1-macos-arm64.zip.sha256"},
 {"name": "blit-$1-x86_64.AppImage", "browser_download_url": "$u/blit-$1-x86_64.AppImage"},
 {"name": "blit-$1-x86_64.AppImage.sha256", "browser_download_url": "$u/blit-$1-x86_64.AppImage.sha256"}]}
JSON
}
export BLIT_UPDATES_URL="file://$dir/rel/latest.json"
run() { "$blit" --offscreen 800x600 --shot "$dir/shot.bmp" --script "$1;luawait;asked;frame" "$dir/a.txt" 2>&1 >/dev/null; }

# A newer AppImage: asked, fetched, checked, in place, a program.
release 99.0.0
mkdir -p "$dir/apps"
echo old >"$dir/apps/blit.AppImage"
export BLIT_UPDATE_TARGET="$dir/apps/blit.AppImage"
got=$(run "command:Check for updates")
[ "$(cat "$dir/apps/blit.AppImage")" = "new 99.0.0" ] || fail "a newer AppImage: put in place" "$got"
[ -x "$dir/apps/blit.AppImage" ] || fail "the new AppImage: a program" "$got"
printf '%s' "$got" | grep -q 'blit 99.0.0 is out (this one is [0-9.]*). Install it?' || fail "a newer one: asked first" "$got"
printf '%s' "$got" | grep -q 'blit: blit 99.0.0 is installed: it starts next time' || fail "installed: said" "$got"
[ -z "$(ls -A "$dir/apps" | grep -v '^blit.AppImage$')" ] || fail "nothing left beside it" "$(ls -A "$dir/apps")"

# The wrong sum: nothing put in place.
release 99.0.1
echo "0000000000000000000000000000000000000000000000000000000000000000  blit-99.0.1-x86_64.AppImage" >"$dir/rel/blit-99.0.1-x86_64.AppImage.sha256"
echo old >"$dir/apps/blit.AppImage"
got=$(run "command:Check for updates")
[ "$(cat "$dir/apps/blit.AppImage")" = "old" ] || fail "the wrong SHA-256: not put in place" "$got"
printf '%s' "$got" | grep -q 'came with the wrong SHA-256' || fail "the wrong SHA-256: said" "$got"

# A sum written in upper case: the same sum, put in place.
release 99.0.2
awk '{ $1 = toupper($1); print }' "$dir/rel/blit-99.0.2-x86_64.AppImage.sha256" >"$dir/upper" && mv "$dir/upper" "$dir/rel/blit-99.0.2-x86_64.AppImage.sha256"
echo old >"$dir/apps/blit.AppImage"
got=$(run "command:Check for updates")
[ "$(cat "$dir/apps/blit.AppImage")" = "new 99.0.2" ] || fail "a SHA-256 in upper case: put in place" "$got"

# The same version (or an older one): the latest, nothing fetched.
release 0.0.1
got=$(run "command:Check for updates")
printf '%s' "$got" | grep -q 'blit: blit [0-9.]* is the latest' || fail "an older release: this is the latest" "$got"
printf '%s' "$got" | grep -q 'Install it' && fail "an older release: not offered" "$got"

# At the start: once a day (the second start asks nothing).
release 99.0.0
echo old >"$dir/apps/blit.AppImage"
rm -rf "$dir/data"
got=$(run "frame")
[ "$(cat "$dir/apps/blit.AppImage")" = "new 99.0.0" ] || fail "at the start: looked for and installed" "$got"
echo old >"$dir/apps/blit.AppImage"
got=$(run "frame")
[ "$(cat "$dir/apps/blit.AppImage")" = "old" ] || fail "at the start: not again the same day" "$got"
# updates.check off: not at the start.
rm -rf "$dir/data"
printf 'blit.opt.updates.check = false\n' >>"$dir/init.lua"
got=$(run "frame")
[ "$(cat "$dir/apps/blit.AppImage")" = "old" ] || fail "updates.check off: not looked for" "$got"
printf 'blit.opt.lsp.enabled = false\nblit.opt.editor.modal = false\nblit.opt.git.enabled = false\n' >"$dir/init.lua"

# Installed some other way: only told.
export BLIT_UPDATE_TARGET=
unset APPIMAGE
got=$(run "command:Check for updates")
printf '%s' "$got" | grep -q 'blit: blit 99.0.0 is out (this one is' || fail "installed otherwise: told" "$got"

# A blit.app (a Mac's): unpacked beside it and swapped.
if command -v ditto >/dev/null 2>&1; then
    mkdir -p "$dir/apps/blit.app/Contents/MacOS"
    echo old >"$dir/apps/blit.app/Contents/MacOS/blit"
    export BLIT_UPDATE_TARGET="$dir/apps/blit.app"
    got=$(run "command:Check for updates")
    [ "$(cat "$dir/apps/blit.app/Contents/MacOS/blit" 2>/dev/null)" = "new 99.0.0" ] || fail "a newer blit.app: put in place" "$got"
    [ -z "$(ls -A "$dir/apps" | grep -v -e '^blit.AppImage$' -e '^blit.app$')" ] || fail "blit.app: nothing left beside it" "blit: left: $(ls -A "$dir/apps" | tr "\n" " ")"
fi
# curl not installed: said, naming it and this machine.
got=$(PATH=$(path_without curl) run "command:Check for updates")
printf '%s' "$got" | grep -qF "blit: Could not ask for the latest release: curl is not installed on $this" || fail "curl not installed: said" "$got"
# Pre-releases, in semver's order: a release after its pre-releases, and
# these by their words in turn (numbers as numbers, and before any other
# word; fewer words first); a "+build" counts for nothing.
# order RUNNING RELEASE newer|latest: what this blit (as RUNNING) says of RELEASE.
order() {
    release "$2"
    got=$(BLIT_UPDATE_VERSION=$1 BLIT_UPDATE_TARGET= run "command:Check for updates")
    if [ "$3" = newer ]; then
        printf '%s' "$got" | grep -qF "blit: blit $2 is out (this one is $1)" || fail "$1, then $2: newer" "$got"
    else
        printf '%s' "$got" | grep -qF "blit: blit $1 is the latest" || fail "$1, then $2: the latest" "$got"
    fi
}
unset APPIMAGE
order 1.0.0-rc1 1.0.0 newer
order 1.0.0 1.0.0-rc1 latest
order 1.0.0-rc.2 1.0.0-rc.10 newer
order 1.0.0-rc.10 1.0.0-rc.2 latest
order 1.0.0-alpha 1.0.0-alpha.1 newer
order 1.0.0-alpha.1 1.0.0-alpha.beta newer
order 1.0.0-beta 1.0.0-alpha.9 latest
order 1.0.0-rc.1 1.0.0-rc.1+build.5 latest
order 0.9.9 1.0.0-alpha newer
order 1.0.0-rc1 1.0.1-rc1 newer
[ $failed = 0 ] && echo "update ok"
exit $failed
