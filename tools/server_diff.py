#!/usr/bin/env python3
"""Byte-for-byte differential between the two relay implementations.

server/vo_relay_server.py is canonical.  server/c/vo-relay is a C port
of it.  This tool drives BOTH through identical scripted scenarios and compares
every frame the clients receive and every line the server logs.  A non-empty
diff is a bug in the C port -- there is no "close enough" here: the console's
frame reader is a hand-written state machine that misframes on a single
unexpected byte, and test/run_rig.sh's verdict is a grep over the log.

Steps are strictly synchronous: after each one, every client socket is drained
until it has been quiet for QUIET seconds, and the received frames are recorded
in a fixed client order.  That determinism is what makes an exact diff possible.

Only four things may legitimately differ, and each is normalized:
  * the START seed (random per match)  -> payload bytes [1:3] become AAAA
  * log timestamps                     -> the HH:MM:SS prefix is stripped
  * ephemeral ports and the listen port-> tokenized
  * the digit a duplicate name gets    -> tokenized (random in both)

Anything else that differs is a real divergence.

Usage:
    tools/server_diff.py                    # every scenario
    tools/server_diff.py --scenario framing # just the reassembler torture
    tools/server_diff.py --list
"""
import argparse
import difflib
import os
import re
import select
import signal
import socket
import subprocess
import sys
import threading
import time

QUIET = 0.15            # seconds of silence that ends a drain
BOOT_TIMEOUT = 10.0     # seconds to wait for "listening on "

T = {"HELLO": 0x01, "LIST": 0x02, "LOBBY": 0x03, "JOIN": 0x04, "START": 0x05,
     "INPUT": 0x06, "CRC": 0x07, "STATE": 0x08, "RESYNC": 0x09, "BYE": 0x0A,
     "PEER_LEFT": 0x0B, "PING": 0x0C, "PONG": 0x0D}
TNAME = {v: k for k, v in T.items()}

PROTO = 3
TV_NTSC, TV_PAL, TV_SECAM = 0, 1, 2

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RELAY_C = os.path.join(REPO, "server", "c", "vo-relay")
RELAY_PY = os.path.join(REPO, "server", "vo_relay_server.py")

# The 2600 log is bare: "HH:MM:SS " and then the message.  No date, no level,
# and it goes to STDOUT -- see the log() in the Python server.
LOGLINE = re.compile(r"^(\d\d:\d\d:\d\d) (.*)$")
# A sanitizer report or an interpreter traceback carries no timestamp, so the
# log filter below would drop it on the floor and the diff would pass.
SANITIZER = re.compile(r"AddressSanitizer|LeakSanitizer|runtime error:|"
                       r"UndefinedBehaviorSanitizer|Traceback \(most recent")


def frame(ftype, payload=b""):
    return bytes([len(payload) + 1, ftype]) + payload


def hello_payload(name, ver=PROTO, tv=TV_NTSC):
    return bytes([ver, tv]) + name.encode()


IR_CRC = 10                     # keep in step with src/vodefs.inc


def inrec(tick, win=None, crc=0x00):
    """One INPUT payload: tick(1) window(3 ticks x 3 bytes) crc(1) check(1).

    A tick costs three bytes here, not one: paddle A, paddle B, and the
    switches and triggers.  Paddle idle is ZERO -- a paddle is not active-low
    the way a joystick is -- and the switch byte is $FF idle.  Twelve bytes,
    which the relay pads to fourteen on the way out.
    """
    if win is None:
        win = bytes([0x00, 0x00, 0xFF]) * 3
    b = bytes([tick]) + bytes(win) + bytes([crc])
    chk = 0xA5
    for x in b:
        chk ^= x
    return b + bytes([chk])


def free_port():
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


# ---------------------------------------------------------------------------


class Client:
    def __init__(self, ctx, label):
        self.label = label
        self.buf = bytearray()
        self.closed = False
        self.eof_seen = False
        self.sock = socket.create_connection(("127.0.0.1", ctx.port))
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def send(self, data):
        if not self.closed:
            try:
                self.sock.sendall(data)
            except OSError:
                pass

    def close(self):
        if not self.closed:
            self.closed = True
            try:
                self.sock.close()
            except OSError:
                pass


