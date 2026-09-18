#!/usr/bin/env bash
# run_rig.sh -- two consoles, two FujiNets, one relay, one match.
#
#   test/run_rig.sh [seconds]
#
# The shape is the Intellivision family's: isolated copies of fujinet-pc so a
# run cannot disturb a real one, everything this script starts it kills first,
# and an inline verdict block rather than a human reading a log.
#
# Two things about this machine cost an afternoon each and are guarded here:
#
#   * A long-running fujinet-pc has held 127.0.0.1:9995 since September, and
#     the 2600 cartridge model connects there by DEFAULT. A rig on the default
#     port silently measures somebody else's FujiNet; ours logs "bind failed"
#     into a file nobody reads. So the rig uses its own ports and ABORTS if it
#     cannot have them.
#   * `cd "$RIG" && ./fujinet` puts "./fujinet" in the process's command line,
#     and `pkill -f "$RIG/fujinet"` then matches nothing. Every run leaked one,
#     and the next run measured the leak. Launch by absolute path.

. "$(dirname "$0")/serverlib.sh"

set -euo pipefail
cd "$(dirname "$0")/.."
HERE=$(pwd)

SECS=${1:-${SECS:-40}}
RELAY_PORT=${RELAY_PORT:-9600}
BOIP1=${BOIP1:-19995}
BOIP2=${BOIP2:-19996}
FNPC_DIST=${FNPC_DIST:-$HOME/Workspace/fujinet-pc-rs232/build/dist}

cleanup() {
    # Only OUR emulators: a `make play` session may be up on other ports and
    # killing it would be rude as well as wrong.
    for pid in $(pgrep -x mame 2>/dev/null || true); do
        tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null \
            | grep -q "build/rig/${RIGDIR:-fn}vo" && kill "$pid" 2>/dev/null
    done
    for n in 1 2; do
        pkill -f "$HERE/build/rig/${RIGDIR:-fn}$n/fujinet" 2>/dev/null || true
    done
    for pid in $(pgrep -x fujinet 2>/dev/null || true); do
        case "$(readlink -f "/proc/$pid/cwd" 2>/dev/null)" in
            "$HERE"/build/rig/"${RIGDIR:-fn}"*) kill "$pid" 2>/dev/null ;;
        esac
    done
    [ -n "${RELAY_PID:-}" ] && kill "$RELAY_PID" 2>/dev/null || true
    return 0
}
trap cleanup EXIT
cleanup
sleep 0.5

mkdir -p build/rig

# Two ROMs, because the two consoles must introduce themselves by different
# names: the relay refuses a duplicate by renaming it, and a rig that relied on
# that would be testing the rename.
for n in 1 2; do
    PLAYER="PLAYER$n" ENDPOINT="TCP://127.0.0.1:$RELAY_PORT/" \
        ./build.sh vo > "build/rig/build$n.log" 2>&1
    cp build/vo.bin "build/rig/${RIGDIR:-fn}vo$n.bin"
done
echo "== built two client ROMs =="

for n in 1 2; do
    port=$([ "$n" = 1 ] && echo "$BOIP1" || echo "$BOIP2")
    rig="$HERE/build/rig/${RIGDIR:-fn}$n"
    rm -rf "$rig"
    mkdir -p "$rig"
    cp -a "$FNPC_DIST"/. "$rig"/
    python3 - "$rig/fnconfig.ini" "$port" <<'PY'
import re, sys
path, port = sys.argv[1], sys.argv[2]
s = open(path).read()
s = re.sub(r"(\[BOIP\][^\[]*?\bport=)\d*", r"\g<1>" + port, s, flags=re.S)
s = re.sub(r"(\[BOIP\][^\[]*?\benabled=)\d*", r"\g<1>1", s, flags=re.S)
open(path, "w").write(s)
PY
    ( cd "$rig" && setsid "$rig/fujinet" < /dev/null > "$HERE/build/rig/fn$n.log" 2>&1 & )
done
sleep 2
for n in 1 2; do
    if grep -q "bind failed" "build/rig/fn$n.log"; then
        echo "run_rig: fujinet-pc $n could not bind its BoIP port -- something" \
             "else holds it. Aborting rather than measuring it." >&2
        grep -m1 "bind failed" "build/rig/fn$n.log" >&2
        exit 1
    fi
done
echo "== two fujinet-pc on :$BOIP1 and :$BOIP2 =="

setsid relay_server --host 127.0.0.1 \
    --port "$RELAY_PORT" --delay 2 --variation "${VARIATION:-2}" \
    < /dev/null > build/rig/relay.log 2>&1 &
RELAY_PID=$!
sleep 1
# THE SAME GUARD THE BoIP PORTS GET, FOR THE SAME REASON. A relay left running
# by `make play` still owns 9600; this one dies with "Address already in use"
# into a log nobody reads, the two consoles pair against the OTHER relay, and
# every verdict below is measured somewhere else. It cost a whole ladder run.
if ! kill -0 "$RELAY_PID" 2>/dev/null; then
    echo "run_rig: the relay could not take 127.0.0.1:$RELAY_PORT." >&2
    tail -3 build/rig/relay.log >&2
    echo "run_rig: something else holds it -- test/stop.sh, or set RELAY_PORT." >&2
    exit 1
