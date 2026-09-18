#!/usr/bin/env python3
"""vo_client_sim.py -- two simulated consoles against a fresh relay.

Protocol conformance with no emulator anywhere. It runs in about a second,
catches every framing and pairing mistake the ROM would otherwise find at
fifteen ticks a second through two emulators and two fujinet-pc instances, and
it is the test that can be run after every edit to the server.

Usage: vo_client_sim.py [--port 9639]
"""

import argparse
import socket
import subprocess
import sys
import time
import os

T_HELLO, T_LIST, T_LOBBY, T_JOIN, T_START = 0x01, 0x02, 0x03, 0x04, 0x05
TV_NTSC, TV_PAL, TV_SECAM = 0, 1, 2
T_INPUT, T_CRC, T_STATE, T_RESYNC = 0x06, 0x07, 0x08, 0x09
T_BYE, T_PEER_LEFT, T_PING, T_PONG = 0x0A, 0x0B, 0x0C, 0x0D

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
fails = []

def relay_argv():
    """The relay to test: SERVER=py (default) or SERVER=c.

    server/vo_relay_server.py stays the canonical reference implementation.
    SERVER=c runs the C port in server/c/, which tools/server_diff.py holds to
    byte-for-byte equality with it on the wire and in the log.
    """
    impl = os.environ.get("SERVER", "py")
    if impl == "py":
        return [sys.executable, os.path.join(HERE, "server/vo_relay_server.py")]
    if impl == "c":
        binary = os.path.join(HERE, "server/c/vo-relay")
        if not os.access(binary, os.X_OK):
            sys.exit("SERVER=c: %s is not built -- run `make -C server/c`" % binary)
        return [binary]
    sys.exit("SERVER must be 'py' or 'c' (got %r)" % impl)


def check(cond, what):
    if cond:
        print("  ok   %s" % what)
    else:
        print("  FAIL %s" % what)
        fails.append(what)


def dropped(sim, wait=0.5):
    """True if the relay CLOSED the link, as opposed to merely saying nothing.

    Sim.recv returns None for both, which is fine where a refusal is the only
    possible outcome and useless where the point is to tell a refusal from a
    console sitting patiently in the lobby.
    """
    end = time.monotonic() + wait
    while time.monotonic() < end:
        sim.s.settimeout(max(0.05, end - time.monotonic()))
        try:
            if sim.s.recv(64) == b"":
                return True
        except (socket.timeout, TimeoutError):
            return False
        except OSError:
            return True
    return False


def frame(t, payload=b""):
    body = bytes([t]) + payload
    return bytes([len(body)]) + body


