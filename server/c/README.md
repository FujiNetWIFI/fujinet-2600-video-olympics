# vo-relay — the C relay

A C port of `../vo_relay_server.py`. **The Python stays canonical**: it is
the reference every behaviour here is derived from, and `tools/server_diff.py`
holds this binary to byte-for-byte equality with it on the wire and in the log.
If the two ever disagree, **the Python is right and this is a bug** — except
for the differences listed under "Deliberate divergences".

The port follows the Intellivision family's (`fujinet-intv-soccer/server/c/`),
which is where `lobby.c` comes from. It does **not** follow that relay's shape:
there are no rooms, no seats, no GO frame and no roster here. This protocol
pairs two consoles and forwards fixed-size frames between them, and the C
mirrors that.

## Build

```sh
make -C server/c            # -> server/c/vo-relay
make -C server/c strict     # the same, with -Werror  (gate 0)
make -C server/c asan       # ASan + UBSan build      (gate 0)
```

No external dependencies. `cc`, libc, and `-lpthread`; the binary links
nothing else. The Lobby POST is hand-rolled plain HTTP over a socket, because
the Lobby's Go server binds `:8080` and the relay reaches it directly. There is
no TLS to terminate and therefore no TLS to link.

## Running it

```sh
server/c/vo-relay --host 127.0.0.1 --port 9600 --delay 2 --variation 2
```

Every flag the Python takes, with the same defaults, plus two test hooks.

| flag | default | notes |
|---|---|---|
| `--host ADDR` | `0.0.0.0` | |
| `--port N` | 9600 | |
| `--delay N` | 2 | ticks; byte [3] of START. The ROM checks it against its own `VOD` and falls back to local play on a mismatch, so 2 is the only value this ROM accepts |
| `--variation N` | 2 | 0–49, in `$96`'s own numbering (manual game N is N−1), and must be one of the 22 two-player games unless `--any-variation` |
| `--lobby-url URL` | off | **http:// only** — see below |
| `--lobby-appkey N` | 24 | the Lobby rejects 0 |
| `--lobby-serverurl URL` | `TCP://fujinet.online:9600/` | |
| `--lobby-client-url URL` | the TNFS ROM path | |
| `--game-name S` | `Video Olympics` | |
| `--server-name S` | `Video Olympics Netplay` | |
| `--region S` | `us` | |
| `--any-variation` | off | start on a variation outside the two-player set |
| `--lobby-keepalive SECS` | 300 | **new**, test hook — a subprocess cannot be monkeypatched the way a Python class attribute can |
| `--fixed-seed N` | off | **new**, test only — pin the match seed |

The test scripts select the implementation by environment variable, defaulting
to the Python:

```sh
make relay-c                 # build it
SERVER=c make sim            # and lobby, rig, rig-play, rig-repair, play, session
make server-diff             # the differential
```

## Gates

```sh
make -C server/c strict                    # builds clean at -Werror
make -C server/c asan && make server-diff  # the differential under ASan+UBSan
SERVER=c make sim                          # 22 protocol conformance checks
SERVER=c make lobby                        # 11 Lobby registration checks
SERVER=c make rig                          # two consoles, one match, zero desyncs
```

`tools/server_diff.py` raises on a sanitizer report or an interpreter traceback
before it compares anything, because neither carries a timestamp and the log
filter would otherwise drop it on the floor.

## This is protocol 3, and the Python's docstring says 2

`PROTO = 3` here. Version 2 put the television standard between the version
byte and the name; 3 widened a tick's input from one joystick nibble to three
paddle bytes, which is another payload SHAPE change — an older relay would pair
the wrong checksums. A console built for 2 is refused on the HELLO.

The wire frame is **sixteen** bytes during play, not Combat's eight: a payload
of `tick(1) + 3 ticks x 3 bytes + crc(1) + check(1)` = 12, padded to 14, and 16
on the wire. The console reads `avail AND $F0`. The checksum is therefore at
`IR_CRC = 10`, and `log_crc`'s guard is `len(payload) > IR_CRC` — one byte
laxer than Combat's `>= 6`, because it does not require the check byte. That
off-by-one is transcribed from this game's own Python and deliberately not
unified with the others.

