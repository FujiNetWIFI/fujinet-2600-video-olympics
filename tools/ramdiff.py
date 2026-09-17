#!/usr/bin/env python3
"""ramdiff.py -- compare two per-frame state checksum streams.

`make det` proves two builds agree. It does NOT prove either of them does
anything -- two runs that both sit still are identical by definition, and the
Intellivision family lost a day to exactly that (PORTING.md §7.31: a draft that
dropped the call to the game's primary logic routine passed 256/256). So this
also requires the stream to CHANGE: a run whose checksum never moves is a
failure however well it matches.

Usage: ramdiff.py a.txt b.txt [--min-frames N]
"""

import sys


def load(path):
    out = {}
    for line in open(path):
        parts = line.split()
        if len(parts) == 2 and parts[0].isdigit():
            try:
                out[int(parts[0])] = int(parts[1], 16)
            except ValueError:
                pass
    return out


def main():
    a, b = load(sys.argv[1]), load(sys.argv[2])
    minf = 200
    if "--min-frames" in sys.argv:
        minf = int(sys.argv[sys.argv.index("--min-frames") + 1])

    common = sorted(set(a) & set(b))
    if len(common) < minf:
        print("ramdiff: only %d frames in common, wanted %d" % (len(common), minf),
              file=sys.stderr)
        return 1

    distinct = len({a[f] for f in common})
    if distinct < 2:
        print("ramdiff: the state never changed across %d frames -- the run "
              "proved nothing" % len(common), file=sys.stderr)
        return 1

    for f in common:
        if a[f] != b[f]:
            print("ramdiff: DIVERGED at frame %d: %04X vs %04X" % (f, a[f], b[f]),
                  file=sys.stderr)
            near = [g for g in common if abs(g - f) <= 3]
            for g in near:
                print("   frame %5d  %04X  %04X%s"
                      % (g, a[g], b[g], "  <--" if a[g] != b[g] else ""),
                      file=sys.stderr)
            return 1

    print("ramdiff: %d frames identical, %d distinct states -- PASS"
          % (len(common), distinct))
    return 0


if __name__ == "__main__":
    sys.exit(main())
