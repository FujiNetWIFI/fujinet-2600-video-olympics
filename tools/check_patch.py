#!/usr/bin/env python3
"""check_patch.py -- audit the built image against the cartridge dump.

The family's rule, from intv-baseball-experiment/tools/check_patch.py: a change
that was not declared is a build failure, and so is a declaration that changed
nothing. This is the gate that makes "Combat, moved into banks" a claim rather
than a hope.

It works because of a deliberate layout choice: every byte of Combat keeps the
OFFSET it has in the dump, and each bank simply leaves the other's regions
empty. So the audit is a byte compare of each region against the same offsets
of the baseline, and the only bytes allowed to differ are the ones
tools/patches.py declares.

The baseline is NOT rom/combat.bin. It is the same pristine source assembled at
$1000, where a cartridge bank is actually mapped -- because every absolute
address inside Combat has a high byte, and $F4 really does become $14. Comparing
against the $F000 image would report a hundred and more "undeclared changes"
that are the rebase and nothing else, and a gate that cries wolf that loudly is
not a gate. Both images come from the same never-edited disassembly through the
same converter, and `make verify-org` proves that path reproduces the cartridge
dump byte for byte, so the baseline cannot drift.

Usage: check_patch.py baseline.bin built.bin
"""

import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from mkbanks import REGIONS, BASE, REBASE
import patches as patchmap

BANKS = {"BOOT": 0, "GAME": 1, "KERN": 2}
BANKSZ = 0x800


def main():
    stock = open(sys.argv[1], "rb").read()
    built = open(sys.argv[2], "rb").read()
    if len(stock) != 0x800:
        sys.exit("check_patch: %s is %d bytes, expected 2048" % (sys.argv[1], len(stock)))

    allowed = set()
    for lo, hi, _why in patchmap.REWRITTEN:
        allowed.update(range(lo, hi))
    for addr, span, _why in patchmap.SPANS:
        allowed.update(range(addr, addr + span))

    problems, changed, seen = [], set(), 0
    for lo, hi, bank, why, _kind in REGIONS:
        # A BOTH region is duplicated into every bank, so audit every copy: a
        # patch that landed in one bank and not the other would otherwise pass.
        banks = ["GAME", "KERN"] if bank == "BOTH" else [bank]
        for bk in banks:
            base = BANKS[bk] * BANKSZ
            for a in range(lo, hi):
                off = a - BASE
                seen += 1
                if stock[off] == built[base + off]:
                    continue
                changed.add(a)
                if a not in allowed:
                    problems.append(
                        "UNDECLARED DIFF at $%04X (%s bank, %s): $%02X -> $%02X"
                        % (a, bk.lower(), why, stock[off], built[base + off]))

    # A declaration that changed nothing is as much a bug as an undeclared
    # change: it means the patch did not land where it was aimed.
    for addr, span, why in patchmap.SPANS:
        if not any(a in changed for a in range(addr, addr + span)):
            problems.append("DECLARED PATCH at $%04X changed nothing: %s" % (addr, why))
    for lo, hi, why in patchmap.REWRITTEN:
        if not any(a in changed for a in range(lo, hi)):
            problems.append("DECLARED REWRITE $%04X-$%04X changed nothing: %s"
                            % (lo, hi - 1, why))

    for p in problems:
        print("check_patch: " + p, file=sys.stderr)
    if problems:
        return 1
    print("check_patch: %d bytes compared, %d changed, all declared"
          % (seen, len(changed)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