**`vo_relay_server.py`'s module docstring is inherited from Combat and is stale
in four places** — it still says `ver=2`, an 8-byte wire frame,
`(avail >> 3) << 3`, and a joystick-nibble input byte. So is its SECAM refusal
string, which still talks about telling two tanks apart by colour. The comments
in `src/vo-relay.c` are written from what the Python's *code* does; the log
*string* is kept byte-for-byte, stale wording and all, because that one is on
the wire-visible side of the differential.

## Log output is a contract

`test/run_rig.sh` greps the relay log for `match:` and counts `CRC MISMATCH`;
`tools/vo_client_sim.py` reads it too. The format is the Python's, exactly:

```python
print(("%s " % time.strftime("%H:%M:%S")) + msg, flush=True)
```

Local time-of-day, **no date and no level**, on **stdout**, flushed per line —
the harnesses read the log while the server is still running. This is *not* the
Intellivision relay's `logging.basicConfig` format on stderr; `relay_log()`
keeps that function's signature only so `lobby.c` drops in unchanged, and
ignores the level argument.

Two `%`-formatting details are reproduced deliberately:

- **`log("stats %s", self.stats)` is a Python dict repr in INSERTION order**,
  not sorted. (The Intellivision port sorts, because its Python does.)
- **`bad name %r`** is `repr()` of a `str` that has been through
  `decode("ascii", "replace")`, so a byte ≥ 0x80 is one U+FFFD and renders
  literally, as UTF-8. `py_repr()` reproduces that, along with `\xNN` for C0
  controls and Python's quote-selection rule.

And one semantic oddity: `match ended: %s left, %d CRC rounds verified` prints
the **global** `crc_ok`, not a per-match count.

## Three things in this file that look wrong and are not

**1. `service()` keeps parsing after the client has been dropped.** The
Python's frame loop has no liveness check after `handle()`, and `drop()` clears
only the *partner's* link — never `c.partner` — so a dropped console can still
forward its remaining queued frames to its ex-partner. That is observable on
the wire, so it is reproduced rather than fixed. It is safe here because
`drop()` does not free the slot: `reap()` does, at the end of the event-loop
iteration, which is the C equivalent of a Python object outliving its removal
from `self.clients`.

**2. A second HELLO on one connection leaves the old name behind.** `on_hello`
never checks `c.name`, so `by_name` keeps an entry pointing at this client that
`drop()` will not remove — it stays taken and keeps counting towards
`curplayers`. The Python leaks a dict entry; here the entry is never
dereferenced, only matched by name, so there is nothing to dangle.

**3. `order[]` exists because `self.clients` is a dict.** Python dicts are
insertion-ordered and `self.clients` is keyed by socket, so iterating it is
iterating in ACCEPT order. `send_lobby()` slices the first eight named entries
out of that, which means accept order *is* the lobby a console displays, and
`try_pair()`'s `min(..., key=born)` resolves ties by it. A slot array iterates
in slot order, and slots get reused — a different order the moment anyone
disconnects. `tools/server_diff.py --scenario lobby` exists to catch exactly
that, and forces a slot reuse to do it.

## Deliberate divergences

1. **`--fixed-seed` and `--lobby-keepalive` exist only here**, as test hooks.
   Python's Mersenne Twister sequence cannot be reproduced in C, so the
   differential masks the seed rather than pinning it; `--fixed-seed` is for
   anything that needs a stable one.
2. **An `https://` `--lobby-url` is refused at startup**, with a message. The
   Python's help text shows one, and `urllib` would honour it; this build has
   no TLS. `tools/test_lobby_pub.py` already drives the relay over `http://`,
   which is the path production actually uses.
3. **`by_name` is bounded** at `NAME_SLOTS` (256). The Python's dict is not,
   and divergence 2 above means it can grow without limit under a client that
   sends HELLO repeatedly. On overflow the oldest entry is discarded.
4. **A duplicate name is resolved at most 1000 times** before the connection is
   refused. `while name in self.by_name` is unbounded in the Python.
5. **Send loops snapshot the client list before iterating**, so a `flush()`
   that drops a client cannot disturb the walk.

