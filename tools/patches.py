"""patches.py -- the declared patch map for networked Video Olympics.

The family's discipline, from intv-baseball-experiment: the original source is
never edited, every change to it is DECLARED here, and tools/check_patch.py
fails the build on any difference between the built image and the cartridge
dump that is not on this list. A patch that changes nothing and a change that
was never declared are both build failures.

Each entry is anchored on a LINE NUMBER of rom/video-olympics.asm plus the
exact text that line must contain. That file is GENERATED -- `make disasm` runs
DiStella over the dump with tools/vo.cfg -- so unlike Combat's hand-written
source its line numbers move whenever the code/data map changes. The `old` text
check is what makes that safe: a shifted line is a loud build error rather than
a silent mis-patch. If tools/vo.cfg changes, expect to re-anchor this file, and
let the errors tell you where.

`addr` is where the patch lands in the stock ROM, for check_patch.py's byte
audit. `size` is 0 when the patch is the same length as what it replaces, which
every input-site patch is: `LDA SWCHA` is `ad 80 02` and `LDA VOSWA` is
`ad d1 00` -- three bytes and four cycles either way. Nothing retimes, and
every byte of the cycle-exact kernel keeps the address it has in the dump.
"""

# ---------------------------------------------------------------------------
# Milestone 2: the bank split. Structural only -- the game still reads its own
# console, so a split build must play EXACTLY like stock.
# ---------------------------------------------------------------------------

