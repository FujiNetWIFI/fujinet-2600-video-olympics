#!/usr/bin/env bash
# run_play.sh -- two consoles you can actually play, side by side.
#
#   test/run_play.sh          two MAME windows, open-ended
#   test/run_play.sh stella   ...with Stella for the second console instead
#   test/stop.sh              tear it all down
#
# The same infrastructure as test/run_rig.sh -- two isolated fujinet-pc copies
# on their own BoIP ports, one relay -- but windowed, unthrottled by nothing,
# and with no -seconds_to_run. The rig proves it; this is for watching it.
#
# Each console's player uses JOYSTICK 1 on their own machine: the host drives
# the left tank and the guest the right, and each player's own difficulty
# switch controls their own tank. Whichever window has focus takes the keyboard.

set -euo pipefail
cd "$(dirname "$0")/.."
HERE=$(pwd)

RELAY_PORT=${RELAY_PORT:-9600}
BOIP1=${BOIP1:-19995}
BOIP2=${BOIP2:-19996}
FNPC_DIST=${FNPC_DIST:-$HOME/Workspace/fujinet-pc-rs232/build/dist}
MAME=${MAME:-$HOME/Workspace/mame}

"$HERE/test/stop.sh" 2>/dev/null || true
sleep 0.5
mkdir -p build/rig

echo "== building two client ROMs =="
for n in 1 2; do
    PLAYER="PLAYER$n" ENDPOINT="TCP://127.0.0.1:$RELAY_PORT/" \
        ./build.sh vo > "build/rig/build$n.log" 2>&1
    cp build/vo.bin "build/vo$n.bin"
done

for n in 1 2; do
    port=$([ "$n" = 1 ] && echo "$BOIP1" || echo "$BOIP2")
    rig="$HERE/build/rig/fn$n"
    rm -rf "$rig"; mkdir -p "$rig"; cp -a "$FNPC_DIST"/. "$rig"/
    python3 - "$rig/fnconfig.ini" "$port" <<'PY'
import re, sys
s = open(sys.argv[1]).read()
s = re.sub(r"(\[BOIP\][^\[]*?\bport=)\d*", r"\g<1>" + sys.argv[2], s, flags=re.S)
s = re.sub(r"(\[BOIP\][^\[]*?\benabled=)\d*", r"\g<1>1", s, flags=re.S)
open(sys.argv[1], "w").write(s)
PY
    # Absolute path, so test/stop.sh can find it again: `cd dir && ./fujinet`
    # puts "./fujinet" in the command line and every pattern misses it.
    ( cd "$rig" && setsid "$rig/fujinet" < /dev/null > "$HERE/build/rig/fn$n.log" 2>&1 & )
done
sleep 2
for n in 1 2; do
    if grep -q "bind failed" "build/rig/fn$n.log"; then
        echo "run_play: fujinet-pc $n could not bind its BoIP port." >&2
        grep -m1 "bind failed" "build/rig/fn$n.log" >&2
        exit 1
    fi
done
echo "== two fujinet-pc on :$BOIP1 and :$BOIP2 =="

setsid python3 server/vo_relay_server.py --host 127.0.0.1 \
    --port "$RELAY_PORT" --delay 2 --variation "${VARIATION:-2}" \
    < /dev/null > build/rig/playrelay.log 2>&1 &
sleep 1
echo "== relay on :$RELAY_PORT  (tail -f build/rig/playrelay.log) =="

launch() {   # launch <n> <boip-port>
    local n=$1 port=$2
    ( cd "$MAME" && setsid env FUJINET_TCP="127.0.0.1:$port" \
        A2600_EMU="$HERE/emu" \
        ./mame a2600 -window -nomaximize -resolution 640x480 \
            -skip_gameinfo \
            -cartslot fujinet -cart "$HERE/build/vo$n.bin" \
            < /dev/null > "$HERE/build/rig/play$n.log" 2>&1 & )
}

launch 1 "$BOIP1"
sleep 2                     # let console 1 connect first, so it is the host
launch 2 "$BOIP2"

cat <<'MSG'

== two consoles up ==

  window 1 is PLAYER1, the host  -- the LEFT tank
  window 2 is PLAYER2, the guest -- the RIGHT tank

  Each player uses joystick 1 on their own console, so in MAME that is the
  arrow keys and Left-Ctrl in whichever window has focus. RESET and SELECT
  (F3 and F2 by default, or 1 and 2) work from either console: the two are
  ANDed on the wire, so either player may press them.

  SELECT steps the game variation, and it only does so BETWEEN games -- it is
  ignored once a game is running, which is how holding it mid-game can no
  longer walk the two consoles onto different variations. Pick the variation
  first, then press RESET to start.

  tail -f build/rig/playrelay.log  what the relay sees
  test/stop.sh                    tear it down
MSG
