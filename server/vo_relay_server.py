#!/usr/bin/env python3
"""vo_relay_server.py -- the relay for networked Atari 2600 Video Olympics.

Port 9600. A fork of the Intellivision family's relay, whose single most
important design decision is carried over unchanged: THE RELAY IS DUMB. It
pairs two consoles, hands each a role and a seed, and then forwards frames
between them without ever looking inside one. It does not know what a tick is.
Every piece of synchronisation lives in the ROM, where it has to, because the
ROM is the only thing that knows when a frame has been simulated.

FujiNet has no listener and no NAT story, so both consoles connect outwards and
this is a mandatory relay rather than a matchmaker that steps out of the way.

    frame := len(1) type(1) payload(len-1)
    `len` counts type+payload, so a frame is never zero length and a bad one is
    visible on sight.

    C->S  $01 HELLO   ver(1)=2, tv(1) 0=NTSC 1=PAL (2=SECAM is refused),
                      name 2-8 of A-Z0-9
    C->S  $02 LIST                      (reserved; the ROM auto-pairs)
    S->C  $03 LOBBY   count(1), then name(8) status(1) per entry
    C->S  $04 JOIN    name               (reserved)
    S->C  $05 START   role(1) seed_lo seed_hi delay(1) variation(1) opp(8)
    C<->C $06 INPUT   tick(1) input(3) crc(1) check(1)
    C<->C $08 STATE   pos(1) data(5)
    C->S  $0A BYE
    S->C  $0B PEER_LEFT   pad(6)
    C->S  $0C PING  ->  S->C $0D PONG

The input byte packs one player's whole console into eight bits, which is what
lets the console keep a ring of it:

    bit 7  trigger,    1 = released      (INPT4's sense, straight through)
    bit 6  difficulty, 1 = A / Pro       (SWCHB's sense)
    bit 5  select,     1 = not pressed
    bit 4  reset,      1 = not pressed
    bits 3-0  the stick, SWCHA's sense (1 = not pushed): up, down, left, right

One byte a player a tick means the remote ring is eight bytes of a zero page
with twenty-six free, and eight is the size it has to be: a console can be
holding records for ticks T..T+2d, which is five at d=2, and eight is the next
power of two, for a free AND #7 instead of a modulo.

EVERY FRAME A CONSOLE CAN RECEIVE DURING PLAY IS EXACTLY EIGHT BYTES ON THE
WIRE. That is the one place this protocol departs from the Intellivision
family's, and it is worth the padding. The 2600 has no buffer to reassemble a
split frame in -- the 128 bytes of console RAM are the game's, and the reply
window is repainted by the next transaction -- so a READ that cut a frame in
half would lose the tail and misframe the stream forever. With a fixed size the
console reads `(avail >> 3) << 3` bytes, which is three shifts, and a partial
frame is simply not read this time. The re-sync path the family needs
(PORTING.md §7.3) becomes structurally unreachable rather than carefully
handled.

The checksum rides in the INPUT record rather than in a CRC frame of its own,
for the same reason: one frame type during play, one size, one parser. The
relay pairs the crc bytes by tick and says whether the two consoles ever
disagreed.

The session frames -- HELLO, START, LOBBY -- are NOT padded. The boot bank
reads them one at a time, knows each one's length, and is under no time
pressure at all.

Usage: vo_relay_server.py [--port 9600] [--delay 2] [--lobby-url ...]
"""

import argparse
import json
import random
import signal
import selectors
import socket
import string
import sys
import threading
import time
import urllib.request

T_HELLO, T_LIST, T_LOBBY, T_JOIN, T_START = 0x01, 0x02, 0x03, 0x04, 0x05
T_INPUT, T_CRC, T_STATE, T_RESYNC = 0x06, 0x07, 0x08, 0x09
T_BYE, T_PEER_LEFT, T_PING, T_PONG = 0x0A, 0x0B, 0x0C, 0x0D

# Forwarded to the partner verbatim, never interpreted.
RELAY_TYPES = {T_INPUT, T_CRC, T_STATE, T_RESYNC}