STRUCTURAL = [
    dict(
        name="START: the bank entry dispatcher, and the RAM clear moved out",
        line=55, nlines=10, addr=0xF000, size=0,
        old="""START:
\tSEI
\tCLD
\tLDX    #$FF
\tTXS
\tINX
\tTXA
LF007: STA    $8D,X
\tINX
\tBNE    LF007   """,
        new="""; $1000 is where a bank switch lands, and this bank is entered twice a
; frame: once cold at power-on and once every frame on the way back from
; the kernel bank. Two entry BITS, not one entry value -- PORTING.md 4.12:
; a single value meant the boot bank could hand over "playing" and the game
; bank would read "not cold" and skip START entirely, coming up with every
; variable zero.
;
; The stock RAM clear cannot stay here. It runs X from 0 to 255 storing
; through `STA $8D,X`, which wraps inside page zero and therefore covers
; ALL of $00-$FF -- the whole 128 bytes of RAM and a strobe of every TIA
; register. That is deliberate 1977 economy and it is also 4.12's sibling
; trap: it would wipe every netcode cell the boot bank has just set up. It
; moves to VOSTART, which skips the netcode's range and does the rest.
;
; EXACTLY TWELVE BYTES, because that is what it replaces. Every region is
; emitted at its own ORG and then runs on, so a patch one byte short moves
; every byte after it and check_patch reports the entire rest of the bank.
;
; SEI is gone from here and lives in VOSTART, in the boot bank. Note that
; losing it would not have broken the BRK calls even if it had stayed: the I
; flag masks IRQ, not BRK.
;
; THERE IS NO WARM/COLD TEST HERE ANY MORE. The RAM clear moved to the boot
; bank -- the game bank had no room and the boot bank is 9 bytes used of 2048 --
; and the boot bank is the only other way into this one. So every arrival is a
; warm one and the dispatcher is a single jump. The nine bytes after it are
; dead and declared.
START:
\tJMP  LF00C              ; 3
\tDB $EA,$EA,$EA,$EA,$EA,$EA,$EA,$EA,$EA  ; 9, never executed""",
    ),
    dict(
        name="the sound countdown comes out of the ungated part of the frame",
        line=121, nlines=1, addr=0xF06D, size=0,
        old="LF06D: DEC    $8C    ",
        new="""; $8C is the sound duration, and it is decremented HERE -- in the audio block,
; a hundred bytes before the lockstep gate -- so in the stock frame it counts
; down whether or not the sim advances. Exactly the frame counter's problem one
; cell along, and it showed up the same way: `make rig-play` found two consoles
; that agreed on every checksum and differed by one in $8C on thirty ticks out
; of five hundred.
;
; Nothing but the sound reads it, so the audible consequence is a beep a frame
; longer on one console than the other -- which nobody would ever notice and
; which is still a quantity that is a function of the local stall pattern. The
; doctrine does not have a clause for "small".
;
; TWO BYTES, AND EXACTLY FIVE CYCLES. `DEC zp` is five; two NOPs are four; and
; that ONE CYCLE is a bug that took a long time to find.
;
; The paddle capacitor's dump transistor is asserted at $F31A, inside LF23A,
; which this sound block runs BEFORE -- and it is released at the top of the
; kernel, which is WSYNC-aligned. So the dump WINDOW is
; (aligned release) - (unaligned assert), and anything that moves the assert
; moves the window. Two NOPs moved it one cycle earlier on every frame the
; sound was running, MAME's pot model discharges for that one cycle longer, and
; once in a while the extra cycle is enough to flip the comparator a scanline
; early. `make det` then diverged from frame 1560 in $BB, $BC and the position
; they drive -- 1.5% of frames, five hundred frames past where any ten-second
; run stopped looking.
;
; So the cycles are spent rather than saved. VOTMP is the netcode's own
; scratch, dead between routines and dead here, and `DEC` on it costs precisely
; what the instruction it replaces cost.
;
; The general rule, and it is the sharpest one this port taught: ON A MACHINE
; WITH AN ANALOG INPUT TIMED BY THE PROGRAM ITSELF, A PATCH IS NOT
; SIZE-PRESERVING UNLESS IT IS ALSO CYCLE-PRESERVING. Every other patch in this
; file replaces an instruction with one of the same length AND the same cost;
; this is the only one that had to be thought about, because it is the only one
; that removes work rather than redirecting it.
;
; The label stays: $F069's BVS branches to it.
LF06D\tDEC  VOTMP             ; 2 bytes, 5 cycles -- exactly what DEC $8C cost""",
    ),
    dict(
        name="the frame counter comes out of the ungated part of the frame",
        line=127, nlines=4, addr=0xF07A, size=0,
        old="""\tINC    $88
\tBNE    LF082
\tINC    $89
\tINC    $8A""",
        new="""; THE SIM CLOCK MUST NOT ADVANCE ON A STALLED FRAME.
;
; $88/$89/$8A is this game's frame counter and it is not decoration: $F0E3
; masks it with #$1F to debounce SELECT once every thirty-two frames, and
; $F215 masks it with #$1C to pace the serve. It is simulation.
;
; It increments HERE, at $F07A, which is ninety bytes before the lockstep gate
; at $F0D0 -- so in the stock frame it advances whether or not the sim does.
; Two consoles stall at different moments by design, because absorbing that is
; what lockstep is FOR, so a counter that counted frames DRAWN rather than
; ticks RUN diverges by exactly the difference in their stall patterns. The
; symptom was 143 CRC mismatches on a pair that was otherwise in perfect step,
; and consoles 337 and 340 ticks apart.
;
; Moving it behind the gate makes it count simulated frames, which both
; consoles agree about by construction. Nothing between here and the gate
; reads it -- $F082-$F093 touches $97, $94, $B9 and $B8, and the paddle-trigger
; loop at $F0A1 touches $92 and $93 -- so the move is invisible to everything
; except the thing it fixes. This is 4.20's rule in its Video Olympics form.
;
; Eight bytes of filler, and the eight bytes of work move to VOFCNT, which
; lives in the dead space the $F21F patch leaves behind.
\tDB $EA,$EA,$EA,$EA,$EA,$EA,$EA,$EA""",
    ),
    dict(
        name="the head of the timed band becomes the lockstep gate",
        line=170, nlines=1, addr=0xF0D0, size=0,
        old="\tSTA    TIM64T  ",
        new="""\tJMP  VOGATE             ; was STA TIM64T -- VOGATE arms the timer
\t                        ;   itself, FIRST, then decides whether the sim
\t                        ;   advances and either falls into the game logic
\t                        ;   at $10D3 or jumps over it to the spin at $121F.
\t                        ;   Three bytes for three; the band is the same
\t                        ;   length either way, which is what make frames
\t                        ;   checks.""",
    ),
    dict(
        name="the vertical blank's spin becomes the switch into the kernel bank",
        line=343, nlines=11, addr=0xF21F, size=0,
        old="""LF21F: LDA    INTIM
\tBNE    LF21F
\tSTA    CXCLR
\tSTA    WSYNC
\tSTA    HMOVE
\tSTA    VBLANK
\tLDY    #$07
\tJSR    LF5B8
\tJSR    LF585
\tJSR    LF5D0
\tJMP    LF00C   """,
        new="""; THE SWITCH MOVES UP, TO THE HEAD OF THE SPIN.
;
; It was at $F22C, after the spin and immediately before the three kernel
; calls, which is the smallest change that works -- and it puts the bank
; boundary in the wrong place. The vertical blank's spin is THE SLACK IN THIS
; FRAME: `LDA INTIM / BNE` is the game waiting for a timer it armed at $F0D0,
; and it is where the transport's micro-steps have to run. Leaving it in the
; game bank would have meant reaching the network machine, which lives in the
; kernel bank because that is where the room is, through a bank switch -- and a
; bank switch is a jump that never returns.
;
; So the kernel bank takes the whole tail of the frame: the spin, the four
; strobes, the three kernel routines and the checksum. It re-runs them at its
; own entry; see vokern.asm. Twenty-two of these twenty-seven bytes become
; dead, and being dead in the game bank is exactly what they should be -- the
; game bank has no room to spare and this is the one place it gains any.
;
; The stall path still works unchanged: VOGATE jumps to $121F when the sim does
; not advance, which is now the switch itself, so a stalled frame goes straight
; here with the game logic skipped.
; THE LABEL STAYS. Three branches in the game logic land here -- $F20D, $F211
; and $F217 all say "give up on this frame and go and wait out the blank" --
; and they are now branches to the bank switch, which is exactly right: giving
; up on the frame and handing over to the kernel bank are the same act.
LF21F\tLDA  #BANKKERN          ; 2
\tJMP  VOGOTO             ; 3
;
; The twenty-two bytes that were the strobes and the three kernel calls are
; unreachable now, and they are the only spare space this bank has ever had.
; VOFCNT gets nine of them: the frame counter the patch above took out of the
; ungated part of the frame, called from VOGATE when the sim advances.
; THE GUARD IS THE ONE STOCK USES, not the obvious one. $F04E is `BIT $8C`
; with A = $3F and $F050 is `BEQ`, so the decrement is skipped when
; ($8C AND $3F) is zero -- NOT when $8C is zero. A plain zero test decrements
; on the values $40, $80 and $C0 where stock does not, and `make det` diverged
; at frame 117 by one.
VOFCNT\tLDA  $8C                ; 2  the sound countdown
\tAND  #$3F               ; 2  ...on stock's own condition
\tBEQ  VOFCN0             ; 2
\tDEC  $8C                ; 2
VOFCN0\tINC  $88                ; 2  the frame counter
\tBNE  VOFCN1             ; 2
\tINC  $89                ; 2
\tINC  $8A                ; 2
VOFCN1\tRTS                     ; 1
\tDB $EA,$EA,$EA,$EA,$EA  ; 5 left""",
    ),
]