fi
echo "== relay on :$RELAY_PORT =="

for n in 1 2; do
    port=$([ "$n" = 1 ] && echo "$BOIP1" || echo "$BOIP2")
    SNAPTICK="${SNAPTICK:-150}" RIG_HOLD="${RIG_HOLD:-}" \
    PLAY_WINDOW="${PLAY_WINDOW:-}" \
    PLAY_INJECT="$([ "$n" = 1 ] && echo "${PLAY_INJECT:-}")" \
    SECS="$SECS" FUJINET_TCP="127.0.0.1:$port" \
        ./run.sh "rig/${RIGDIR:-fn}vo$n" "${RIG_LUA:-rig}" \
            < /dev/null > "build/rig/c$n.out" 2>&1 &
    sleep 1
done
wait_secs=$((SECS + 20))
for _ in $(seq "$wait_secs"); do
    pgrep -f "build/rig/${RIGDIR:-fn}vo" > /dev/null || break
    sleep 1
done
sleep 1

echo
echo "== console 1 =="; tail -7 build/rig/c1.out
echo "== console 2 =="; tail -7 build/rig/c2.out
echo "== relay =="; grep -v "CRC MISMATCH" build/rig/relay.log
echo "   $(grep -c "CRC MISMATCH" build/rig/relay.log || true) CRC MISMATCH lines"

# emu/play.lua's verdict is a different question, so it gets a different block:
# not "did these two agree" -- the relay answers that -- but WHICH CELL WENT
# FIRST. The state dump is megabytes, so only the diagnosis is printed.
if [ "${RIG_LUA:-rig}" = "play" ] && [ -n "${PLAY_INJECT:-}" ]; then
    # The repair gate asks the OPPOSITE question: one console was deliberately
    # corrupted, so mismatches are the point and silence would mean the
    # injection missed. What has to be true is that they STOPPED.
    echo
    echo "== injection =="; grep -h "^INJECT" build/rig/c1.out || true
    python3 tools/playdiff.py build/rig/c1.out build/rig/c2.out --repair
    rc=$?
    echo
    if [ "$rc" = 0 ] && grep -q "CRC MISMATCH" build/rig/relay.log; then
        echo "REPAIR PASS"; exit 0
    fi
    [ "$rc" = 0 ] || echo "  FAIL the two consoles never came back together"
    grep -q "CRC MISMATCH" build/rig/relay.log \
        || echo "  FAIL the relay never saw the injected desync at all"
    echo "REPAIR FAIL"; exit 1
fi

if [ "${RIG_LUA:-rig}" = "play" ]; then
    echo
    python3 tools/playdiff.py build/rig/c1.out build/rig/c2.out
    rc=$?
    echo
    if [ "$rc" = 0 ] && ! grep -q "CRC MISMATCH" build/rig/relay.log; then
        echo "PLAY PASS"; exit 0
    fi
    echo "PLAY FAIL"; exit 1
fi

python3 - build/rig/c1.out build/rig/c2.out build/rig/relay.log <<'PY'
import re, sys
c1, c2, relay = (open(p, errors="replace").read() for p in sys.argv[1:4])
fails = []

def want(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        fails.append(what)

want("match:" in relay, "the relay paired the two consoles")
want("CRC MISMATCH" not in relay, "the two consoles never disagreed")

# The two snapshots are taken at the SAME simulated tick on both consoles, so
# they have to be identical byte for byte -- including the game variation, which
# is what proves SELECT reached both machines and advanced them together.
#
# SNAP is the SIMULATION only. The raw SWCHB port and the peer's ring slot moved
# to the LOCAL line, which is reported and never compared: with SELECT held on
# one console those two bytes differ BECAUSE the press is working, and comparing
# them asserted that both players had their hands in the same place.
sa = re.search(r"^SNAP .*$", c1, re.M)
sb = re.search(r"^SNAP .*$", c2, re.M)
want(sa is not None and sb is not None, "both consoles snapshotted the same tick")
if sa and sb:
    want(sa.group(0) == sb.group(0),
         "the two consoles agree byte for byte at the snapshot tick")
    ga = re.search(r": \w+ \w+ (\w+)", sa.group(0))
    gb = re.search(r": \w+ \w+ (\w+)", sb.group(0))
    want(ga and gb and ga.group(1) == gb.group(1),
         "the two consoles are on the SAME game variation")
    want(ga is not None and ga.group(1) != "24",
         "SELECT changed the variation away from the one START set")
for n, out in ((1, c1), (2, c2)):
    m = re.search(r"RIG tick=(\d+) err=\$([0-9A-F]{2}) state=(\d+)", out)
    want(m is not None, "console %d reported its state" % n)
    if m:
        tick, err = int(m.group(1)), int(m.group(2), 16)
        want(tick > 50, "console %d ran %d ticks" % (n, tick))
        want(err & 0x0F == 0, "console %d has no transport error ($%02X)" % (n, err))
print()
print("RIG FAIL" if fails else "RIG PASS")
sys.exit(1 if fails else 0)
PY
