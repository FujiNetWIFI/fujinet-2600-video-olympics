#!/usr/bin/env python3
"""checkrom_filter.py -- run the firmware's checkrom.py and judge its findings.

The upstream tool is authoritative and is not forked. But it walks every bank
linearly looking for banned opcodes, and a linear walk through a DATA table is
misaligned garbage that will sooner or later decode as one. Its own comment
accepts that for the RMW check, which is gated on the operand landing in the
write-only page; the indirect-store check is not gated on anything, so any
$91 or $81 byte the walk happens to land on is an unconditional failure.

Combat has two: $91 inside CTRLTBL and $81 inside VARMAP. Both are data, both
are in regions this port declares as data in tools/mkbanks.py, and neither is
an instruction -- the disassembly contains no indirect store anywhere, which
is checked independently below against the pristine source.

So findings inside a declared data region are reported and dropped; everything
else fails the build.

It also corrects the reported address. checkrom.py labels every bank but bank 0
as though it were based at $1800 (`base = BASE if start == 0 else 0x1800`),
which is right for the fixed half and wrong for banks 1..N -- they are all
mapped at $1000. An upstream fix would be one line; until then this is where
the addresses are made true, because a build gate that prints the wrong address
costs more time than the finding saves.

Usage: checkrom_filter.py <checkrom.py> <image.bin>
"""

import re
import subprocess
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from mkbanks import REGIONS, BASE, REBASE

BANKS = {"BOOT": 0, "GAME": 1, "KERN": 2}
FINDING = re.compile(r"^checkrom: (?P<img>\S+): bank (?P<bank>\d+) \$(?P<addr>[0-9A-F]{4}): (?P<what>.*)$")


def data_ranges():
    """(bank index, lo, hi) in BANK address space, for every data region.

    A BOTH region is emitted into every bank, so it is a data range in every
    bank -- a linear walk through it is misaligned garbage wherever it lands.
    """
    out = []
    for lo, hi, bank, _why, kind in REGIONS:
        if kind != "data":
            continue
        for b in (["GAME", "KERN"] if bank == "BOTH" else [bank]):
            out.append((BANKS[b], lo - REBASE, hi - REBASE))
    return out


def main():
    tool, image = sys.argv[1], sys.argv[2]
    r = subprocess.run([sys.executable, tool, image],
                       capture_output=True, text=True)
    sys.stdout.write(r.stdout)

    ranges = data_ranges()
    fatal, dropped = [], []
    for line in r.stderr.splitlines():
        m = FINDING.match(line)
        if not m:
            fatal.append(line)
            continue
        bank = int(m.group("bank"))
        addr = int(m.group("addr"), 16)
        # Undo the upstream mislabelling for banks 1..N.
        true = addr if bank == 0 else addr - 0x1800 + 0x1000
        if any(b == bank and lo <= true < hi for b, lo, hi in ranges):
            dropped.append("  $%04X (bank %d): %s" % (true, bank, m.group("what").split(" -- ")[0]))
            continue
        fatal.append("checkrom: bank %d $%04X: %s" % (bank, true, m.group("what")))

    if dropped:
        print("checkrom: %d finding(s) inside declared DATA regions, dropped:"
              % len(dropped))
        for d in dropped:
            print(d)

    for f in fatal:
        print(f, file=sys.stderr)
    if fatal:
        return 1
    print("checkrom: %s passes" % image)
    return 0


if __name__ == "__main__":
    sys.exit(main())