class Ctx:
    """One server process plus the clients talking to it."""

    def __init__(self, impl, extra):
        self.impl = impl
        self.port = free_port()
        self.clients = []
        self.transcript = []
        self.loglines = []
        self._step = 0

        if impl == "py":
            cmd = [sys.executable, RELAY_PY]
        else:
            if not os.access(RELAY_C, os.X_OK):
                sys.exit("%s is not built -- run `make -C server/c`" % RELAY_C)
            cmd = [RELAY_C]
        cmd += ["--host", "127.0.0.1", "--port", str(self.port)]
        cmd += list(extra)

        self.proc = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True,
                                     bufsize=1)
        self._raw_log = []
        self._reader = threading.Thread(target=self._pump, daemon=True)
        self._reader.start()
        self._await_boot()

    def _pump(self):
        for line in self.proc.stdout:
            self._raw_log.append(line.rstrip("\n"))

    def _await_boot(self):
        # Waiting for the log line rather than probing with TCP keeps the
        # readiness check from injecting a connect/drop pair into the
        # transcript -- and it works identically for both implementations,
        # which a fixed sleep would not (Python takes ~80ms to import, the C
        # binary ~2ms).
        t0 = time.monotonic()
        while time.monotonic() - t0 < BOOT_TIMEOUT:
            if any("listening on " in l for l in self._raw_log):
                return
            if self.proc.poll() is not None:
                raise RuntimeError("%s server exited during boot:\n%s"
                                   % (self.impl, "\n".join(self._raw_log)))
            time.sleep(0.01)
        raise RuntimeError("%s server never logged 'listening on '" % self.impl)

    # -- scripting ---------------------------------------------------------

    def connect(self, label):
        c = Client(self, label)
        self.clients.append(c)
        return c

    def hello(self, label, name=None, ver=PROTO, tv=TV_NTSC):
        """Connect, identify, and sync.

        The sync is not optional.  With two connects in flight at once, the
        interleaving of accept() against the first client's readable data is
        genuinely timing-dependent in BOTH implementations -- the Python
        accepts one connection per select() pass, so whether "hello A" lands
        before or after "connect B" is a race, not a semantic.  Syncing after
        every connect removes the race instead of papering over it.
        """
        c = self.connect(label)
        c.send(frame(T["HELLO"], hello_payload(name or label, ver, tv)))
        self.sync("hello %s" % label)
        return c

    def sync(self, step):
        """Drain every client until quiet, then record what arrived."""
        self._step += 1
        tag = "%02d %s" % (self._step, step)
        while True:
            live = [c for c in self.clients if not c.closed]
            if not live:
                break
            r, _, _ = select.select([c.sock for c in live], [], [], QUIET)
            if not r:
                break
            for s in r:
                c = next(x for x in live if x.sock is s)
                try:
                    data = s.recv(65536)
                except OSError:
                    data = b""
                if not data:
                    c.eof_seen = True
                    c.closed = True
                    try:
                        c.sock.close()
                    except OSError:
                        pass
                else:
                    c.buf += data

        for c in self.clients:          # fixed order: creation order
            while len(c.buf) >= 1 and len(c.buf) >= c.buf[0] + 1:
                need = c.buf[0] + 1
                body = bytes(c.buf[1:need])
                del c.buf[:need]
                self.transcript.append("%s | %s <- %s" % (tag, c.label,
                                                          render(body)))
            if c.buf:
                self.transcript.append("%s | %s <- PARTIAL %s"
                                       % (tag, c.label, bytes(c.buf).hex()))
                c.buf.clear()
            if c.eof_seen:
                self.transcript.append("%s | %s <- EOF" % (tag, c.label))
                c.eof_seen = False

    def finish(self):
        for c in self.clients:
            c.close()
        time.sleep(0.2)
        self.proc.send_signal(signal.SIGINT)
        try:
            self.proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=5)
        self._reader.join(timeout=5)
        # Only timestamped records are compared, which would also swallow an
        # ASan/UBSan report or a Python traceback.  Look for those explicitly
        # before discarding anything: under `make -C server/c asan` they are
        # the whole point of the run.
        for line in self._raw_log:
            if SANITIZER.search(line):
                raise RuntimeError(
                    "%s server reported a fault:\n%s"
                    % (self.impl, "\n".join(self._raw_log)))
        if self.proc.returncode not in (0, -signal.SIGINT):
            raise RuntimeError(
                "%s server exited %s:\n%s"
                % (self.impl, self.proc.returncode, "\n".join(self._raw_log)))
        for line in self._raw_log:
            m = LOGLINE.match(line)
            if m:
                self.loglines.append("log %s" % m.group(2))
        return self.transcript + self.loglines


