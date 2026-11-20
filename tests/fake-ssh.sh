#!/bin/sh
# Stands in for ssh in the tests (BLIT_SSH names it): the options and the
# host are passed over, and the command is run here, in a home of its own
# ($BLIT_FAKE_HOME), so that blit's agent is copied there and run as it
# would be on another machine.
while [ $# -gt 1 ]; do
    case "$1" in
        -o) shift 2 ;;
        -*) shift ;;
        *) shift; break ;;  # the host
    esac
done
[ -n "${BLIT_FAKE_HOME:-}" ] && HOME=$BLIT_FAKE_HOME && export HOME
[ -n "${BLIT_FAKE_FAIL:-}" ] && { echo "ssh: connect to host: $BLIT_FAKE_FAIL" >&2; exit 255; }
# BLIT_FAKE_DOWN=FILE: the host cannot be reached while that file is there
# (it says why), so that a test can bring it back. One that says "hang" is a
# host that does not answer at all (as when the link is down: ssh waits)
# until the file goes.
if [ -n "${BLIT_FAKE_DOWN:-}" ] && [ "$(cat "$BLIT_FAKE_DOWN" 2>/dev/null)" = hang ]; then
    while [ -e "$BLIT_FAKE_DOWN" ]; do sleep 0.05; done
fi
[ -n "${BLIT_FAKE_DOWN:-}" ] && [ -e "$BLIT_FAKE_DOWN" ] && { echo "ssh: connect to host: $(cat "$BLIT_FAKE_DOWN")" >&2; exit 255; }
# BLIT_FAKE_PASSWORD: the "host" has no key for us and asks for that
# password, as ssh does, through $SSH_ASKPASS (which blit sets to itself).
if [ -n "${BLIT_FAKE_PASSWORD:-}" ]; then
    [ -n "${SSH_ASKPASS:-}" ] || { echo "tester@box: Permission denied (publickey,password)." >&2; exit 255; }
    got=$("$SSH_ASKPASS" "tester@box's password: ") || { echo "tester@box: Permission denied (publickey,password)." >&2; exit 255; }
    [ "$got" = "$BLIT_FAKE_PASSWORD" ] || { echo "tester@box: Permission denied, please try again." >&2; exit 255; }
fi
# BLIT_FAKE_SLOW=SECONDS: a slow host. Once blit's agent has started, each
# piece of what it sends back is held that long first, so that every request
# blit makes waits at least that long for its answer.
if [ -n "${BLIT_FAKE_SLOW:-}" ]; then
    /bin/sh -c "$*" | exec perl -e '
        $| = 1;
        my ($slow, $on, $seen) = ($ENV{BLIT_FAKE_SLOW}, 0, "");
        while (sysread(STDIN, my $b, 65536)) {
            if ($on) { select(undef, undef, undef, $slow) }
            else { $seen = substr($seen . $b, -64); $on = 1 if $seen =~ /BLIT-START\n/ }
            syswrite(STDOUT, $b) or exit 0;
        }'
    exit
fi
exec /bin/sh -c "$*"
