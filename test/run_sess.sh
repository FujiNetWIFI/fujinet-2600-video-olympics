#!/usr/bin/env bash
# run_sess.sh -- one console, one FujiNet, one relay: where does the session get to?
#
# A single-console version of the rig, for the half of the handshake that does
# not need an opponent. The colour the boot bank paints IS its state, so
# emu/sess.lua taps COLUBK and prints the sequence.
. "$(dirname "$0")/serverlib.sh"

set -euo pipefail
cd "$(dirname "$0")/.."
HERE=$(pwd)
SECS=${1:-15}
BOIP=${BOIP:-19995}
RELAY_PORT=${RELAY_PORT:-9600}
FNPC_DIST=${FNPC_DIST:-$HOME/Workspace/fujinet-pc-rs232/build/dist}
RIG="$HERE/build/rig/fn1"

cleanup() {
    pkill -x mame 2>/dev/null || true
    pkill -f "$RIG/fujinet" 2>/dev/null || true
    for pid in $(pgrep -x fujinet 2>/dev/null || true); do
        case "$(readlink -f "/proc/$pid/cwd" 2>/dev/null)" in
            "$HERE"/build/rig/*) kill "$pid" 2>/dev/null ;;
        esac
    done
    [ -n "${RELAY_PID:-}" ] && kill "$RELAY_PID" 2>/dev/null || true
    return 0
}
trap cleanup EXIT
cleanup
sleep 0.5

PLAYER=PLAYER1 ENDPOINT="TCP://127.0.0.1:$RELAY_PORT/" ./build.sh vo >/dev/null 2>&1
cp build/vo.bin build/vo1.bin

rm -rf "$RIG"; mkdir -p "$RIG"; cp -a "$FNPC_DIST"/. "$RIG"/
python3 - "$RIG/fnconfig.ini" "$BOIP" <<'PY'
import re, sys
s = open(sys.argv[1]).read()
s = re.sub(r"(\[BOIP\][^\[]*?\bport=)\d*", r"\g<1>" + sys.argv[2], s, flags=re.S)
s = re.sub(r"(\[BOIP\][^\[]*?\benabled=)\d*", r"\g<1>1", s, flags=re.S)
open(sys.argv[1], "w").write(s)
PY
( cd "$RIG" && setsid "$RIG/fujinet" < /dev/null > "$HERE/build/rig/fn1.log" 2>&1 & )
sleep 2
grep -q "bind failed" build/rig/fn1.log && { echo "run_sess: BoIP port taken" >&2; exit 1; }

setsid relay_server --host 127.0.0.1 --port "$RELAY_PORT" \
    < /dev/null > build/rig/relay.log 2>&1 &
RELAY_PID=$!
sleep 1

SECS="$SECS" FUJINET_TCP="127.0.0.1:$BOIP" ./run.sh vo1 sess \
    < /dev/null > build/rig/sess.out 2>&1 || true
echo "== console =="; cat build/rig/sess.out
echo "== relay =="; cat build/rig/relay.log
echo "== fujinet (network lines) =="
grep -iE "N:|TCP|open|connect|error" build/rig/fn1.log | tail -12
