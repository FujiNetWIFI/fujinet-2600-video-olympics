#!/usr/bin/env python3
"""latency_probe_server.py -- a TCP echo server for the M1 transaction probe.

Every latency estimate in this port depends on one number: how many video
frames a single FujiNet mailbox transaction costs. PORTING.md's §2 is emphatic
that the number must be measured and not assumed -- the Intellivision family
spent years believing a tick rate that was three times too optimistic -- so
this is the first thing that runs, before any Combat work at all.

It echoes whatever it is sent, immediately, with Nagle off, and reports the
inter-arrival gaps it saw so the server's view can be compared with the
console's. If the two disagree the time is going somewhere between them:
fujinet-pc, the BoIP hop, or the cartridge model.

Usage: latency_probe_server.py [--port 9605] [--host 127.0.0.1]
"""

import argparse
import socket
import selectors
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9605)
    ap.add_argument("--host", default="127.0.0.1")
    args = ap.parse_args()

    lsock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    lsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    lsock.bind((args.host, args.port))
    lsock.listen(8)
    lsock.setblocking(False)

    sel = selectors.DefaultSelector()
    sel.register(lsock, selectors.EVENT_READ, None)
    print(f"echo: listening on {args.host}:{args.port}", flush=True)

    # Per-connection: (rounds, first_ts, last_ts, total_bytes)
    stats = {}

    while True:
        for key, _ in sel.select(timeout=5.0):
            if key.data is None:
                conn, addr = lsock.accept()
                conn.setblocking(False)
                # Without this the echo itself is subject to Nagle and the
                # measurement reports the delayed-ACK timer, not the path.
                conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                sel.register(conn, selectors.EVENT_READ, addr)
                stats[conn] = [0, None, None, 0]
                print(f"echo: {addr[0]}:{addr[1]} connected", flush=True)
                continue

            conn = key.fileobj
            try:
                data = conn.recv(4096)
            except OSError:
                data = b""
            if not data:
                s = stats.pop(conn, [0, None, None, 0])
                if s[0] > 1 and s[1] is not None:
                    span = s[2] - s[1]
                    print("echo: %s closed after %d rounds, %d bytes, "
                          "%.2f ms mean inter-arrival"
                          % (key.data, s[0], s[3], 1000.0 * span / (s[0] - 1)),
                          flush=True)
                else:
                    print(f"echo: {key.data} closed after {s[0]} rounds",
                          flush=True)
                sel.unregister(conn)
                conn.close()
                continue

            now = time.monotonic()
            s = stats[conn]
            s[0] += 1
            s[3] += len(data)
            if s[1] is None:
                s[1] = now
            s[2] = now
            try:
                conn.sendall(data)
            except OSError:
                pass


if __name__ == "__main__":
    main()
