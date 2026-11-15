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
# BLIT_FAKE_PASSWORD: the "host" has no key for us and asks for that
# password, as ssh does, through $SSH_ASKPASS (which blit sets to itself).
if [ -n "${BLIT_FAKE_PASSWORD:-}" ]; then
    [ -n "${SSH_ASKPASS:-}" ] || { echo "tester@box: Permission denied (publickey,password)." >&2; exit 255; }
    got=$("$SSH_ASKPASS" "tester@box's password: ") || { echo "tester@box: Permission denied (publickey,password)." >&2; exit 255; }
    [ "$got" = "$BLIT_FAKE_PASSWORD" ] || { echo "tester@box: Permission denied, please try again." >&2; exit 255; }
fi
exec /bin/sh -c "$*"