# ---------------------------------------------------------------------------
# Milestone 3: the input sites.
#
# SEVEN sites, and every one is size-preserving. The five SWCHB reads and the
# SWCHA read are ABSOLUTE ($0280/$0282, three bytes), and the shadows they are
# redirected to are in ZERO PAGE -- so the assembler has to be TOLD to keep the
# long form. `>` is AS's force-long prefix; without it `LDA VOSWB` assembles to
# `A5 D2`, two bytes, and every byte after the site moves. The symptom is not a
# helpful error: it is check_patch reporting a thousand undeclared differences
# because a JSR six bytes further on now names an address six bytes short.
#
# (AS has the `<`/`>` prefixes for ADDRESSING MODE but not as lo/hi-byte
# OPERATORS -- for a low byte the family spells it `#(X)&$FF`. Two different
# things wearing the same character.)
#
# PORTING.md 4.1: the LOCAL console's input is patched too. In delay-based
# lockstep both consoles apply both inputs at the same tick; a console that
# read its own paddle live at delay 0 while the peer applied it at delay d
# would be running a different simulation from the same tick number.
#
# $F1B4 is the ONE place the game reads a paddle position. The kernel keeps
# writing $BB-$BE every frame from the local paddle and that is left alone on
# purpose: patching the kernel's store would have to be conditional on
# networked mode, and `make det` would stop being a byte-for-byte claim.
# ---------------------------------------------------------------------------

INPUTS = [
    dict(name="the four paddle triggers", line=146, addr=0xF0A1, size=0,
         old="LF0A1: LDA    SWCHA   ",
         new="LF0A1:\tLDA  >VOSWA             ; was SWCHA"),

    dict(name="THE paddle position read -- players 0-3 at $BB-$BE",
         line=288, addr=0xF1B4, size=0,
         old="LF1B4: LDA    $BB,X   ",
         new="LF1B4:\tLDA  VOPAD,X            ; was $BB,X"),

    dict(name="console switches (1 of 5)", line=171, addr=0xF0D3, size=0,
         old="\tLDA    SWCHB  ",
         new="\tLDA  >VOSWB             ; was SWCHB"),

    dict(name="console switches (2 of 5)", line=182, addr=0xF0E9, size=0,
         old="LF0E9: ORA    SWCHB   ",
         new="LF0E9:\tORA  >VOSWB             ; was SWCHB"),

    dict(name="console switches (3 of 5)", line=381, addr=0xF26A, size=0,
         old="\tBIT    SWCHB  ",
         new="\tBIT  >VOSWB             ; was SWCHB"),

    dict(name="console switches (4 of 5)", line=439, addr=0xF2D7, size=0,
         old="\tBIT    SWCHB  ",
         new="\tBIT  >VOSWB             ; was SWCHB"),

    dict(name="console switches (5 of 5)", line=767, addr=0xF528, size=0,
         old="\tLDA    SWCHB  ",
         new="\tLDA  >VOSWB             ; was SWCHB"),
]

