# Porting Video Olympics to networked play on the FujiNet 2600 cartridge

The successor to `fujinet-2600-combat/PORTING.md`, which is the doctrine this
port inherits. Read that one first: everything it says about the sim clock, the
tick boundary, the immutability of a sent record and the shape of the gates
applies here unchanged. This document records only what is **different**, plus
what this cartridge taught that Combat's could not.

Section numbers in the form §4.x refer to Combat's document.

## 1. The two differences that shape the whole port

**It is a paddle game.** Combat's input was a 4-bit joystick nibble, active
low, and the mixer was masks and shifts. Video Olympics' input is an 8-bit
analog paddle position, sampled inside the display kernel by counting scanlines
until the controller's capacitor charges. The wire byte, the mixer, the capture
and the record layout all widen.

**There was no disassembly.** Combat started from the 2002
Dodgson/Bensema/Williams commented DASM source, held pristine and re-verified on
every build. Nothing equivalent exists for Video Olympics — not in the
`milnak/atari-vcs-disassembly` collection, not anywhere else. This port had to
earn one, and `tools/vo.cfg` plus `make verify-org` is the result. That makes
the anti-drift gate load-bearing in a way it was not there: it is the only thing
standing between a wrong code/data split and a build that looks fine until it
runs.

## 2. Know your numbers before you write any code

Same rule, same reason, and `make echo` is still the first thing that runs. The
two numbers that govern the port are the transaction latency `L` and the slack
in the blank bands, and **both are measured, not assumed**.

What is different here is that there are **two** candidate hook points, not
one. Combat had no overscan and only `VOUT`'s vertical-blank spin to live in.
Video Olympics has two timed bands per frame, each ending in a five-byte spin:

| Band | Timer | Spin | What runs in it |
|---|---|---|---|
| 1 | `TIM8T` = `$A2` → 1296 cycles, armed at `$F00C` | `$F094: LDA INTIM / BNE $F094` | score BCD prep, sound, `JSR $F23A`, the frame counters |
| 2 | `TIM64T` = `$20` → 2048 cycles, armed at `$F0D0` | `$F21F: LDA INTIM / BNE $F21F` | the game logic |

### The measured answer

`make echo`, 25 s against a real `fujinet-pc` and a real TCP echo server,
**489 complete rounds, zero errors**:

```
FRAMES 1471  ROUNDS 489  ERR $00
OPEN    n=1    mean=1.00 min=1 max=1
WRITE   n=490  mean=1.00 min=1 max=1
STATUS  n=490  mean=1.00 min=1 max=1
READ    n=489  mean=1.00 min=1 max=1
TICK 3.0 frames for WRITE+STATUS+READ = 20.0 Hz
```

The same figure Combat measured, to the decimal: **one frame per transaction,
three frames per lockstep cycle, 20 Hz.** So `K = 3` (20 Hz, `d = 2` → 100 ms)
is the ceiling and `K = 4` (15 Hz, `d = 2` → 133 ms) is the ceiling with one
frame of margin. This port ships `K = 4` to start, for Combat's reason — the
mask is then free — and because a paddle game that stalls is worse than a
paddle game that lags. Both are build knobs in `vodefs.inc` and the intention
is to bring `d` down once `make rig-play` is green.

One frame is the *floor*, not a measurement of the bus: in MAME the cartridge
answers inside the commit. What the figure does include is the whole FujiNet
software path — BoIP, `fujinet-pc`, the N: device, a real socket — so what is
missing is only the physical cartridge bus, which does not exist yet to
measure.

Both spins are exactly the five bytes Combat's `CBWAIT` had to fit into, so
either or both can carry the hook. The hook needs no cycle budget of its own —
§3.2's structure, a loop that re-reads `INTIM` before every bounded micro-step
and stops when the slack is gone, cannot overrun by construction — so a band
with little slack simply contributes fewer transport steps. `emu/slack.lua`
measures them; run it against the **stock** ROM before deciding.

## 3. What this cartridge taught

### 3.1 `BRK` is a subroutine call, and the IRQ vector is load-bearing

The single most surprising thing in the ROM, and the one most likely to destroy
a banked build silently.

Three sites — `$F262`, `$F2C8`, `$F453` — execute `BRK` followed by a `NOP`:

```
$F25F  LDA  $8A
$F262  BRK              ; $00
$F263  NOP              ; $EA -- the padding byte BRK skips
$F264  STA  $80
```

`BRK` pushes `PC+2`, so the byte after it is skipped and `RTI` returns to
`PC+2`. It is a **two-byte subroutine call**, one byte cheaper than `JSR`. The
handler is the IRQ vector's target, `$F438`:

```
$F438  ASL / ADC #$00   ; x4 -- a true 4-bit left rotate of A,
$F444  RTI              ;       i.e. swap the nibbles
```

So `BRK` means *swap the nibbles of A*, in two bytes. It is a 1977 Atari trick
and Joe Decuir got a byte a call out of it three times.

**Why it matters here.** In a flat 2K cartridge the IRQ vector at `$FFFE` is
the game's own. In the FujiNet banked layout the vectors live in the **fixed
tail**, not in any bank — so `votail.asm` must point `$1FFE` at the handler,
and the handler must be in whichever bank is mapped when a `BRK` executes.
Combat routed IRQ to its cold-start stub because Combat never issues a `BRK`;
doing that here would send the first nibble swap into a cold boot, and the
symptom would be a console that reboots partway through a frame for reasons
nothing in the netcode explains.

All three call sites and the handler are in the game-logic half, so they share
a bank and the constraint is satisfiable:

```
        ORG     $1FFC
        DW      VOCOLD          ; RESET
        DW      $1438           ; IRQ -- the nibble-swap handler in BANKGAME
```

The general rule to carry forward: **before banking a ROM, find out what its
IRQ and NMI vectors are for.** A vector that points at real code is a fourth
entry point, and the fixed tail owns it.

### 3.2 The per-variation routines are reached through a table of pointers

This is why DiStella's automatic pass leaves `$F337-$F3FF` as `.byte` and why
the code/data map had to be derived by hand rather than iterated into.

`$F23A` decodes the variation:

```
LDA $96 / AND #$3E / TAX        ; $96 is the variation, 0-49
LDA LF713,X / STA $98
LDA LF712,X / STA $97
AND #$07 / ... / TAY            ; Y = v*9 + 8, where v = $97 AND 7
LDX #$08
LF257: LDA LF659,Y / STA $9B,X / DEY / DEX / BPL LF257
```

— a nine-byte group copied into `$9B-$A3`, selected by `v`, which is the low
three bits of a per-variation byte. Only **seven** groups are live (`v` = 0..6,
so the table is exactly `$F659-$F697`), and the fifty variations map onto them:

| v | variations |
|---|---|
| 0 | 0-11 | 
| 1 | 12-21 |
| 2 | 22-31 |
| 3 | 32-33 |
| 4 | 34-37 |
| 5 | 38-41 |
| 6 | 42-49 |

Of the nine bytes, `$9C`, `$9E` and `$A2` are immediately overwritten — `$F28A`
sets `$A2` and `$A4` to `$F3`, `$F28E` sets `$9C`, `$9E`, `$C6`, `$C8`, `$CA`
and `$CC` to `$F7`, and `$F2A9` sets `$A0`. What survives is four pointers into
page `$F7` (playfield and score graphics, dereferenced by the kernel through
`LDA ($9B),Y` and friends) and **two code pointers into page `$F3`**, reached by
`JMP ($00A1)` and `JMP ($00A3)`:

```
v=0  $F386 $F3F5     v=4  $F34D $F3E6
v=1  $F386 $F3E0     v=5  $F337 $F3E6
v=2  $F374 $F3C8     v=6  $F390 $F3BA
v=3  $F37C $F3E0
```

`tools/vo.cfg` declares `CODE F337 F3FF` for exactly this reason. The general
form, and DiStella's own documentation says so: **an absolute-indirect `JMP`
ends a trace, and everything it would have reached looks like data.** Find the
table that feeds the pointer before believing any automatic code/data split.

### 3.3 The paddle positions are in RAM, and that is the luckiest fact here

Combat's §4.26 is about the repair being a restart because **tank X is not in
RAM at all** — it lives in the TIA's `HMP0`/`HMP1`, applied incrementally by
`HMOVE`. Video Olympics is better off. The paddle is sampled inside the sprite
kernel, one sample per iteration, the value being the kernel's own line counter
at the moment the capacitor charges:

```
$F62A  LDY $80
$F62C  LDA INPT0,Y      ; $0038,Y -- the $38 MIRROR, not $08
$F62F  BMI +
$F631  STX $84          ; raw paddle A = the line counter
$F633  LDA INPT2,Y
$F636  BMI +
$F638  STX $85          ; raw paddle B
```

and after the kernel a two-tap low-pass filter lands the filtered values in RAM:

```
$F643  LDA $85 / CLC / ADC $BD,Y / ROR A / STA $BD,Y
$F64D  LDA $84 / CLC / ADC $BB,Y / ROR A / STA $BB,Y
```

**`$BB-$BE` are the four filtered paddle positions**, and `$F1B4 LDA $BB,X` is
the one and only place the game reads one. Applying a remote paddle is
therefore four bytes of store, where Combat had nothing it could store at all.

Two traps come with that:

- **The mirror.** `recon.py`'s port table covers `INPT0-INPT3` at `$08-$0B` and
  this ROM reads them at `$38-$3B`. A scan that does not know TIA read registers
  mirror through `$00-$7F` reports *no paddle reads at all*, which reads exactly
  like a game that does not use paddles.
- **The kernel keeps writing those cells.** Every frame, from the local paddle.
  That is why the patch is on the **consumer** at `$F1B4` and not on the
  kernel's store: patching the store would have to be conditional on networked
  mode, and `make det` would stop being a byte-for-byte claim.

### 3.4 The RAM clear covers everything, which removes one Combat problem and keeps its sibling

```
$F000  SEI / CLD / LDX #$FF / TXS / INX / TXA
$F007  STA $8D,X / INX / BNE $F007
```

`STA $8D,X` wraps inside page zero, so the loop covers **all** of `$00-$FF` —
the whole 128 bytes of RAM and a strobe of every TIA register, the same
deliberate use of the wrap Combat documents in §4.11.

- §4.3 does **not** apply. Nothing is left uninitialised, so there is no
  equivalent of Combat's `HIRES` power-on garbage feeding the collision latches
  on frame one.
- §4.12's sibling trap applies in full. The clear wipes every netcode cell the
  boot bank has just set up. It has to be bounded below them, which costs a
  `CPX #n` — two bytes more than the stock `INX / BNE`, and free, because
  `START` is inside the wholesale-rewritten region the patch map declares.

### 3.5 The input surface is seven sites and every one is size-preserving

| Site | Stock bytes | Stock | Patched |
|---|---|---|---|
| `$F0A1` | `ad 80 02` | `LDA SWCHA` | `LDA VOSWA` |
| `$F1B4` | `b5 bb` | `LDA $BB,X` | `LDA VOPAD,X` |
| `$F0D3` | `ad 82 02` | `LDA SWCHB` | `LDA VOSWB` |
| `$F0E9` | `0d 82 02` | `ORA SWCHB` | `ORA VOSWB` |
| `$F26A` | `2c 82 02` | `BIT SWCHB` | `BIT VOSWB` |
| `$F2D7` | `2c 82 02` | `BIT SWCHB` | `BIT VOSWB` |
| `$F528` | `ad 82 02` | `LDA SWCHB` | `LDA VOSWB` |

Every `SWCHB` site is absolute, so redirecting it to a zero-page shadow through
*absolute* addressing keeps all three bytes — nothing retimes, and every byte of
the cycle-exact kernel keeps its dump address, which is what keeps
`check_patch.py` a real audit rather than a formality.

`$F0A1` is followed by `AND $F6B4,X` against the mask table `80 40 08 04`, so
`SWCHA` bits 7 and 6 are the left port's two paddle triggers and bits 3 and 2
the right port's. The synthetic `VOSWA` carries the host's triggers in 7/6 and
the guest's in 3/2, which lands them on players 0/1 and 2/3 — the same mapping
`VOPAD` uses, for free.

The role mapping is confirmed by the game itself: `$F206` computes
`$93 = $92 EOR #$02`, so the two active players in a two-player variation always
differ in bit 1 — indices **0 and 2**, the left port's paddle A and the right
port's. `$92`/`$93` are *not* the role mapping and the shim must not key off
them: they say which player is currently in play and the game logic writes them
at `$F122`, `$F3AD` and `$F4A8`.