def render(body):
    """One received frame as a comparable string, with the seed masked."""
    t = body[0]
    p = bytearray(body[1:])
    name = TNAME.get(t, "$%02X" % t)
    if t == T["START"] and len(p) >= 3:
        # role seed_lo seed_hi delay variation opponent(8): seed at [1:3]
        p[1:3] = b"\xAA\xAA"            # random per match
        return "START %s (seed masked)" % p.hex()
    return "%s %s" % (name, bytes(p).hex())


# Log lines the C emits that the Python has no equivalent for.  Every entry
# here is a DELIBERATE addition, listed by name so the set cannot grow
# silently -- compare() reports how many lines each run filtered.  Keep this
# list empty unless a line genuinely helps an operator and has nowhere else
# to go.
C_ONLY_LINES = []


def normalize_log(lines):
    out = []
    filtered = 0
    for l in lines:
        if any(pat.match(l) for pat in C_ONLY_LINES):
            filtered += 1
            continue
        l = re.sub(r"seed \$[0-9A-F]{4}", "seed $SEED", l)
        l = re.sub(r"listening on 127\.0\.0\.1:\d+", "listening on 127.0.0.1:PORT", l)
        l = re.sub(r"127\.0\.0\.1:\d+ connected", "127.0.0.1:PORT connected", l)
        # A duplicate name is resolved with random.randrange(10); the digit
        # differs between implementations by construction.  Mask it in the log
        # text and in the hex of any frame that carries the name.
        l = re.sub(r"\b(DUPE)[0-9]\b", r"\1#", l)
        l = re.sub(r"(44555045)3[0-9a-f]", r"\1 3#", l)
        out.append(l)
    return out, filtered


# ---------------------------------------------------------------------------
# Scenarios
# ---------------------------------------------------------------------------


def s_lobby(x):
    """LIST ordering and the eight-entry cap.

    send_lobby() iterates self.clients -- ACCEPT order, not slot order and not
    name order -- and slices the first eight named entries.  A C port that
    walks its client array gets a different order the moment a slot is reused,
    which is exactly what the drop-then-connect below forces.
    """
    a = x.hello("ALICE")
    b = x.hello("BOB")
    a.send(frame(T["LIST"]))
    b.send(frame(T["LIST"]))
    x.sync("LIST with two paired")

    a.close()
    x.sync("ALICE gone")

    # The freed slot is now the lowest free one; a fresh console takes it but
    # must still appear LAST in the lobby.
    c = x.hello("CAROL")
    d = x.hello("DAVE")
    for cl in (b, c, d):
        cl.send(frame(T["LIST"]))
    x.sync("LIST after a slot was reused")

    # More than eight named consoles: the count byte and the cut are both on
    # the wire.
    extra = [x.hello("P%d" % i) for i in range(7)]
    extra[0].send(frame(T["LIST"]))
    x.sync("LIST with more than eight")


def s_pairing(x):
    """Oldest-waiting-first, roles, and the START payload."""
    a = x.hello("ALICE")
    b = x.hello("BOB")                  # A waited, so A hosts
    x.sync("first match")

    c = x.hello("CAROL")
    d = x.hello("DAVE")
    x.sync("second match")

    # JOIN is reserved but still calls try_pair; from a paired console it must
    # do nothing at all.
    a.send(frame(T["JOIN"], b"ALICE"))
    x.sync("JOIN from a paired console")

    # A PAL console pairs with an NTSC one: the sim is tick-driven.
    e = x.hello("EVE", tv=TV_PAL)
    f = x.hello("FRANK", tv=TV_NTSC)
    x.sync("PAL pairs with NTSC")


def s_dupname(x):
    """A duplicate name is uniquified rather than refused."""
    a = x.hello("DUPE")
    b = x.hello("DUPE2", name="DUPE")   # label differs, wire name collides
    x.sync("duplicate name")
    a.send(frame(T["LIST"]))
    x.sync("LIST shows both")