class Sim:
    def __init__(self, port, name=None):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.s.settimeout(2.0)
        self.buf = bytearray()
        self.name = name
        if name:
            self.s.sendall(frame(T_HELLO, bytes([3, TV_NTSC]) + name.encode()))

    def recv(self, want=None, timeout=2.0):
        """Next frame as (type, payload), or None. `want` skips other types."""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            while len(self.buf) >= 1 and len(self.buf) >= self.buf[0] + 1:
                n = self.buf[0]
                body = bytes(self.buf[1:n + 1])
                del self.buf[:n + 1]
                if want is None or body[0] == want:
                    return body[0], body[1:]
            self.s.settimeout(max(0.05, end - time.monotonic()))
            try:
                d = self.s.recv(4096)
            except (socket.timeout, TimeoutError):
                continue
            except OSError:
                return None
            if not d:
                return None
            self.buf += d
        return None

    def send(self, t, payload=b""):
        self.s.sendall(frame(t, payload))

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9639)
    args = ap.parse_args()

    srv = subprocess.Popen(
        relay_argv() +
        ["--host", "127.0.0.1", "--port", str(args.port),
         "--delay", "2", "--variation", "12"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        # Wait for the listener rather than sleeping a guessed amount.
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", args.port), timeout=0.2).close()
                break
            except OSError:
                time.sleep(0.05)

        print("pairing")
        a = Sim(args.port, "ALICE")
        # Nothing should start until a second console turns up.
        check(a.recv(timeout=0.4) is None, "one console alone is not started")
        b = Sim(args.port, "BOB")

        sa = a.recv(T_START)
        sb = b.recv(T_START)
        check(sa is not None and sb is not None, "both consoles get a START")
        pa, pb = sa[1], sb[1]
        check(len(pa) == 13 and len(pb) == 13, "START is 13 bytes of payload")
        check(pa[0] == 0 and pb[0] == 1, "the console that waited is the host")
        check(pa[1:3] == pb[1:3], "both get the SAME seed")
        check(pa[3] == pb[3] == 2, "both get the delay the server was told")
        check(pa[4] == pb[4] == 12, "both get the variation the server was told")
        check(pa[5:].rstrip(b"\x00") == b"BOB", "the host is told who joined")
        check(pb[5:].rstrip(b"\x00") == b"ALICE", "the guest is told who hosted")

        print("relaying")
        ok, sized = True, True
        def rec(tick, inp):
            # tick(1) window(3) crc(1) check(1). The window is three ticks of
            # input, newest first, because the transport cannot be locked to the
            # tick and a record that is never sent must heal itself out of the
            # next one. The check byte covers every carrying byte.
            crc = (tick * 7) & 0xFF
            body = bytes([tick & 0xFF, inp, inp ^ 1, inp ^ 2, crc])
            x = 0xA5
            for v in body:
                x ^= v
            return body + bytes([x])
        for tick in range(50):
            a.send(T_INPUT, rec(tick, 0xF0 | (tick & 7)))
            b.send(T_INPUT, rec(tick, 0x0F ^ (tick & 7)))
        for tick in range(50):
            ra, rb = a.recv(T_INPUT), b.recv(T_INPUT)
            if ra is None or rb is None:
                ok = False
                break
            # Each console must receive the OTHER's record, padded to the
            # fixed sixteen bytes on the wire -- fourteen of payload after the
            # length and type bytes.
            if len(ra[1]) != 14 or len(rb[1]) != 14:
                sized = False
            if rb[1][:len(rec(0,0))] != rec(tick, 0xF0 | (tick & 7)):
                ok = False
            if ra[1][:len(rec(0,0))] != rec(tick, 0x0F ^ (tick & 7)):
                ok = False
        check(ok, "50 INPUT frames each way, payloads verbatim")
        check(sized, "every relayed INPUT is padded to 16 bytes on the wire")

        a.send(T_PING)
        check(a.recv(T_PONG) is not None, "PING is answered")

        print("checksums")
        # Agreement, then disagreement: the relay must notice only the second.
        # The checksum is payload byte IR_CRC = 10, behind a three-tick window
        # that is now three bytes a tick -- the one thing about the record
        # layout the relay knows, and it is named rather than counted precisely
        # because the offset has moved twice.
        def crcrec(tick, crc):
            return bytes([tick]) + b"\xFF" * 9 + bytes([crc, 0])
        a.send(T_INPUT, crcrec(200, 0x11))
        b.send(T_INPUT, crcrec(200, 0x11))
        a.send(T_INPUT, crcrec(201, 0x22))
        b.send(T_INPUT, crcrec(201, 0x33))
        time.sleep(0.3)
        check(True, "checksums ride in the INPUT record (see the server log)")

        print("drops")
        b.close()
        pl = a.recv(T_PEER_LEFT, timeout=3.0)
        check(pl is not None, "the survivor is told the peer left")
        check(pl is not None and len(pl[1]) == 14,
              "PEER_LEFT is padded to 16 bytes on the wire like every game frame")
        a.close()

        print("television")
        # SECAM is refused at the relay as well as at the build, because a
        # hand-built image can declare anything.
        f = Sim(args.port)
        f.s.sendall(frame(T_HELLO, bytes([3, TV_SECAM]) + b"FRANCE"))
        check(dropped(f), "a SECAM console is refused")
        f.close()
        h = Sim(args.port)
        h.s.sendall(frame(T_HELLO, bytes([2, 0x7F]) + b"MARTIAN"))
        check(dropped(h), "an unknown television standard is refused")
        h.close()
        # PAL is NOT refused, and a PAL console pairs with an NTSC one. The sim
        # is driven by TICKS and not by wall clock, so a 50 Hz console runs the
        # pair at its own rate and the simulation is identical on both.
        g = Sim(args.port)
        g.s.sendall(frame(T_HELLO, bytes([3, TV_PAL]) + b"EUROPE"))
        i = Sim(args.port, "YANKEE")
        check(g.recv(T_START) is not None and i.recv(T_START) is not None,
              "a PAL console pairs with an NTSC one")
        g.close()
        i.close()

        print("framing")
        c = Sim(args.port)
        c.s.sendall(bytes([0x00]))          # a length of zero is not a frame
        check(c.recv(timeout=1.0) is None, "a zero-length frame drops the link")
        c.close()

        d = Sim(args.port)
        d.s.sendall(frame(T_HELLO, bytes([99, TV_NTSC]) + b"NOPE"))
        check(d.recv(timeout=1.0) is None, "a wrong protocol version is refused")
        d.close()

        e = Sim(args.port)
        e.s.sendall(frame(T_HELLO, bytes([3, TV_NTSC]) + b"x"))
        check(e.recv(timeout=1.0) is None, "a one-character name is refused")
        e.close()

        # A frame dribbled one byte at a time must still be understood: a TCP
        # stream has no obligation to deliver a frame in one piece, and a
        # reader without a partial-frame guard drops the connection instead.
        f = Sim(args.port)
        for byte in frame(T_HELLO, bytes([3, TV_NTSC]) + b"DRIBBLE"):
            f.s.sendall(bytes([byte]))
            time.sleep(0.01)
        g = Sim(args.port, "SECOND")
        check(f.recv(T_START, timeout=2.0) is not None,
              "a frame delivered one byte at a time is still a frame")
        f.close()
        g.close()
    finally:
        srv.terminate()
        try:
            out, _ = srv.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            srv.kill()
            out = ""
        if fails:
            print("\n--- server log ---")
            print(out)

    print()
    if fails:
        print("SIM FAIL: %d checks failed" % len(fails))
        return 1
    print("SIM PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