### 3.6 SECAM is refused, and here the reason is measurable

Combat refused SECAM because its two tanks are the same shape and are told apart
by colour alone. Video Olympics' case is the same argument with the numbers in
front of it. The object colour table at `$F698` reads

```
0C 00 0E 06   20 20 30 20
```

and the first four entries are **hue 0 throughout** — they differ in luminance
only (6, 0, 7, 3). A SECAM TIA renders eight colours chosen by the hue nibble
and ignores luminance entirely, so those four collapse to one colour and two
players end up looking at indistinguishable objects. Refused in `build.sh`, for
§4.27's reason: a 2600 cannot detect its own television, so the only moment
anyone knows is build time.

### 3.7 The variation whitelist

Of the fifty variations, two are single-player (Robot Pong, and Pong against the
computer) and are meaningless over a network. Twenty-two are two-player and
twenty-six are four-player:

| Players | Variations |
|---|---|
| 1 | 1, 2 |
| 2 | 3, 4, 9, 10, 13, 14, 19, 20, 23-28, 35, 36, 39, 40, 43-46 |
| 4 | 5-8, 11, 12, 15-18, 21, 22, 29-34, 37, 38, 41, 42, 47-50 |

SELECT walks only the whitelist. It is a fifty-bit bitmap, seven bytes of ROM,
and it starts as the twenty-two two-player games, widening to forty-eight when
the second paddle slot is wired up.

`$96` holds the variation and the SELECT handler at `$F0F8` wraps it on
`CMP #$32`. **`$96` is the first cell the checksum must cover**, for §4.21's
reason: two consoles on different variations draw visibly different games while
agreeing perfectly on a checksum that only covers what is moving.

### 3.8 Determinism

`INTIM` is read only in the two spin loops. There is no LFSR and nothing is
seeded from the timer, so Video Olympics is as deterministic as Combat was —
the same piece of luck, and it is what makes lockstep possible at all.

The one free-running quantity is the frame counter `$88`/`$89`/`$8A`, which
feeds serve behaviour. It must sit inside the gated sim chain so it advances
once per *tick that ran* and not once per frame, which is §4.20's rule in its
Video Olympics form.

### 3.9 Two converter fixes DiStella needed that hand-written DASM did not

`tools/dasm2as.py` came over from Combat and needed two changes, both
mechanical, both caught immediately by `verify-org`:

- **DiStella writes a colon after a label** and Combat's hand-written source did
  not. The converter's label regex swallowed the name and left `:` as the
  mnemonic, so every labelled `.byte` line reached AS as `.byte` rather than
  `DB`.
- **`-f` emits DASM's forced-addressing suffixes** — `LDA.wy $009B,Y`,
  `JMP.ind ($00A1)`. AS has no such syntax and does not need one: all eight
  occurrences name the only mode their opcode has (there is no LDA/ADC/STA
  zero-page,Y, and `JMP` indirect is always absolute), so dropping the suffix
  cannot change a byte. That is safe to *assert* rather than hope because
  `verify-org` would fail the `cmp` if AS ever picked a narrower mode.

### 3.10 MAME must be TOLD this is a paddle game

`run.sh` passes `-joyport1 pad -joyport2 pad`. Combat never had to: MAME
defaults both controller slots to `joy`, which was right for it and is wrong
here. With a digital joystick in the slot the game still runs, and every gate
that only compares two builds against each other still passes — because both
sides are equally wrong. Nothing is measuring what it claims to.

The paddle fields, and they confirm the role mapping the netcode uses:

```
:joyport1:pad:POTX  "Paddle"    paddle 0   left port    -> player 0
:joyport1:pad:POTY  "Paddle 2"  paddle 1   left port    -> player 1
:joyport2:pad:POTX  "Paddle 3"  paddle 2   right port   -> player 2
:joyport2:pad:POTY  "Paddle 4"  paddle 3   right port   -> player 3
```

### 3.11 A shadow that starts at zero is a phantom press

