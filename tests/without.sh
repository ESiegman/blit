# Sourced by the suites that say what is missing: without DIR PROGRAM...
# makes DIR a folder of links to everything in /usr/bin and /bin but those
# programs, a PATH on which they are not installed, wherever the system
# keeps them (Arch Linux's packages put OpenOCD, probe-rs and dfu-util in
# /usr/bin itself, so "$dir/none:/usr/bin:/bin" still has them). (Its
# names begin without_: sh has no local variables, and a suite's own are
# left alone.)
without() {
    without_dir=$1
    shift
    rm -rf "$without_dir"
    mkdir -p "$without_dir"
    for without_f in /usr/bin/* /bin/*; do [ -e "$without_dir/${without_f##*/}" ] || ln -s "$without_f" "$without_dir/" 2>/dev/null; done
    for without_p; do rm -f "$without_dir/$without_p"; done
}
