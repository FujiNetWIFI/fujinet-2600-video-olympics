#!/usr/bin/env python3
"""playdiff.py -- which cell went first, and at which tick.

Two per-tick dumps from emu/play.lua, one per console. Lines up the ticks,
finds the first one at which the authoritative sim state differs, and NAMES THE
CELLS -- because "the two consoles desynced" is what the relay already says and
is not actionable, while "$A4 TankY0 is one lower on the guest from tick 63" is.

    tools/playdiff.py build/rig/c1.out build/rig/c2.out
"""
import re
import sys

# The addresses emu/play.lua dumps, in order. NOT a base plus an offset: the
# dump has a hole in it, because $BB-$BE are the filtered LOCAL paddle
# positions and two consoles with two hands on two paddles differ there on
# every tick of a perfectly synchronised pair.
CELLS = ([a for a in range(0x80, 0xB7) if a not in (0x84, 0x85)]
         + list(range(0xCD, 0xD1)))

# How many ticks at the end of the run must agree for a repair to count as one.
# 120 is eight seconds at fifteen ticks a second -- long enough that a pair that
# merely happened to coincide for a moment cannot pass.
RECOVER_TAIL = 120

# And how long the repair itself may take. Measured, repeatedly, at 4 ticks --
# detection on the tick after the injection, the synthetic RESET captured on the
# next boundary, delivered d ticks later, acted on immediately. 30 is that with
# room, and it is a REAL bound: without it the gate passes a pair that took
# half a minute to come back, which is indistinguishable to a player from not
# coming back at all.
REPAIR_BOUND = 30

# NAMES ARE ONLY GIVEN WHERE THEY WERE DERIVED, and every one here was read out
# of the disassembly rather than guessed. There is no commented source for this
# game -- that is the whole of PORTING.md 1 -- so an invented name would be a
# lie that a future reader would then chase. A blank is honest.
NAMES = {
    0x88: "FrameCtrLo",   # $F07A, now behind the gate
    0x89: "FrameCtrMid",
    0x8A: "FrameCtrHi",
    0x8D: "Score0",       # $F015's BCD loop reads $8D,X for X = 1, 0
    0x8E: "Score1",
    0x8F: "SwchbShadow",  # $F0D9
    0x92: "PlayerA",      # the two live players; $F206 makes $93 = $92 EOR 2
    0x93: "PlayerB",
    0x96: "Variation",    # 0-49; SELECT walks it at $F0F8, wrapping on #$32
    0x97: "VarFlagsLo",   # $F23A decodes the variation into these
    0x98: "VarFlagsHi",
    0x9B: "PlfPtr0Lo",    # the nine-byte dispatch group, $F257
    0x9C: "PlfPtr0Hi",
    0x9D: "PlfPtr1Lo",
    0x9E: "PlfPtr1Hi",
    0x9F: "PlfPtr2Lo",
    0xA0: "PlfPtr2Hi",
    0xA1: "CodePtr0Lo",   # JMP ($00A1), high byte forced to $F3 at $F28A
    0xA2: "CodePtr0Hi",
    0xA3: "CodePtr1Lo",   # JMP ($00A3)
    0xA4: "CodePtr1Hi",
    0xB2: "PosY0",        # the kernel compares its line counter against these
    0xB3: "PosY1",
    0xB4: "PosY2",
    0xB5: "PosY3",
    0xB6: "BallY",
    0xCD: "VOPAD0",       # what both consoles compute from the two wire bytes
    0xCE: "VOPAD1",
    0xCF: "VOPAD2",
    0xD0: "VOPAD3",
}


def load(path):
    """Index by the harness's boundary COUNT, and check the ROM's raw tick.

    The count is authoritative: the tap fires once per completed boundary, so
    counting them is the tick exactly. The raw VOTICK rides along only so a
    disagreement between the two numbering schemes is visible instead of being
    silently absorbed into the diff.
    """
    out, raw = {}, {}
    for line in open(path, errors="replace"):
        m = re.match(r"^S (\d+) (\d+) ([0-9A-F]+)$", line.strip())
        if m:
            n = int(m.group(1))
            out[n] = m.group(3)
            raw[n] = int(m.group(2))
    return out, raw


def addr_of(i):
    """Dump index to zero-page address. Combat's version special-cased one
    trailing cell; here the whole layout is CELLS, hole and all."""
    return CELLS[i] if i < len(CELLS) else None


