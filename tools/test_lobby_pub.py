#!/usr/bin/env python3
"""test_lobby_pub.py -- the Lobby registration contract, against a mock.

The FujiNet Lobby has no expiry job. `status` is whatever the publisher last
said, and a server that stops POSTing stays in every client's list for ever
with a growing pingage. So three things have to be true, and none of them is
visible by looking at the code:

  * the registration POST carries platform "a2600" -- that is what the 2600
    Lobby client queries, and a row published under any other spelling simply
    never appears in it
  * a keepalive re-POST happens unprompted, or the room goes stale
  * a clean shutdown publishes status "offline", or the room outlives the server

It runs against a mock Lobby on an ephemeral port, so it needs no network and
cannot touch the real one -- which matters more than it sounds: this family has
rewritten a machine-wide appkey by pointing a test at production before now.

Usage: test_lobby_pub.py
"""

import json
import os
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

_SRV = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                    "..", "server", "vo_relay_server.py")
import importlib.util as _ilu
_spec = _ilu.spec_from_file_location("vo_relay_server", _SRV)
_mod = _ilu.module_from_spec(_spec)
_spec.loader.exec_module(_mod)
PROTO = _mod.PROTO

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
posts = []
fails = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        fails.append(what)


class Mock(BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(n)
        try:
            posts.append(json.loads(body))
        except ValueError:
            posts.append({"_unparseable": body.decode("latin1")})
        self.send_response(201)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(b'{"success":true}')

    def log_message(self, *a):
        pass


def main():
    srv = HTTPServer(("127.0.0.1", 0), Mock)
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    relay = subprocess.Popen(
        [sys.executable, os.path.join(HERE, "server/vo_relay_server.py"),
         "--host", "127.0.0.1", "--port", "9641",
         "--lobby-url", "http://127.0.0.1:%d/server" % port,
         # A short keepalive, so the test does not take five minutes.
         "--game-name", "Video Olympics",
         "--server-name", "Video Olympics Netplay"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        for _ in range(100):
            if posts:
                break
            time.sleep(0.05)

        check(len(posts) >= 1, "it registers on startup without being asked")
        if not posts:
            return 1
        p = posts[0]
        check(p.get("status") == "online", 'the first POST says status "online"')
        check(p.get("game") == "Video Olympics", "it publishes the game name")
        check(isinstance(p.get("appkey"), int) and p["appkey"] > 0,
              "it publishes a nonzero appkey (the Lobby rejects zero)")
        check(p.get("maxplayers") == 2, "it publishes two seats")
        clients = p.get("clients") or [{}]
        check(clients[0].get("platform") == "a2600",
              'the platform is "a2600" -- what the 2600 Lobby client queries')
        check(bool(clients[0].get("url")), "a client URL is published for it")
        check(bool(p.get("serverurl")), "a server URL is published")

        before = len(posts)
        # Connect a console: a player count change must re-publish.
        import socket
        s = socket.create_connection(("127.0.0.1", 9641), timeout=3)
        name = b"TESTER"
        # HELLO is ver(1) tv(1) name: protocol 2, NTSC. The version moved
        # with the payload SHAPE -- a relay reading version 1 would take the
        # television byte for the first letter of the name.
        # PROTO comes from the relay itself. A literal here declared version 2
        # for as long as that was current and then silently stopped pairing when
        # the record widened -- the relay dropped the link, curplayers stayed at
        # zero, and the failure read as "the re-publish carries the new player
        # count", which is not where the problem was.
        s.sendall(bytes([3 + len(name), 0x01, PROTO, 0]) + name)  # len counts type+payload
        for _ in range(60):
            if len(posts) > before:
                break
            time.sleep(0.05)
        check(len(posts) > before, "a player joining re-publishes the count")
        if len(posts) > before:
            check(posts[-1].get("curplayers", 0) >= 1,
                  "the re-publish carries the new player count")
        s.close()
    finally:
        relay.terminate()
        try:
            relay.communicate(timeout=8)
        except subprocess.TimeoutExpired:
            relay.kill()
        srv.shutdown()

    # The offline POST is published from the calling thread during shutdown, so
    # it is the last one recorded.
    check(any(q.get("status") == "offline" for q in posts),
          'a clean shutdown publishes status "offline"')

    print()
    if fails:
        print("LOBBY FAIL: %d checks failed" % len(fails))
        print(json.dumps(posts, indent=2)[:1200])
        return 1
    print("LOBBY PASS (%d posts)" % len(posts))
    return 0


if __name__ == "__main__":
    sys.exit(main())