The mirror that fills `VOSWA`/`VOSWB`/`VOPAD` on the local path runs at the end
of a frame, in the kernel bank, immediately before the switch back — which is
the instant before the frame loop's head, and therefore the freshest possible
sample. On the *first* frame there has not been one yet.

`VOSWB` reading zero is not a neutral value. SWCHB is **active low**, so zero is
every console switch held down — §4.8's rule, met in a new place. The game
notices at once: `$F0D3` reads it into `$8F` and `$F0D7` does
`EOR $8F / AND #$02` to find switches that *changed*, so a shadow that is zero
on frame one and `$3F` on frame two is a phantom press. It takes the new-game
path, resets the frame counter at `$F0DF`, and the two builds are on different
simulations from frame two onward. `make det` diverged at its first frame.

`VOSTART` primes the shadows before it enters the frame loop. `VOPAD` is primed
by copying `$BB-$BE`, which the clear has just zeroed, rather than from a
constant — so it gets exactly what stock's first frame would read there.

### 3.12 Three silent traps in the inherited toolchain

All three were carried over from Combat unchanged, and all three were correct
there:

- **`mkbanks.py` dropped "line 201"**, which in Combat's hand-written
  disassembly was the `ORG` that opens the image. In this *generated* one it is
  an `INX` in the SELECT handler. One byte short, every byte after it displaced,
  and a thousand undeclared differences pointing everywhere except at the cause.
  The opening `ORG` is now found by matching the directive. **A hardcoded line
  number that means something in one file means something else in the next.**
- **AS needs `>` to force long addressing.** The shadows are in zero page and
  the sites they replace are absolute, so `LDA VOSWB` quietly assembles two
  bytes where three are required. Same symptom, same difficulty reading it.
  (AS has `<`/`>` as addressing-mode prefixes but not as lo/hi-byte operators —
  two different things wearing the same character.)
- **A region is filed by source LINE.** One 16-byte `.byte` run at `$F743`
  straddles the `$F744` boundary between the variation table and the kernel's
  graphics, so it went whole into bank 1 and bank 2 got 188 bytes of filler.
  It is a `BOTH` region now, and `mkbanks` fails the build if any other
  boundary ever lands mid-line.

`LF5B8` needed `BOTH` for a different reason: the kernel entry calls it *and*
the game bank tail-jumps to it from `LF23A` at `$F31E`, returning through its
`RTS` to `LF23A`'s own caller. A bank switch cannot stand in for that.

### 3.13 Work of a varying length must run inside a timed band

The shim went in the kernel bank first: called from that bank's entry, after
the three kernel routines, which is the freshest possible paddle capture and
costs no patch site at all. `make frames` refused it. Stock measures 262 lines;
that build measured 263, every frame, because the shim ran in **unbudgeted
time** between the last line the kernel drew and the `STA WSYNC` at the top of
the frame loop. Thirty cycles of local-play mirror was enough.

Constant is not the danger. **Variable is.** In a match the boundary work runs
on one frame in `K` and is a hundred and fifty cycles longer, so the frame
would have been two lines longer every fourth frame and the picture would roll.
The only safe home for work whose length varies is inside a timed band, where
the spin at the far end absorbs whatever it took.

So the whole shim moved into the game bank, behind `STA TIM64T` at `$F0D0` —
three bytes, the head of the 2048-cycle band the game logic already runs in,
and `$F21F`'s spin pays for it. That is the single input-side structural patch.

The checksum is the one piece that would not fit (the game bank's three holes
come to 360 bytes and the shim wanted 379), so it runs in the kernel bank and
costs the frame one line: **the networked build is a constant 263 where stock
is a constant 262.** Paying for it by asking `LF5B8` for six blank lines
instead of seven does give 262 back — and `make det` then fails, because the
kernel samples the paddle by counting scanlines until the capacitor charges, so
moving the picture up a line moves the sample and changes the reading. The
frame length was right and the game was playing differently.

Moving the checksum to the kernel bank is safe for a reason worth stating: the
kernel bank runs at the *end* of frame N and the boundary is at the *start* of
frame N+1, and nothing between them writes a cell it covers — the display
kernel writes `$84`/`$85`, `$BB-$BE` and `$81`, and none of those is in it.

### 3.14 Anything inserted into a 1977 frame loop inherits its register conventions