# Every frame a console can receive DURING PLAY is this long on the wire, and
# it is a POWER OF TWO on purpose: the console reads (avail AND $F0) and leaves
# a partial frame where it is, which makes misframing structurally unreachable.
#
# SIXTEEN, WHERE COMBAT USED EIGHT. A paddle position is a whole byte where a
# joystick was a nibble, and every record carries a WINDOW of three ticks, so a
# tick costs three bytes on the wire -- paddle A, paddle B, and the switches
# and triggers. Payload is tick(1) + 3*3 + crc(1) + check(1) = 12, and 14 on
# the wire with the length and type bytes.
#
# The second paddle is carried from day one even though only paddle A is wired
# up: four-player Quadrapong and Foozpong then cost a mixer change rather than
# a protocol version bump, and the record was going to be padded to 16 anyway.
GAME_FRAME = 16
GAME_PAYLOAD = GAME_FRAME - 2

# Offsets inside an INPUT payload. Keep in step with IR_* in src/vodefs.inc.
IR_TICK, IR_IN, IR_CRC, IR_CHK = 0, 1, 10, 11

PROTO = 3                   # 2 put the television standard between the version
                            # byte and the name. 3 widened a tick's input from
                            # one byte to three, which is another SHAPE change:
                            # an older relay would pair the wrong checksums.
# THE VARIATION WHITELIST.
#
# Video Olympics has fifty variations and the manual numbers them 1-50; the
# ROM's own cell, $96, holds 0-49, so a manual game N is $96 = N-1.
#
# Two of them are single-player -- Robot Pong, and Pong against the computer --
# and are meaningless over a network. Twenty-six are four-player. Until the
# second paddle slot is wired up at both ends, the twenty-two two-player games
# are the ones that work, and those are the ones SELECT walks.
#
# It lives here as well as in the ROM because the START frame carries the
# variation the match begins on, and a relay that could start a pair on Robot
# Pong would be handing them a game one of them cannot play.
TWO_PLAYER = [n - 1 for n in (3, 4, 9, 10, 13, 14, 19, 20,
                              23, 24, 25, 26, 27, 28,
                              35, 36, 39, 40, 43, 44, 45, 46)]
DEFAULT_VARIATION = TWO_PLAYER[0]       # manual game 3: two-player Pong

TV_NTSC, TV_PAL, TV_SECAM = 0, 1, 2
TV_NAME = {TV_NTSC: "NTSC", TV_PAL: "PAL", TV_SECAM: "SECAM"}
DEFAULT_DELAY = 2           # ticks; measured at 3 frames a tick, so ~100 ms
MAX_TX_BACKLOG = 64 * 1024
MAX_CLIENTS = 64
MAX_CONNECTIONS_PER_IP = 8  # generous: FujiNets can share a NAT address
HELLO_TIMEOUT = 60.0
STATS_INTERVAL = 60.0
NAME_OK = set(string.ascii_uppercase + string.digits)


def frame(ftype, payload=b""):
    body = bytes([ftype]) + payload
    assert len(body) <= 255, "frame too long"
    return bytes([len(body)]) + body


def log(fmt, *a):
    print(("%s " % time.strftime("%H:%M:%S")) + (fmt % a if a else fmt), flush=True)


class Client:
    def __init__(self, sock, addr):
        self.sock = sock
        self.addr = addr
        self.rx = bytearray()
        self.tx = bytearray()
        self.name = None
        self.tv = None          # the television standard HELLO declared
        self.partner = None
        self.role = None
        self.born = time.monotonic()
        self.crc = {}
        # The wire tick is EIGHT BITS and wraps every 256 ticks -- about
        # seventeen seconds at fifteen a second. The console does not care:
        # every comparison it makes is signed, which hides a wrap across a
        # window of +-127. The relay does care, because it pairs checksums BY
        # tick, and lap 2's tick 5 is a different tick from lap 1's.
        #
        # So the lap is reconstructed here, from the stream itself: a tick that
        # jumps backwards by more than half the range is a wrap, not a reorder.
        # Pairing then uses lap*256+tick, and two consoles a few ticks apart
        # either side of a wrap still line up.
        self.lap = 0
        self.last_tick = None

    def send(self, data):
        self.tx += data