## Memory

Measured on this machine: 32 idle lobby clients plus one live match pumping
INPUT at 15 Hz, and a churn client connecting and disconnecting every second so
slot reuse, `by_name` churn and the CRC map all keep turning over. Seventy
seconds; RSS sampled throughout.

| | RSS idle | high-water under load | growth in the second half |
|---|---|---|---|
| the Python relay | 24824 K | 24888 K | none — flat |
| this binary | **2252 K** | 6352 K | none — flat |

**11x smaller at idle**, and neither implementation grows over the run. The
shared host runs one relay per game, so the fleet cost falls from ~25 MB each
to ~2 MB each plus whatever working set the live matches touch.

`.bss` is ~16 MiB, nearly all of it the 64 transmit buffers, and it is
demand-paged. Two things are load-bearing in getting that to stay unresident,
and both have a comment at the code:

- **`rxbuf[]`/`txbuf[]` live outside `client_t`.** With the buffers inline,
  `sizeof(client_t)` is a quarter of a megabyte, and `main()`'s loop setting
  every `fd` to `-1` then touches one byte every 260 KiB across the whole 16
  MiB. Transparent huge pages are `[always]` by default here, so that faults
  in 2 MiB at a time and makes essentially all of `.bss` resident before a
  single console has connected — measured at 18.6 MB, *worse than the Python*.
- **They are `madvise(MADV_NOHUGEPAGE)`d.** A steady-state client writes only
  into the first page of its 256 KiB transmit buffer; on a huge page that one
  write would make 2 MiB resident and hold it.

`client_alloc()` zeroes the scalars field by field for the same reason — a
`memset(c, 0, sizeof *c)` would touch every buffer page on every accept.

**What this does not buy:** throughput or latency. Two consoles trading 8-byte
frames at 15 Hz is well under 100 frames a second; the Python spends tens of
microseconds of interpreter overhead per frame and C spends well under one, and
both are a rounding error against a single core. Both set `TCP_NODELAY`, and
the relay hop is one RTT either way. The port is worth doing for footprint and
for being one binary per game with no interpreter to install — not for speed.

## What the differential does not cover

`tools/server_diff.py` compares every frame and every log line, but two paths
are inherently not comparable and are excluded rather than faked:

- **The transmit-backlog drop.** `flush()` logs `%d bytes backed up, dropping`
  with the exact number of bytes still queued, which depends on how much the
  kernel's socket buffer accepted on that particular `send()`. No two runs of
  the *same* implementation agree on it. `TX_CAP` is sized so that C never
  silently discards where the Python would have delivered — the derivation is
  at the constant — but the log line itself is untested.
- **The 60-second HELLO timeout**, which would make every run a minute longer.
  `sweep()` is exercised only in that it runs every five seconds without
  dropping anyone it should not.

## The real risk

The Python has **no** `except Exception` around its per-client service call —
unlike the Intellivision relay, which logs a traceback and drops one
connection. A latent bug is already fatal to the process in both. The
difference is that in C it is a crash or memory corruption rather than a clean
exit, so:

- every wire-derived index is bounds-checked at the point of use (frame length,
  name length, CRC offset, lobby entry count);
- allocation is fully static — fixed arrays sized by `MAX_CLIENTS`, no `malloc`
  in the steady state, so there is no allocator state to corrupt;
- gate 0 builds with ASan + UBSan and runs the full differential under it, leak
  detection on.

If you change this file, re-run `tools/server_diff.py` and the ASan build. The
gates are cheap; the failure mode is not.

## Structure

`src/vo-relay.c` keeps the Python's function names *and their order in the
file* (`frame_build`, `relay_log`, `accept_client`, `service`, `handle`,
`on_hello`, `try_pair`, `send_lobby`, `log_crc`, `drop`, `sweep`, `flush`,
`update_events`, `main`). Two implementations only stay in sync if the diff
between them is mechanical — keep it that way.

`src/lobby.{c,h}` is the Intellivision port's publisher thread with three
changes: the platform string is `a2600`, `--lobby-url` is the **complete**
endpoint rather than a base to which `/server` is appended, and the log lines
match this family's wording.