def s_relay(x):
    """Forwarding, padding, and the checksum pairing."""
    a = x.hello("ALICE")
    b = x.hello("BOB")
    x.sync("paired")

    # Agreeing checksums: no log line, just the forwarded frames.
    for t in range(4):
        a.send(frame(T["INPUT"], inrec(t, crc=0x10 + t)))
        b.send(frame(T["INPUT"], inrec(t, crc=0x10 + t)))
    x.sync("four agreeing ticks")

    # A disagreement: the SECOND reporter's name leads the log line.
    a.send(frame(T["INPUT"], inrec(9, crc=0x55)))
    b.send(frame(T["INPUT"], inrec(9, crc=0x66)))
    x.sync("one mismatched tick")

    # The 8-bit tick wraps; lap reconstruction has to keep the two consoles
    # paired across it.  A bare `tick < last` would count a reorder as a wrap.
    #
    # One sync PER TICK, deliberately.  Nine records from each side in a single
    # step is a race in both implementations, not a semantic: if the loop reads
    # all nine of ALICE's before any of BOB's, the ninth insert evicts tick 250
    # before its partner ever arrives and the pair count comes out one short.
    # Draining between ticks removes the race rather than papering over it.
    for t in (250, 251, 252, 253, 254, 255, 0, 1, 2):
        a.send(frame(T["INPUT"], inrec(t, crc=t)))
        b.send(frame(T["INPUT"], inrec(t, crc=t)))
        x.sync("tick %d across the wrap" % t)

    # Unpaired entries have to be evicted by AGE.  Only one side reports.
    for t in range(20, 40):
        a.send(frame(T["INPUT"], inrec(t, crc=t)))
    x.sync("twenty unanswered ticks")

    # A SHORT record is padded on the way out but never truncated on the way
    # in; an OVER-LONG one is forwarded at its own length.
    a.send(frame(T["INPUT"], b"\x01\x02"))
    a.send(frame(T["STATE"], b"\x01" * 40))
    a.send(frame(T["CRC"], b""))
    a.send(frame(T["RESYNC"], b"\x07"))
    x.sync("short, long and empty relay frames")

    b.send(frame(T["PING"]))
    x.sync("PING")

    a.send(frame(T["LIST"]))
    x.sync("LIST mid-match shows status 1")


def s_crcevict(x):
    """Eviction by AGE, not by key -- the false-mismatch bug of PORTING.md 4.19.

    The two rules only differ when insertion order differs from key order, so
    the ticks here arrive DESCENDING: a reorder, which the lap reconstruction
    correctly declines to treat as a wrap.  Eleven unpaired records then leave
    the eight NEWEST (ticks 97..90) under the real rule and the eight
    LOWEST-KEYED (100..93) under the wrong one, and the partner reports tick
    90 -- present in one, evicted in the other.
    """
    a = x.hello("ALICE")
    b = x.hello("BOB")
    x.sync("paired")

    for t in range(100, 89, -1):        # 100, 99, ... 90: eleven records
        a.send(frame(T["INPUT"], inrec(t, crc=t)))
    x.sync("eleven unpaired ticks, descending")

    b.send(frame(T["INPUT"], inrec(90, crc=0xEE)))
    x.sync("the partner reports the oldest surviving tick")



def s_drops(x):
    """PEER_LEFT, the survivor re-pairing, and BYE."""
    a = x.hello("ALICE")
    b = x.hello("BOB")
    x.sync("paired")

    a.close()
    x.sync("host leaves")

    # The survivor stays in the lobby and pairs with the next arrival -- and
    # is now the one that waited, so it hosts.
    c = x.hello("CAROL")
    x.sync("survivor re-pairs")

    b.send(frame(T["BYE"]))
    x.sync("BYE")

    # An unknown frame type is logged and the connection SURVIVES.
    c.send(frame(0x7F, b"\x01\x02"))
    x.sync("unknown frame type")
    c.send(frame(T["PING"]))
    x.sync("still alive after the unknown type")


