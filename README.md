# Networked Video Olympics on the Atari 2600

The 1977 cartridge, patched at seven read sites and re-laid across three banks,
playing across two consoles in delay-based input lockstep over a TCP socket.

Video Olympics is a **paddle** game — fifty variations of Pong, by Joe Decuir,
CX2621 — and that is most of what makes this port different from its
predecessor, [`fujinet-2600-combat`](../fujinet-2600-combat). Combat's input was
a four-bit joystick nibble; this one is an eight-bit analog paddle position,
sampled inside the display kernel by counting scanlines until the controller's
capacitor charges. `PORTING.md` is the full account.

## Status

| Gate | Result |
|---|---|
| `make verify-org` | **PASS** — the generated disassembly rebuilds to the cartridge dump, byte for byte |
| `make echo` | **PASS** — 489 rounds, 0 errors, one frame per transaction, **20 Hz** |
| `make vo` | **PASS** — 8192 bytes; `check_patch` reports 65 bytes changed and **all declared** |
| `make frames` | **PASS** — every frame 262 lines, through two bank switches a frame; stock measures 262 too |
| `make det` | **PASS** — **2177 frames identical to the 1977 ROM**, 2136 distinct states |
| `make inputs` | **PASS** — every `SWCHA`/`SWCHB` read comes from the shim; the game's own six sites are gone |
| `make sim` | **PASS** — 24 protocol conformance checks, no emulator |
| `make lobby` | **PASS** — the Lobby registration contract, against a mock |
| `make session` | **PASS** — socket opened, HELLO delivered, the relay names the player |
| `make rig` | **PASS** — two consoles, **341 and 343 ticks, zero CRC mismatches**, byte-identical zero page at the snapshot tick |
| `make rig-hold` | **PASS** — SELECT held 45 s: the variation walks identically on both consoles |
| `make rig-play` | **PASS** — two consoles with **hands on the paddles**, byte-identical sim state at every common tick |
| `make rig-repair` | **PASS** — a score corrupted at tick 120: detected, repaired, and **back in agreement after 4 ticks**, measured over a dozen runs, with the following 120 byte-identical |

## How it plays

Each console runs its own copy. The FujiNet Lobby lists the room; picking it
writes the relay's URL to an appkey and boots the ROM, which opens
`N:TCP://host:9600/`, says HELLO, and waits. The relay pairs the first two
consoles and tells each its role.

The **host** drives player 0 — the left port's paddle A — and the **guest**
player 2, the right port's. That is the game's own convention, not a choice:
`$F206` computes the second live player as the first `EOR 2`.

RESET and SELECT are **ANDed on the wire**, so either player may press them, and
both consoles compute the same result from the same two bytes without any
protocol for it. SELECT walks only the variations two remote humans can
actually play: of the fifty, two are single-player and twenty-six need four
paddles.

Black-and-white stays **local** — it reaches only the colour registers, and no
colour register reaches physics — so each player sees their own set in whichever
they chose. Each player's own difficulty switch crosses the wire, because that
one does reach play.

## How it works

Delay-based input lockstep. The tick is `K = 4` video frames (15 Hz) and input
is captured at tick `T`, sent tagged for `T + d` with `d = 2`, and applied on
both consoles at `T + d` — about 133 ms, symmetric, both knobs build-time
constants in `src/vodefs.inc`. `make echo` measured the ceiling at 20 Hz before
any of it was written; PORTING.md §2 is why that came first.

A **stall** is a frame that runs normally with the game logic skipped. The 2600
regenerates its whole picture from state every frame, so a frozen sim draws a
frozen picture and nothing needs repairing — and the collision latches refill
identically, so a stall of any length is invisible to the next frame's reads.

**Desync** is detected on the console, not in the relay: every record carries
the sender's checksum, sampled at its own tick boundary, and when that boundary
is the one we are on the two are samples of the same instant and must be equal.
The repair is a **synthetic RESET pressed into this console's own wire byte**,
so it reaches both ends by the road every real press takes and lands on the same
tick at both. The scores are wiped, and that is correct: the two consoles have
just disagreed, so the only state agreed by construction is the state the game
builds from nothing.

A cartridge with **no server** is still a Video Olympics cartridge. Any session
failure — no FujiNet, no relay, a refused connection — hands over not networked,
which is the unpatched game in every respect that matters.

## Notes

Every gate passes. The one that took longest to get there was `make det`, which
failed past frame 1560 until the cause turned out to be **a single cycle**: the
patch moving the sound countdown behind the lockstep gate replaced `DEC $8C`
with two NOPs — the same two bytes, but four cycles instead of five.

The paddle's dump transistor is asserted inside `LF23A`, which that sound block
runs before, and released two instructions after a `WSYNC`. So the dump window
is *(aligned release) − (unaligned assert)*, and one cycle earlier on the
assert is one cycle longer on the ground — occasionally enough to flip the
comparator a scanline early and change the reading.

**On a machine whose analog input is timed by the program itself, a patch is
not size-preserving unless it is also cycle-preserving.** PORTING.md §3.20 has
the full route, including the five theories that were wrong first.

## Building

```
make disasm        # DiStella over the dump, steered by tools/vo.cfg
make verify-org    # reassemble it and require the cartridge back, byte for byte
make vo            # the 8192-byte client
make ladder        # every gate, in order
```

`TVSTD=ntsc` (the default) or `pal`. **SECAM is refused at build time**: a SECAM
TIA renders eight colours by hue and ignores luminance, and this game's object
colour table at `$F698` is hue 0 for its first four entries — they differ in
luminance alone, so on a SECAM set they collapse to one colour. A 2600 cannot
detect its own television, so the only moment anyone knows is build time.

Needs Macroassembler AS (`asl`/`p2bin`), DiStella, a MAME with the FujiNet
cartridge applied, and a `fujinet-pc` for anything that touches the network.

## The cartridge is not in this repository

Supply your own dump at `rom/Video-Olympics.bin` — 2048 bytes, md5
`60e0ea3cbe0913d39803477945e9e5ec`. `rom/video-olympics.asm` is **generated**
from it by `make disasm` and is not checked in either.

There was no published commented disassembly of Video Olympics to start from —
not in the `milnak/atari-vcs-disassembly` collection, not anywhere. This project
produced one, and `make verify-org` is what makes it trustworthy.