class Server:
    def __init__(self, args):
        self.args = args
        self.sel = selectors.DefaultSelector()
        self.clients = {}
        self.by_name = {}
        self.stats = dict(matches=0, crc_ok=0, crc_bad=0, bad_frames=0,
                          tx_drops=0, hello_timeouts=0, ip_rejects=0)
        self.lobby = None

        self.lsock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.lsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.lsock.bind((args.host, args.port))
        self.lsock.listen(8)
        self.lsock.setblocking(False)
        self.sel.register(self.lsock, selectors.EVENT_READ, None)
        log("listening on %s:%d, delay %d ticks", args.host, args.port, args.delay)

    # ---------------------------------------------------------------- accept
    def accept(self):
        try:
            sock, addr = self.lsock.accept()
        except OSError:
            return
        if len(self.clients) >= MAX_CLIENTS:
            sock.close()
            return
        same = sum(1 for c in self.clients.values() if c.addr[0] == addr[0])
        if same >= MAX_CONNECTIONS_PER_IP:
            self.stats["ip_rejects"] += 1
            sock.close()
            return
        sock.setblocking(False)
        # Without this the relay's own forwarding is subject to Nagle, and the
        # delay lands squarely on the lockstep critical path.
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        c = Client(sock, addr)
        self.clients[sock] = c
        self.sel.register(sock, selectors.EVENT_READ, c)
        log("%s:%d connected", addr[0], addr[1])

    # ------------------------------------------------------------- framing
    def service(self, c):
        while True:
            if len(c.rx) < 1:
                return
            n = c.rx[0]
            # Structural validation at the door. A length of 0 cannot be a
            # frame (it would carry no type) and nothing this protocol defines
            # is anywhere near 200 bytes.
            if n < 1 or n > 200:
                self.stats["bad_frames"] += 1
                log("%s: bad frame length %d, dropping the connection",
                    c.name or "?", n)
                self.drop(c)
                return
            if len(c.rx) < n + 1:
                return                      # wait for the rest of the frame
            body = bytes(c.rx[1:n + 1])
            del c.rx[:n + 1]
            self.handle(c, body[0], body[1:])

    def handle(self, c, ftype, payload):
        if ftype in RELAY_TYPES:
            if c.partner is not None:
                # Padded on the way out, never on the way in: a console may
                # send a short record, but one must never RECEIVE a short one.
                c.partner.send(frame(ftype, payload.ljust(GAME_PAYLOAD, b"\x00")))
            if ftype == T_INPUT and len(payload) > IR_CRC:
                self.log_crc(c, payload)
            return

        if ftype == T_HELLO:
            self.on_hello(c, payload)
        elif ftype == T_LIST:
            self.send_lobby(c)
        elif ftype == T_JOIN:
            self.on_join(c, payload)
        elif ftype == T_PING:
            c.send(frame(T_PONG))
        elif ftype == T_BYE:
            self.drop(c)
        else:
            log("%s: unknown frame type $%02X", c.name or "?", ftype)

    # ----------------------------------------------------------- the lobby
    def on_hello(self, c, payload):
        if len(payload) < 3 or payload[0] != PROTO:
            log("%s: HELLO version %s, wanted %d",
                c.addr[0], payload[:1].hex() or "none", PROTO)
            self.drop(c)
            return
        # THE TELEVISION STANDARD, AND THE ONE THIS SERVER WILL NOT PAIR.
        #
        # A 2600 cannot measure which television it is plugged into -- the ROM
        # generates the video timing and there is nothing to read -- so the
        # console declares what it was BUILT as and build.sh will not build a
        # SECAM image. This is the second lock, for a hand-built one: on a SECAM
        # TIA the eight colours are chosen by the hue nibble and the luminance
        # bits are ignored, so Combat's two tanks, which differ only in colour,
        # can render identically. It is not a sync problem and lockstep does not
        # help it, so the console is refused rather than guarded.
        c.tv = payload[1]
        if c.tv == TV_SECAM:
            log("%s: refusing a SECAM console -- its TIA cannot tell the two "
                "tanks apart by colour", c.addr[0])
            self.drop(c)
            return
        if c.tv not in (TV_NTSC, TV_PAL):
            log("%s: HELLO declares television standard $%02X, which is not "
                "one this server knows", c.addr[0], c.tv)
            self.drop(c)
            return
        name = payload[2:].decode("ascii", "replace").strip("\x00 ").upper()
        if not (2 <= len(name) <= 8) or any(ch not in NAME_OK for ch in name):
            log("%s: bad name %r", c.addr[0], name)
            self.drop(c)
            return
        while name in self.by_name:
            name = (name[:7] + str(random.randrange(10)))
        c.name = name
        self.by_name[name] = c
        log("%s is %s (%s)", c.addr[0], name, TV_NAME[c.tv])
        self.lobby_update()
        self.try_pair(c)

    def idle(self):
        return [c for c in self.clients.values() if c.name and c.partner is None]

    def try_pair(self, c):
        """Auto-pair: the first two idle consoles become a match.

        The ROM does not send JOIN. A 2600 client has twelve columns of
        cartridge-composed text and no keyboard, so an in-ROM lobby list costs a
        whole bank; the Lobby already does room discovery, and what is left for
        this server is to put the two consoles that turned up together.
        """
        others = [o for o in self.idle() if o is not c]
        if not others:
            return
        # Oldest waiting first, so a console cannot be queue-jumped forever.
        other = min(others, key=lambda o: o.born)
        seed = random.randrange(1, 0x10000)
        # The console that was already waiting is the host and drives player 0
        # -- the same convention as the Intellivision family, where the waiting
        # player takes role 0. Player 0 is the LEFT port's paddle A, and the
        # guest's is player 2, the right port's: the game itself says so at
        # $F206, which computes the second live player as the first EOR 2.
        host, guest = other, c
        host.partner, guest.partner = guest, host
        host.role, guest.role = 0, 1
        for me, them in ((host, guest), (guest, host)):
            me.crc.clear()
            me.send(frame(T_START, bytes([
                me.role, seed & 0xFF, seed >> 8, self.args.delay,
                self.args.variation]) + them.name.encode().ljust(8, b"\x00")))
        self.stats["matches"] += 1
        log("match: %s (host) vs %s, seed $%04X, delay %d, variation %d",
            host.name, guest.name, seed, self.args.delay, self.args.variation)
        self.lobby_update()

    def on_join(self, c, payload):
        # Reserved. The ROM auto-pairs today; keeping JOIN in the protocol is
        # what lets an in-ROM lobby list arrive later without a wire change.
        self.try_pair(c)

    def send_lobby(self, c):
        entries = b""
        n = 0
        for o in self.clients.values():
            if not o.name or n >= 8:
                continue
            entries += o.name.encode().ljust(8, b"\x00")
            entries += bytes([1 if o.partner else 0])
            n += 1
        c.send(frame(T_LOBBY, bytes([n]) + entries))

    # ------------------------------------------------------------ the CRCs
    def log_crc(self, c, payload):
        """Pair the two consoles' checksums by tick and report disagreement.

        The checksum rides in byte 3 of every INPUT record, so this costs the
        console nothing extra and happens fifteen times a second rather than
        once every sixty-four ticks.

        The relay does not act on a mismatch -- repair is the ROM's job, and a
        server that tried would be guessing at a simulation it cannot see. What
        it can do is say, in one line, whether the two consoles ever disagreed,
        which is the whole verdict test/run_rig.sh reads.
        """
        # payload is tick(1) window(3*3) crc(1) check(1). The checksum's offset
        # has moved twice now -- once when the record started carrying a WINDOW
        # of three ticks instead of one byte, and again when a tick's input grew
        # from a joystick nibble to a paddle byte -- so it is named, not
        # counted. See the note in vonet.inc --
        # the transport is not locked to the tick, so a record can simply never
        # be sent, and the peer's ring then keeps the previous lap's byte in
        # that slot. This relay stays dumb about all of it and only reads the
        # tick and the checksum.
        tick, crc = payload[IR_TICK], payload[IR_CRC]
        if c.last_tick is not None:
            delta = (tick - c.last_tick) & 0xFF
            if 0 < delta < 128 and tick < c.last_tick:
                c.lap += 1
        c.last_tick = tick
        key = c.lap * 256 + tick

        c.crc[key] = crc
        p = c.partner
        if p is None or key not in p.crc:
            # Evict by AGE, never by value. A dict preserves insertion order,
            # so the oldest is simply first. Eight is about half a second: both
            # consoles send every tick, so an entry that has not paired within a
            # handful of them is one whose partner never sent it.
            while len(c.crc) > 8:
                c.crc.pop(next(iter(c.crc)))
            return
        other = p.crc.pop(key)
        if other != crc:
            self.stats["crc_bad"] += 1
            log("CRC MISMATCH tick %d: %s=$%02X vs $%02X", tick, c.name, crc, other)
        else:
            self.stats["crc_ok"] += 1

    # --------------------------------------------------------------- drops
    def drop(self, c):
        if c.sock not in self.clients:
            return
        if c.name and self.by_name.get(c.name) is c:
            del self.by_name[c.name]
        p = c.partner
        if p is not None:
            p.partner = None
            p.send(frame(T_PEER_LEFT, b"\x00" * GAME_PAYLOAD))
            log("match ended: %s left, %d CRC rounds verified",
                c.name or "?", self.stats["crc_ok"])
        try:
            self.sel.unregister(c.sock)
        except (KeyError, ValueError):
            pass
        c.sock.close()
        del self.clients[c.sock]
        if c.name:
            log("%s disconnected", c.name)
        self.lobby_update()

    def sweep(self, now):
        for c in list(self.clients.values()):
            if c.name is None and now - c.born > HELLO_TIMEOUT:
                self.stats["hello_timeouts"] += 1
                self.drop(c)

    def lobby_update(self):
        if self.lobby:
            self.lobby.update(len(self.by_name))

    # ---------------------------------------------------------------- loop
    def run(self):
        if self.args.lobby_url:
            self.lobby = LobbyPublisher(self.args)
            self.lobby.start()
        last_sweep = last_stats = time.monotonic()
        try:
            while True:
                for key, mask in self.sel.select(timeout=1.0):
                    if key.data is None:
                        self.accept()
                        continue
                    c = key.data
                    if mask & selectors.EVENT_READ:
                        try:
                            data = c.sock.recv(4096)
                        except OSError:
                            data = b""
                        if not data:
                            self.drop(c)
                            continue
                        c.rx += data
                        self.service(c)
                    if c.sock in self.clients and mask & selectors.EVENT_WRITE:
                        self.flush(c)
                for c in list(self.clients.values()):
                    self.flush(c)
                    self.reselect(c)
                now = time.monotonic()
                if now - last_sweep > 5.0:
                    self.sweep(now)
                    last_sweep = now
                if now - last_stats > STATS_INTERVAL:
                    log("stats %s", self.stats)
                    last_stats = now
        except KeyboardInterrupt:
            log("shutting down")
        finally:
            if self.lobby:
                self.lobby.shutdown()

    def flush(self, c):
        if not c.tx:
            return
        try:
            sent = c.sock.send(c.tx)
            del c.tx[:sent]
        except BlockingIOError:
            pass
        except OSError:
            self.drop(c)
            return
        if len(c.tx) > MAX_TX_BACKLOG:
            self.stats["tx_drops"] += 1
            log("%s: %d bytes backed up, dropping", c.name or "?", len(c.tx))
            self.drop(c)

    def reselect(self, c):
        want = selectors.EVENT_READ | (selectors.EVENT_WRITE if c.tx else 0)
        try:
            self.sel.modify(c.sock, want, c)
        except (KeyError, ValueError):
            pass


