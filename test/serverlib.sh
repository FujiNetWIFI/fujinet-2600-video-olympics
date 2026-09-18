# Shared relay-server selection for the test scripts.  Source, don't execute:
#     . "$(dirname "$0")/serverlib.sh"
#
# SERVER=py (default) runs server/vo_relay_server.py, which stays the canonical
# reference implementation.  SERVER=c runs the C port in server/c/, which
# tools/server_diff.py holds to byte-for-byte equality with it on the wire and
# in the log -- including the two lines the rigs grep for, "match:" and
# "CRC MISMATCH".
#
# It sets ONE array, RELAY_SERVER, holding the command.  setsid stays in the
# CALLER, exactly where it stood before the C port existed:
#
#     setsid "${RELAY_SERVER[@]}" --host 127.0.0.1 --port "$P" \
#         < /dev/null > "$LOG" 2>&1 &
#     RELAY_PID=$!
#
# THE FIRST VERSION OF THIS FILE MADE relay_server A SHELL FUNCTION and asked
# callers to write `setsid relay_server ...`.  setsid is /usr/bin/setsid and
# execs a BINARY: a shell function does not exist on the far side of an exec,
# so every caller died with status 127 -- "setsid: failed to execute
# relay_server" -- before a relay was ever started.  make play, session, rig,
# rig-hold, rig-play and rig-repair were all broken by it, and nothing caught
# it because those six are the gates that need MAME, and sim/lobby/server-diff
# pick the implementation in Python instead of coming through here.
#
# An array cannot fail that way: it expands to a real argv before setsid is
# reached.  It also keeps $! the server's own PID rather than a wrapper's, with
# no dependence on whether setsid decides to fork -- which is what the traps in
# run_rig.sh and run_sess.sh kill, and the difference between a teardown and a
# leaked relay holding :9600 against the next run.

# Absolute, resolved from this file rather than the caller's cwd: run_play.sh
# and run_rig.sh source this BEFORE their own `cd`, so a relative -x test here
# would answer a different question depending on where the script was invoked
# from.
_VO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

case "${SERVER:-py}" in
py)
    RELAY_SERVER=(python3 "$_VO_ROOT/server/vo_relay_server.py")
    ;;
c)
    if [ ! -x "$_VO_ROOT/server/c/vo-relay" ]; then
        echo "SERVER=c: server/c/vo-relay is not built -- run" >&2
        echo "  make -C server/c" >&2
        exit 1
    fi
    RELAY_SERVER=("$_VO_ROOT/server/c/vo-relay")
    ;;
*)
    echo "SERVER must be 'py' or 'c' (got '$SERVER')" >&2
    exit 1
    ;;
esac
