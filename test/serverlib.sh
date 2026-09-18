# Shared relay-server launcher for the test scripts.  Source, don't execute:
#     . "$(dirname "$0")/serverlib.sh"
#
# SERVER=py (default) runs server/vo_relay_server.py, which stays the
# canonical reference implementation.  SERVER=c runs the C port in server/c/,
# which tools/server_diff.py holds to byte-for-byte equality with it on the
# wire and in the log -- including the two lines the rigs grep for, "match:"
# and "CRC MISMATCH".
#
# Callers background and redirect as before:
#     setsid relay_server --host 127.0.0.1 --port "$P" < /dev/null > "$LOG" 2>&1 &
#     RELAY_PID=$!
# The exec below replaces the subshell, so $! is still the real server PID and
# every existing kill/trap keeps working.

relay_server() {
    case "${SERVER:-py}" in
    py)
        exec python3 server/vo_relay_server.py "$@"
        ;;
    c)
        if [ ! -x server/c/vo-relay ]; then
            echo "SERVER=c: server/c/vo-relay is not built -- run" >&2
            echo "  make -C server/c" >&2
            exit 1
        fi
        exec server/c/vo-relay "$@"
        ;;
    *)
        echo "SERVER must be 'py' or 'c' (got '$SERVER')" >&2
        exit 1
        ;;
    esac
}