class LobbyPublisher(threading.Thread):
    """Register this relay as a room on the FujiNet Lobby.

    The Lobby has no expiry job: `status` is whatever the publisher last said,
    and a server that stops POSTing stays in every client's list forever with a
    growing pingage. So the keepalive re-POST is what keeps the room honest, and
    the offline POST on shutdown is what takes it away.

    The platform string must be "a2600" -- that is what the 2600 Lobby client
    queries (`/view?bin=2&platform=a2600`), and a row published under any other
    spelling simply never appears in it.
    """

    KEEPALIVE = 300.0

    def __init__(self, args):
        super().__init__(daemon=True)
        self.url = args.lobby_url
        self.cv = threading.Condition()
        self.stop = threading.Event()
        self.dirty = True
        self.failures = 0
        self.payload = {
            "game": args.game_name,
            "appkey": args.lobby_appkey,
            "server": args.server_name,
            "region": args.region,
            "serverurl": args.lobby_serverurl,
            "status": "online",
            "maxplayers": 2,
            "curplayers": 0,
            "clients": [{"platform": "a2600", "url": args.lobby_client_url}],
        }

    def update(self, curplayers):
        with self.cv:
            self.payload["curplayers"] = curplayers
            self.dirty = True
            self.cv.notify()

    def run(self):
        while not self.stop.is_set():
            with self.cv:
                if not self.dirty:
                    self.cv.wait(self.KEEPALIVE)
                self.dirty = False
                body = json.dumps(self.payload).encode()
            self.post(body)
            self.stop.wait(1.0)     # at most one POST a second

    def post(self, body):
        req = urllib.request.Request(
            self.url, data=body, headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=10) as r:
                r.read()
        except OSError as e:
            self.failures += 1
            log("lobby: POST failed (%s)", e)

    def shutdown(self):
        with self.cv:
            self.payload["status"] = "offline"
            body = json.dumps(self.payload).encode()
        self.stop.set()
        with self.cv:
            self.cv.notify()
        self.join(timeout=12)
        self.post(body)
        log("lobby: published offline")


