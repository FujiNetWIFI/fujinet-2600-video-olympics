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
# Each console's player uses the PADDLE on their own machine's left port: the
# host drives player 0 and the guest player 2, which is where the game itself
# puts the two sides of a two-player variation. Whichever window has focus
# takes the mouse and the keyboard.

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

# THROUGH run.sh, NOT A SECOND COPY OF THE MAME COMMAND LINE. This used to
# spell the arguments out again and quietly lost `-joyport1 pad -joyport2 pad`
# in the process: two windows came up with DIGITAL JOYSTICKS in the slots of a
# paddle game, which runs and is not the game. One place describes the
# controller; everything else goes through it.
launch() {   # launch <n> <boip-port>
    local n=$1 port=$2
    ( setsid env FUJINET_TCP="127.0.0.1:$port" \
        MAME="$MAME" "$HERE/run.sh" "vo$n" \
        < /dev/null > "$HERE/build/rig/play$n.log" 2>&1 & )
}

launch 1 "$BOIP1"
sleep 2                     # let console 1 connect first, so it is the host
launch 2 "$BOIP2"

cat <<'MSG'

== two consoles up ==

  window 1 is PLAYER1, the host  -- player 0, the LEFT paddle
  window 2 is PLAYER2, the guest -- player 2, the RIGHT paddle

  MOVE THE MOUSE to move your paddle, in whichever window has focus. Click in
  a window first so MAME takes the pointer; press the MAME UI key (Scroll Lock
  by default) to give it back. PADDLE_DEVICE=keyboard uses the arrow keys
  instead, if you would rather.

  RESET and SELECT (F3 and F2, or 1 and 2) work from EITHER console: the two
  are ANDed on the wire, so either player may press them and both consoles see
  the same byte on the same tick.

  SELECT steps the variation, and the relay starts the pair on manual game 3 --
  two-player Pong. The ROM only walks variations two remote players can
  actually play: of the fifty, two are single-player and twenty-six want four
  paddles.

  tail -f build/rig/playrelay.log  what the relay sees
  test/stop.sh                    tear it down
MSG