`VOGATE` replaces an instruction nine bytes before `STX $88` at `$F0DF`, and
the X it stores is the zero left by `INX` at `$F0C9` on the way out of the
paddle-trigger loop. The shim counts to four in X — the four paddles — so
without saving it the frame counter is set to `$FF` instead of `0` on the first
frame a console switch changes, and the two builds are on different simulations
from there on. `make det` diverged at frame 2 and named `$88`.

§4.15 is the same lesson from the other side: there, a shared display kernel ate
the *caller's* loop counter. Here the shim ate the game's. **The conventions of
a 1977 frame loop are not written down anywhere**, so preserve everything.

### 3.15 What the checksum must NOT cover

§4.21 says the checksum must cover every cell that *selects* behaviour. The
converse matters just as much here: `$BB-$BE` are the filtered **local** paddle
positions, written by the display kernel every frame from this console's own
controller. They are input, not simulation, and they differ between two
consoles by design — that is the whole point of there being two players.
Checksumming them reports a mismatch on every tick of a perfectly synchronised
pair. What is agreed is `VOPAD`, which both consoles compute from the same two
wire bytes, and it is covered by the positions it drives.

The same applies to `VOADV` and the shadows: a cell the netcode fills is not
state the netcode should check.

### 3.16 Everything ungated is a function of the local stall pattern

§4.20 says the sim clock must not be a cell the game can write. This port found
the same rule from the other end, twice, and both times the gate that caught it
was the one nobody had run yet.

**The frame counter.** `$88`/`$89`/`$8A` increments at `$F07A`, ninety bytes
*before* the lockstep gate at `$F0D0`, so in the stock frame it advances whether
or not the sim does. It is not decoration: `$F0E3` masks it with `#$1F` to
debounce SELECT and `$F215` masks it with `#$1C` to pace the serve. Two consoles
stall at different moments *by design* — absorbing that is what lockstep is FOR
— so a counter of frames DRAWN rather than ticks RUN diverges by exactly the
difference in their stall patterns. **143 CRC mismatches on a pair that was
otherwise in perfect step**, and 337 ticks against 340.

**The sound countdown.** `$8C` is decremented at `$F06D`, in the audio block, a
hundred bytes before the gate. Thirty ticks out of five hundred differed by one.
Nothing but the sound reads it and nobody would ever have heard it — and it is
still a quantity that is a function of the local stall pattern. *The doctrine
has no clause for "small".*

Both moved into `VOFCNT`, called from `VOGATE` only when the sim advances, and
living in the twenty-two dead bytes the `$F21F` patch leaves behind — the only
spare space the game bank has ever had.

Moving the second one needed **stock's own guard, not the obvious one**:
`$F04E` is `BIT $8C` with `A = $3F`, so the decrement is skipped when
`($8C AND $3F)` is zero, *not* when `$8C` is zero. A plain zero test decrements
on `$40`, `$80` and `$C0` where stock does not, and `make det` diverged at frame
117 by one. **Copy the condition, not the intent.**

### 3.17 A cell that survives one clear and not another

The role bit arrives in the START frame and is wanted at the handover, a few
hundred cycles later. It was parked in `VOTMP` — which is `$86`, which the RAM
clear the handover itself runs sweeps on the way past.

Both consoles then came up as the host, neither applied the role swap, and each
put its OWN paddle in player 0's slot: a pair that agreed about every other byte
and disagreed about which end of the court each player was standing at. Only
`make rig-play` could see it, because it is the only gate where the two consoles
send *different* paddle values.

It goes straight into `VOENT` now, which is inside the `$CD-$F9` block `VOSCLR`
skips — and that block exists for exactly this reason.

The same shape twice more, both in the clear itself: `VOSCLR` opened with
`ldx #$FF / txs`, copied from the stock `START` it replaces, which destroys its
own return address; and then it still cleared `$FA-$FF`, which **is the stack**,
because the 6507 puts the stack at `$0180-$01FF` and that mirrors onto
`$80-$FF`. Both times the console ran off into page zero and sat at `$0032` for
ever, and `make frames` reported one bank switch in six hundred frames.

### 3.18 Choosing what to corrupt is choosing what the test proves

