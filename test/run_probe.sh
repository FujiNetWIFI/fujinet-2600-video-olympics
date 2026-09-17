#!/usr/bin/env bash
# run_probe.sh -- M1: measure what a FujiNet mailbox transaction costs.
#
# Brings up one isolated fujinet-pc, the echo server, and one MAME running
# build/probe.bin, then prints the frame cost of OPEN, WRITE, STATUS and READ
# and the implied lockstep tick rate.
#
#   test/run_probe.sh [seconds]
#
# The fujinet-pc is a COPY of the distribution, in build/rig/fn1, so the run
# cannot disturb a real one: nametest-style accidents in this family have
# rewritten the machine-wide username appkey before now. Its BoIP listener
# takes ONE client and the symptom of a second is a hang rather than an error,
# so everything this script starts, it kills first.

set -euo pipefail
cd "$(dirname "$0")/.."
HERE=$(pwd)

SECS=${1:-${SECS:-40}}
ECHO_PORT=${ECHO_PORT:-9605}
# NOT 9995. The 2600 cartridge model defaults there, and on this machine a
# long-running fujinet-pc has held 127.0.0.1:9995 since September. A rig that
# used the default would silently measure THAT instance -- ours would log
# "bind failed: Address already in use" in a file nobody reads, and the numbers
# would be of some other build's transport. The family's rule is that a rig
# kills its own instances; the corollary is that it must not be able to reach
# anyone else's.
BOIP_PORT=${BOIP_PORT:-19995}
FNPC_DIST=${FNPC_DIST:-$HOME/Workspace/fujinet-pc-rs232/build/dist}
RIG="$HERE/build/rig/fn1"

# Kill only OUR instances. The pattern must be anchored on this rig's own
# directory: a bare `pkill -f fujinet` on this machine would take out several
# long-running ones, and the family's rule is that a rig never types a pattern
# it would not want to type on an interactive shell.
cleanup() {
    pkill -f "mame a2600" 2>/dev/null || true
    pkill -f "$RIG/fujinet" 2>/dev/null || true
    # Belt and braces: anything whose cwd is this rig, whatever it calls itself.
    for pid in $(pgrep -x fujinet 2>/dev/null || true); do
        [ "$(readlink -f "/proc/$pid/cwd" 2>/dev/null)" = "$RIG" ] && kill "$pid" 2>/dev/null
    done
    [ -n "${ECHO_PID:-}" ] && kill "$ECHO_PID" 2>/dev/null || true
    return 0
}
trap cleanup EXIT
cleanup
sleep 0.5

[ -f build/probe.bin ] || { echo "run_probe: build/probe.bin missing -- make probe" >&2; exit 1; }

# A fresh copy every run: a fujinet-pc that accumulated state across runs would
# make a measurement depend on what the last one did.
rm -rf "$RIG"
mkdir -p "$RIG"
cp -a "$FNPC_DIST"/. "$RIG"/
python3 - "$RIG/fnconfig.ini" "$BOIP_PORT" <<'PY'
import re, sys
path, port = sys.argv[1], sys.argv[2]
s = open(path).read()
s = re.sub(r"(\[BOIP\][^\[]*?\bport=)\d*", r"\g<1>" + port, s, flags=re.S)
s = re.sub(r"(\[BOIP\][^\[]*?\benabled=)\d*", r"\g<1>1", s, flags=re.S)
open(path, "w").write(s)
PY

echo "== echo server on :$ECHO_PORT =="
setsid python3 tools/latency_probe_server.py --port "$ECHO_PORT" \
    < /dev/null > build/echo.log 2>&1 &
ECHO_PID=$!
sleep 0.5

echo "== fujinet-pc (BoIP :$BOIP_PORT) =="
# Launched by ABSOLUTE path, on purpose. `cd "$RIG" && ./fujinet` puts the
# string "./fujinet" in the process's command line, and `pkill -f "$RIG/fujinet"`
# then matches nothing -- so every run leaks a fujinet that still holds the BoIP
# port, and the NEXT run measures whichever one happened to bind first. That is
# the same hazard the family documents as "a leftover fujinet-pc holds its port
# and every later launch silently no-ops", arriving through the cleanup pattern
# rather than through forgetting to clean up at all.
( cd "$RIG" && setsid "$RIG/fujinet" < /dev/null > "$HERE/build/fn1.log" 2>&1 & )
sleep 2

# Prove OUR instance owns the port before MAME connects to it. A bind failure
# here is the difference between measuring this transport and measuring
# whatever else happens to be listening.
if grep -q "bind failed" "$HERE/build/fn1.log"; then
    echo "run_probe: our fujinet-pc could not bind :$BOIP_PORT --" \
         "something else holds it. Aborting rather than measuring it." >&2
    grep -m2 "bind failed" "$HERE/build/fn1.log" >&2
    exit 1
fi
grep -m1 "BoIPChannel: listening" "$HERE/build/fn1.log" || true

echo "== MAME, ${SECS}s, throttled =="
SECS="$SECS" FUJINET_TCP="127.0.0.1:$BOIP_PORT" ./run.sh probe latency \
    < /dev/null > build/probe.out 2>&1 || true
cat build/probe.out

echo
echo "== echo server saw =="
cat build/echo.log