def main():
    # SIGTERM, not just SIGINT. Python's default SIGTERM handler ends the
    # process without unwinding, so `finally` never runs and the Lobby never
    # hears that this room went away -- and the Lobby has no expiry job, so the
    # room then sits in every client's list for ever. The Intellivision family
    # logs this exact defect against its own relay; there is no reason to
    # inherit it. Raising KeyboardInterrupt reuses the shutdown path that
    # Ctrl-C already exercises, rather than adding a second one.
    def on_term(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, on_term)

    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=9600)
    ap.add_argument("--delay", type=int, default=DEFAULT_DELAY,
                    help="input delay in ticks (measured: 3 frames a tick)")
    ap.add_argument("--variation", type=int, default=DEFAULT_VARIATION,
                    help="variation the match starts on, 0-49 ($96's own "
                         "numbering: manual game N is N-1). Must be one a pair "
                         "of remote players can actually play.")
    ap.add_argument("--lobby-url", default="",
                    help="e.g. https://lobby.fujinet.online/server; off by default")
    ap.add_argument("--lobby-appkey", type=int, default=24)
    ap.add_argument("--lobby-serverurl", default="TCP://fujinet.online:9600/")
    ap.add_argument("--lobby-client-url",
                    default="TNFS://apps.irata.online/Atari2600/Games/VideoOlympics.bin")
    ap.add_argument("--game-name", default="Video Olympics")
    ap.add_argument("--server-name", default="Video Olympics Netplay")
    ap.add_argument("--region", default="us")
    ap.add_argument("--any-variation", action="store_true",
                    help="allow a start variation outside the two-player set")
    args = ap.parse_args()
    if not 0 <= args.variation <= 49:
        sys.exit("vo_relay: variation must be 0-49")
    if args.variation not in TWO_PLAYER and not args.any_variation:
        sys.exit("vo_relay: variation %d (manual game %d) is not a two-player "
                 "game. Two of the fifty are single-player and twenty-six need "
                 "four paddles. Pass --any-variation to override."
                 % (args.variation, args.variation + 1))
    Server(args).run()


if __name__ == "__main__":
    main()