`make rig-repair` deliberately breaks one console mid-game and asserts the pair
comes back. The first choice of cell was a player's Y position — and `$B2` is
recomputed from the paddle every frame, so the corruption was gone by the next
tick without anything having repaired it. The harness reported *"recovered after
1 tick"* and the relay had seen nothing at all: **a test that passes itself**.

A score persists, is in the checksum, and nothing rewrites it. Injecting one
into `$8D` gives 8 CRC mismatches, a repair, and agreement again after **8
ticks — half a second** — with the following 120 ticks byte-identical.

### 3.19 The repair was fine; the ruler was not

`make rig-repair` reported recovery in 8 ticks on one run and 232 on another,
and 393 on a third. Combat's equivalent was consistently 4 to 8, so the spread
looked like the one-shot synthetic RESET sometimes failing to land.

It was the measurement. `playdiff --repair` printed `bad[-1] - bad[0]` — the
distance from the FIRST divergent tick to the LAST — which is the recovery time
*or* the distance to any unrelated divergent tick later in a five-hundred-tick
run, whichever is larger, and nothing in the output said which. One blip at
t512 turns a four-tick repair into "recovered after 393 ticks".

Instrumenting the ROM cleared it directly. Across every run, slow ones
included:

```
RSY ARMED t121 raw121        the mismatch noticed, one tick after the injection
RSY PRESSED t121 raw122      the synthetic RESET made into this console's capture
RESET-ON-WIRE t123 raw124    it arrives in the mixed switch byte, d ticks later
```

Detection, arming, the one-shot press and the delay-ring delivery all behave
exactly as designed, on every run. With the report grouping divergent ticks into
EPISODES instead of spanning them, twelve consecutive runs all say the same
thing: **diverged at tick 120, 4 divergent ticks, one episode, the repair took
4 ticks.**

Two things came out of it. The report now names later episodes separately
instead of silently folding them into the headline number. And the gate now
BOUNDS the repair at 30 ticks rather than only asking whether it eventually
happened — without that, it passes a pair that took half a minute to come back,
which to a player is indistinguishable from not coming back at all.

§4.22's lesson wearing new clothes: *a diagnostic nobody has checked is not
evidence.* The bad number reached a commit message before anybody noticed it
was measuring two different things at once.

### 3.20 A patch is not size-preserving unless it is also CYCLE-preserving

The sharpest lesson in this port, and the one that took longest to reach.

Every `make det` run was ten seconds until the ladder ran one at thirty, and
the thirty-second one failed at frame 1624 — five hundred frames past where any
earlier run had stopped looking. **A ten-second gate is not a short
thirty-second gate. It is a different gate.**

The cells were `$BB` and `$BC`, two filtered paddle positions, and `$B3`, the
player position they drive. Thirty-two frames out of 2187.

**The cause was one cycle.** The patch that moves the sound countdown behind
the lockstep gate replaces `DEC $8C` at `$F06D` with two bytes of filler. Two
NOPs are four cycles; `DEC zp` is five.

That matters because of where the paddle's dump transistor is switched:

```
$F31A  STX VBLANK   ; bit 7 SET -- ground the capacitor. Inside LF23A, which
                    ;   the sound block above runs BEFORE. NOT WSYNC-aligned.
$F22A  STA VBLANK   ; bit 7 clear -- release it. Two instructions after a
                    ;   WSYNC, so this edge IS aligned to a scanline.
```

The dump **window** is therefore `(aligned release) − (unaligned assert)`, and
anything that moves the assert moves the window. One cycle earlier on every
frame the sound was running; MAME's pot model discharges for one cycle longer;
and once in a while that cycle is enough to flip the comparator a scanline
early. The reading steps `$62 → $64` at a different frame in the two builds,
and the position follows.

`DEC VOTMP` costs exactly what `DEC $8C` cost, on exactly the frames the
original was reached. With it, `make det` runs **2177 frames byte-identical**
where it used to fail at 1624.

> Every other patch in `tools/patches.py` replaces an instruction with one of
> the same length **and the same cost**. This was the only one that removes work
> rather than redirecting it, and it was the only one that had to be thought
> about.

**How it was found, because the route matters more than the answer.** Five
theories were killed by measurement before the right question got asked:

- *Run-to-run noise* — stock against stock, two MAME runs of the same binary:
  2200 frames, zero divergence. The emulator is reproducible.