def main():
    (a, ra), (b, rb) = (load(p) for p in sys.argv[1:3] if not p.startswith("--"))
    if not a or not b:
        print("playdiff: one of the consoles printed no state at all")
        return 1
    common = sorted(set(a) & set(b))
    print("%d ticks from console 1, %d from console 2, %d in common"
          % (len(a), len(b), len(common)))
    # The two consoles number their boundaries from their own first one. If the
    # ROM's raw tick disagrees by a constant, the dumps are simply offset and
    # every "divergence" below is that offset; if it disagrees by a VARYING
    # amount, the two consoles really are running different numbers of ticks.
    off = {((ra[t] - rb[t]) & 0xFF) for t in common}
    if off != {0}:
        print("raw-tick offsets seen between the two dumps: %s"
              % " ".join("%+d" % (o - 256 if o > 127 else o)
                         for o in sorted(off)))
    first = None
    counts = {}
    for t in common:
        x, y = a[t], b[t]
        if x == y:
            continue
        bad = [i for i in range(len(x) // 2)
               if x[2 * i:2 * i + 2] != y[2 * i:2 * i + 2]]
        for i in bad:
            counts[addr_of(i)] = counts.get(addr_of(i), 0) + 1
        if first is None:
            first = t
            print("\nFIRST DIVERGENCE at tick %d" % t)
            for i in bad:
                ad = addr_of(i)
                u, v = int(x[2 * i:2 * i + 2], 16), int(y[2 * i:2 * i + 2], 16)
                print("  $%02X %-10s  c1=$%02X  c2=$%02X  (%+d)"
                      % (ad, NAMES.get(ad, ""), u, v, v - u))
            # The three ticks either side, so the run-up is visible.
            for u in [t2 for t2 in common if t - 3 <= t2 <= t + 3]:
                print("    t%-5d %s" % (u, "SAME" if a[u] == b[u] else "DIFF"))
    # --repair inverts the question. A correct pair NEVER diverges, so recovery
    # cannot be tested by waiting for a bug: the harness breaks one console on
    # purpose (PLAY_INJECT) and the assertion is that they come back together.
    if "--repair" in sys.argv:
        bad = [t for t in common if a[t] != b[t]]
        tail = common[-RECOVER_TAIL:]
        healed = all(a[t] == b[t] for t in tail)
        print()
        if not bad:
            print("NO DIVERGENCE AT ALL -- the injection never landed, so this "
                  "run proves nothing about the repair")
            return 1
        # EPISODES, NOT A SPAN. This used to report bad[-1] - bad[0], which is
        # the distance from the first divergent tick to the last -- and that is
        # not the recovery time, it is the recovery time OR the distance to any
        # unrelated blip later in the run, whichever is larger. One divergent
        # tick at t512 turned a four-tick repair into "recovered after 393
        # ticks", and the number went into a commit message before anybody
        # noticed it was measuring two different things at once.
        #
        # Contiguous runs of divergent ticks are grouped, with a gap of up to
        # GAP ticks tolerated inside one episode, because a repair in progress
        # can agree for a tick and then differ again.
        GAP = 8
        eps = []
        for t in bad:
            if eps and t - eps[-1][1] <= GAP:
                eps[-1][1] = t
            else:
                eps.append([t, t])
        first_len = eps[0][1] - eps[0][0] + 1
        print()
        print("diverged at tick %d, %d divergent ticks in all, in %d episode(s)"
              % (bad[0], len(bad), len(eps)))
        print("THE REPAIR took %d ticks (%.1f seconds at 15 a second)"
              % (first_len, first_len / 15.0))
        for lo, hi in eps[1:]:
            print("  later episode at ticks %d-%d (%d ticks) -- NOT the repair; "
                  "a separate divergence the pair also recovered from"
                  % (lo, hi, hi - lo + 1))
        if healed:
            print("the last %d ticks agree byte for byte" % len(tail))
        else:
            print("THE LAST %d TICKS DO NOT AGREE -- no repair happened"
                  % len(tail))
        if first_len > REPAIR_BOUND:
            print("THE REPAIR TOOK TOO LONG -- %d ticks against a bound of %d"
                  % (first_len, REPAIR_BOUND))
        return 0 if (healed and first_len <= REPAIR_BOUND) else 1

    if first is None:
        print("\nNO DIVERGENCE -- the two consoles agree at every common tick")
        return 0
    print("\nevery cell that ever differed, by how many ticks:")
    for ad in sorted(counts, key=lambda k: -counts[k]):
        print("  $%02X %-10s %d" % (ad, NAMES.get(ad, ""), counts[ad]))
    return 1


if __name__ == "__main__":
    sys.exit(main())
