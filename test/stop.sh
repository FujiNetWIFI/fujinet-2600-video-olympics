#!/usr/bin/env bash
# stop.sh -- tear down whatever test/run_play.sh brought up.
#
# It kills ONLY this project's instances: MAME by exact process name, the
# relay by its script path, and fujinet-pc by the working directory it was
# started in. A bare `pkill -f fujinet` on this machine would take out several
# long-running ones that have nothing to do with this.
cd "$(dirname "$0")/.."
HERE=$(pwd)
# TERM, then wait, then KILL. MAME does not always act on a TERM while it owns
# an SDL window -- two runs' worth of windows survived one and sat there for
# twenty minutes, and the next launch simply added two more on top. Escalating
# is the difference between a teardown and a suggestion.
pkill -x mame 2>/dev/null || true
for _ in 1 2 3 4 5 6 7 8 9 10; do
    pgrep -x mame > /dev/null || break
    sleep 0.3
done
pkill -9 -x mame 2>/dev/null || true
pkill -f "vo_relay_server.py" 2>/dev/null || true
for pid in $(pgrep -x fujinet 2>/dev/null || true); do
    case "$(readlink -f "/proc/$pid/cwd" 2>/dev/null)" in
        "$HERE"/build/rig/*) kill "$pid" 2>/dev/null ;;
    esac
done
echo "stopped"