- *The netcode* — a build with `VOCRC` removed and the stock spin inlined
  diverges identically. Same 32 frames, same three cells.
- *The spin's polling phase* — 0, 1, 2 and 3 NOPs of padding: no change at all.
- *Absolute elapsed time* — the boot wait set to 30, 90 and 150 frames, moving
  the game's start by two seconds: the first divergence stayed at frame 1624.
- *The pot position* — holding all four paddles at a fixed value changes
  neither the reading nor the beat.

What cracked it was measuring the **capacitor's timing directly** rather than
the code's: the charge window (release → first `INPT0` read) is bit-identical
in both builds on every frame, and the **dump** window is not — it differs by
exactly 1.00 cycle on 24 of 111 frames, starting at the frame the readings
first part company.

**Three tooling lessons, all paid for:**

- **Two tools numbering frames differently cost an afternoon.** `rawdump.lua`
  counted every `CXCLR`; `det.lua` skipped the ones where the frame counter
  reads zero. "First divergence at F1632" and "diverged at frame 1624" were
  different frames, and reconciling a contradiction that was never in the ROM
  took longer than the bug. They share the skip now.
- **Reading a cell at the wrong moment is worse than not reading it.** The
  first attempt sampled `$84` at `CXCLR`, where it reads zero, and concluded
  the raw captures agreed. They do there, and it means nothing: the value the
  filter uses is written later in the frame. That wrong conclusion sent the
  hunt after the arithmetic when the input was moving.
- **`manager.machine.screens:at(1)` silently killed a tap**, which then printed
  nothing and looked like "the event never happened" — §4.22 exactly, in a
  fresh disguise.

## 4. Status

| Gate | Result |
|---|---|
| `make verify-org` | **PASS** — the generated disassembly rebuilds to the cartridge dump, byte for byte, 2048 bytes, md5 `60e0ea3cbe0913d39803477945e9e5ec` |
| `make vo` | **PASS** — 8192 bytes; `checkbanks`, `checkrom` and `mktail` clean; `check_patch` reports 2088 bytes compared, **39 changed and all declared** |
| `make frames` | **PASS** — every frame the same length, through **two bank switches a frame**. Stock is a constant 262; the networked build is a constant **263**, and §3.13 is why that is the right trade rather than a defect |
| `make det` | **PASS** — **2177 frames identical to the 1977 ROM**, 2136 distinct states, with the whole lockstep gate in the build |
| `make sim` | **PASS** — 24 protocol conformance checks against a fresh relay, no emulator |
| `make lobby` | **PASS** — the registration contract against a mock Lobby on an ephemeral port |
| `make echo` | **PASS** — 489 rounds, 0 errors, **1 frame per transaction, 3 frames per WRITE/STATUS/READ cycle = 20 Hz** |
| `make inputs` | **PASS** — 6 sites: `SWCHA`/`SWCHB` from `VOSHIM` in each bank, and `INPT0`/`INPT2` still in the display kernel, which is where the local paddle capture belongs. **The game's own six console-port sites are gone.** |
| `make session` | **PASS** — socket opened, HELLO delivered, the relay reports the player by name out of the FujiNet username appkey |
| `make rig` | **PASS** — two consoles, **341 and 343 ticks, zero CRC mismatches**, byte-identical zero page at tick 150, with SELECT and RESET pressed on ONE console and obeyed by both |
| `make rig-hold` | **PASS** — SELECT held for 45 s: **639 and 641 ticks**, and the variation walks `1 2 3 4 5 6 7 8 9 10` at ticks 33, 41, 49, 57, 65, 73, 81, 89, 97, 105 on **both** consoles, identically |
| `make rig-play` | **PASS** — two consoles with their **hands on the paddles**, on deliberately different periods: **528 ticks, 0 CRC mismatches, and byte-identical sim state at every common tick** |
| `make rig-repair` | **PASS** — one console's score deliberately nudged at tick 120: the relay saw it, both consoles saw it, and they were **back in agreement 4 ticks later**, consistently, with the following 120 byte-identical |

Bank budget after the split: **GAME 1688 bytes used, 360 spare; KERN 400 used,
1648 spare** — more room than Combat had on either side.