def s_framing(x):
    """The reassembler, one byte at a time."""
    a = x.connect("DRIBBLE")
    for byte in frame(T["HELLO"], hello_payload("DRIBBLE")):
        a.send(bytes([byte]))
        time.sleep(0.005)
    x.sync("HELLO dribbled one byte at a time")

    b = x.connect("SPLIT")
    whole = frame(T["HELLO"], hello_payload("SPLIT"))
    b.send(whole[:3])
    x.sync("half a HELLO")
    b.send(whole[3:])
    x.sync("the other half")

    # Two frames in one segment, then a frame split across two segments.
    b.send(frame(T["PING"]) + frame(T["PING"]))
    x.sync("two frames in one write")

    # A zero length byte cannot be a frame: the link drops.
    z = x.connect("ZERO")
    z.send(b"\x00")
    x.sync("zero length byte")

    # ...and so does an over-long one.
    o = x.connect("OVERLONG")
    o.send(b"\xFF")
    x.sync("length 255")


def s_limits(x):
    """Refusals: version, television standard, name, and the per-IP cap."""
    v = x.connect("OLDVER")
    v.send(frame(T["HELLO"], hello_payload("OLDVER", ver=1)))
    x.sync("protocol version 1 refused")

    e = x.connect("EMPTY")
    e.send(frame(T["HELLO"], b""))
    x.sync("an empty HELLO payload")

    s = x.connect("SECAM")
    s.send(frame(T["HELLO"], hello_payload("SECAM", tv=TV_SECAM)))
    x.sync("SECAM refused")

    u = x.connect("UNKTV")
    u.send(frame(T["HELLO"], hello_payload("UNKTV", tv=9)))
    x.sync("unknown television standard refused")

    short = x.connect("SHORT")
    short.send(frame(T["HELLO"], hello_payload("A")))
    x.sync("a one-character name")

    lower = x.connect("LOWER")
    lower.send(frame(T["HELLO"], hello_payload("bob")))
    x.sync("a lower-case name is upper-cased")

    bad = x.connect("BADCHR")
    bad.send(frame(T["HELLO"], bytes([PROTO, TV_NTSC]) + b"A-B"))
    x.sync("a punctuation character in a name")

    hi = x.connect("HIBYTE")
    hi.send(frame(T["HELLO"], bytes([PROTO, TV_NTSC]) + b"AB\xC3\xA9"))
    x.sync("a non-ASCII byte in a name")

    padded = x.connect("PADDED")
    padded.send(frame(T["HELLO"], bytes([PROTO, TV_NTSC]) + b"  PAD \x00\x00"))
    x.sync("a name with NUL and space padding")

    # Nine connections from one address: the ninth is refused silently.
    hoard = [x.connect("IP%d" % i) for i in range(9)]
    x.sync("per-IP connection cap")


SCENARIOS = {
    "lobby":   s_lobby,
    "pairing": s_pairing,
    "dupname": s_dupname,
    "relay":   s_relay,
    "crcevict": s_crcevict,
    "drops":   s_drops,
    "framing": s_framing,
    "limits":  s_limits,
}
DEFAULT_ORDER = ["lobby", "pairing", "dupname", "relay", "crcevict", "drops",
                 "framing", "limits"]


# ---------------------------------------------------------------------------


def run(impl, name, extra=()):
    x = Ctx(impl, extra)
    try:
        SCENARIOS[name](x)
        x.sync("final")
    finally:
        lines = x.finish()
    return normalize_log(lines)


def compare(name, extra):
    py, _ = run("py", name, extra)
    c, c_filtered = run("c", name, extra)
    note = " [+%d deliberate C-only line(s)]" % c_filtered if c_filtered else ""
    if py == c:
        print("  ok   %-8s %d lines%s" % (name, len(py), note))
        return True
    print("  FAIL %-8s%s" % (name, note))
    for line in difflib.unified_diff(py, c, "python", "c", lineterm="", n=2):
        print("       " + line)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scenario", action="append",
                    help="run only this scenario (repeatable)")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--arg", action="append", default=[],
                    help="extra flag passed to BOTH servers (repeatable)")
    args = ap.parse_args()

    if args.list:
        for k in DEFAULT_ORDER:
            print("%-10s %s" % (k, (SCENARIOS[k].__doc__ or "").split("\n")[0]))
        return 0

    names = args.scenario or DEFAULT_ORDER
    for n in names:
        if n not in SCENARIOS:
            sys.exit("unknown scenario %r; try --list" % n)

    print("server_diff: %d scenario(s)" % len(names))
    ok = True
    for n in names:
        if not compare(n, args.arg):
            ok = False
    print()
    if ok:
        print("SERVER_DIFF PASS: the C port matches the Python byte for byte")
        return 0
    print("SERVER_DIFF FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main())