# ---------------------------------------------------------------------------
# The audit allowances for tools/check_patch.py.
#
# REWRITTEN: ranges compared not at all, because the patch replaced them
# wholesale. Keep this as small as it can be -- every byte in here is a byte
# the audit is not watching.
#
# SPANS: single sites, each (addr, length, why). A declared span that turns out
# to be identical is a build failure too: a patch that changed nothing means
# the anchor moved and the `old` check did not catch it.
# ---------------------------------------------------------------------------

REWRITTEN = [
    (0xF000, 0xF00C, "START becomes the bank entry dispatcher; the RAM clear "
                     "moves to VOSTART, which skips the netcode's cells"),
]

SPANS = [
    (0xF06D, 2, "the sound countdown moves behind the lockstep gate, and the "
                "replacement costs the same FIVE cycles -- see the note"),
    (0xF07A, 8, "the frame counter moves behind the lockstep gate"),
    (0xF0D0, 3, "STA TIM64T -> JMP VOGATE, the lockstep gate"),
    (0xF21F, 27, "the vblank spin, the strobes, the three kernel JSRs and the "
                 "JMP back all become one bank switch; the kernel bank re-runs "
                 "them so the transport can live where the slack is"),
    (0xF0A1, 3, "LDA SWCHA -> LDA VOSWA"),
    (0xF1B4, 2, "LDA $BB,X -> LDA VOPAD,X, the only paddle position read"),
    (0xF0D3, 3, "LDA SWCHB -> LDA VOSWB"),
    (0xF0E9, 3, "ORA SWCHB -> ORA VOSWB"),
    (0xF26A, 3, "BIT SWCHB -> BIT VOSWB"),
    (0xF2D7, 3, "BIT SWCHB -> BIT VOSWB"),
    (0xF528, 3, "LDA SWCHB -> LDA VOSWB"),
]


# The gate jumps to two addresses DiStella never labelled, because nothing
# branches to them. They are spelled out in vodefs.inc as VOLOGIC and VOSPIN,
# and these are the opcodes that must be there -- so a typo in either equate is
# a build error rather than a jump into the middle of an instruction.
LANDINGS = [
    (0xF0D3, 0xAD, "VOLOGIC: LDA SWCHB, the first byte of the game logic"),
    (0xF21F, 0xAD, "VOSPIN: LDA INTIM, the spin that ends the timed band"),
]


def check_landings(rom):
    """rom is the stock 2048-byte dump."""
    for addr, want, why in LANDINGS:
        got = rom[addr - 0xF000]
        if got != want:
            raise SystemExit(
                "patches: %s -- expected $%02X at $%04X, found $%02X. "
                "The gate would jump into the middle of an instruction."
                % (why, want, addr, got))


def _norm(text):
    """Collapse whitespace. The anchor's job is to catch a MOVED LINE, not to
    police column alignment: rom/video-olympics.asm is generated by DiStella,
    which pads mnemonics to a fixed width with spaces, and hand-transcribing
    that padding into this file would make every entry a formatting puzzle with
    no safety gained. Token sequence is what identifies the line."""
    return " ".join(text.split())


def _check(out, p):
    """Verify the anchor before touching anything."""
    i = p["line"] - 1
    n = p.get("nlines", 1)
    if i < 0 or i + n > len(out):
        raise SystemExit(
            "patches: %s -- line %d is past the end of the source (%d lines). "
            "tools/vo.cfg probably changed; re-anchor."
            % (p["name"], p["line"], len(out)))
    want = _norm(p["old"])
    have = _norm("".join(out[i:i + n]))
    if have != want:
        raise SystemExit(
            "patches: %s -- anchor mismatch at line %d\n  want: %s\n  have: %s"
            % (p["name"], p["line"], want, have))


def apply(lines):
    """Apply every declared patch.

    THE LINE COUNT IS PRESERVED, deliberately. tools/mkbanks.py maps every
    source line to the address the assembler put it at, using the listing of
    the UNPATCHED source, and then walks the patched lines by the same index.
    A patch that collapsed fifteen lines into one would shift every line after
    it and silently file half the game into the wrong bank.

    So the whole replacement -- however many lines of it there are -- goes into
    the FIRST slot, and the rest are blanked. The generator writes slots out
    with writelines(), so the assembler still sees ordinary lines.

    Note this is about SOURCE lines, not bytes. Keeping the byte count is a
    separate obligation and it is on the patch author: every region is emitted
    at its own ORG and then simply runs on, so a replacement one byte short of
    what it replaces moves every byte after it, and check_patch reports the
    entire rest of the bank rather than the one thing that is wrong.
    """
    out = list(lines)
    for p in STRUCTURAL + INPUTS:
        _check(out, p)
        i = p["line"] - 1
        n = p.get("nlines", 1)
        out[i] = p["new"] + "\n"
        out[i + 1:i + n] = ["\n"] * (n - 1)
    return out
