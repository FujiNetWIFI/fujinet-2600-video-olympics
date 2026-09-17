#!/usr/bin/env python3
"""dasm2as.py -- convert a DASM disassembly to Macroassembler AS syntax.

The whole FujiNet 2600 client family is built with Macroassembler AS (asl +
p2bin) and every build gate -- checkbanks.py, mktail.py -- parses AS listings.
DiStella emits DASM. Rather than fork the toolchain, convert the source; the
conversion is proved correct by `make verify-org`, which assembles the result
and requires it to be byte-identical to the cartridge dump.

The dialect differences are few, and all of them are mechanical:

  LABEL:              ->  LABEL       (DiStella writes the colon, DASM by
                                       hand does not; AS takes either)
  LDA.wy $009B,Y      ->  LDA $009B,Y (DASM's forced-addressing suffix)
  processor 6502      ->  CPU 6502
  include vcs.h       ->  INCLUDE "vcs.inc"
  .byte / .BYTE/ byte ->  DB          (three spellings in the original)
  .word               ->  DW
  LABEL = value       ->  LABEL EQU value
  #<expr / #>expr     ->  #(expr)&$FF / #(expr)>>8

That last one is the only one with a trap in it. AS has neither DASM's `<`/`>`
prefix operators nor lo()/hi(); the family spells a low byte `#(X)&$FF`. And
DASM tolerates a stray `#` inside a .byte list -- Combat's SPRLO/SPRHI tables
are written `.BYTE #<TankShape, ...` -- where an immediate marker means
nothing. In DB context the `#` has to go, or AS reads it as a symbol.

Usage: dasm2as.py [--window] in.asm > out.asm

  --window  rebase the ORGs from $F000 to $1000, where a cartridge bank
            is actually mapped.
"""

import re
import sys

# The mnemonic field is the second token on a line with a label, the first
# without. Only these are directives; everything else AS already understands.
DIRECTIVE = {".byte": "DB", ".BYTE": "DB", "byte": "DB", ".word": "DW"}

# #<EXPR and #>EXPR. EXPR is either a parenthesised expression -- PLFPNT is
# written `.BYTE #<(PF0_0-4)` -- or a bare symbol with an optional offset.
LOHI = re.compile(
    r"#(?P<op>[<>])(?P<expr>\([^)]*\)|[A-Za-z_][A-Za-z0-9_]*(?:\s*[-+]\s*\d+)?)")

# DASM lets a mnemonic carry a forced addressing mode -- LDA.wy, JMP.ind -- and
# DiStella's -f emits them. AS has no such syntax and does not need one: every
# suffix in this ROM names the only mode its opcode HAS (there is no LDA/ADC/STA
# zero-page,Y, and JMP indirect is always absolute), so dropping the suffix
# cannot change a single byte. `make verify-org` is what makes that safe to
# assert rather than hope -- if AS ever picked a narrower mode, the cmp fails.
FORCED = re.compile(r"^([A-Za-z]{3})\.(?:ind|dix|diy|[wz][xy]?)$", re.IGNORECASE)


def split_comment(line):
    """Return (code, comment). Quotes do not appear in this source."""
    i = line.find(";")
    return (line, "") if i < 0 else (line[:i], line[i:])


def convert_lohi(code, in_data):
    def sub(m):
        expr, op = m.group("expr"), m.group("op")
        if not expr.startswith("("):
            expr = f"({expr})"
        hash_ = "" if in_data else "#"
        return f"{hash_}{expr}&$FF" if op == "<" else f"{hash_}{expr}>>8"
    return LOHI.sub(sub, code)


def convert(line):
    code, comment = split_comment(line)
    if not code.strip():
        return line

    # Leading label, if any. DASM and AS both take a bare label in column 0;
    # DiStella writes a colon after it and AS is happy either way.
    m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*):?(\s*)(.*)$", code)
    label, gap, rest = (m.group(1), m.group(2), m.group(3)) if m else ("", "", code)
    if not m:
        rest = code.lstrip()
        gap = code[: len(code) - len(rest)]

    parts = rest.split(None, 1)
    head = parts[0] if parts else ""
    tail = parts[1] if len(parts) > 1 else ""

    # LABEL = value  ->  LABEL EQU value
    if head == "=" or rest.lstrip().startswith("="):
        value = rest.lstrip()[1:].strip()
        return f"{label}\tEQU\t{value}\t{comment}".rstrip() + "\n"

    if head == "processor":
        return f"\tCPU\t{tail.strip()}\t{comment}".rstrip() + "\n"
    if head == "include":
        return f'\tINCLUDE\t"vcs.inc"\t{comment}'.rstrip() + "\n"

    in_data = head in DIRECTIVE
    if in_data:
        head = DIRECTIVE[head]
    else:
        head = FORCED.sub(r"\1", head)

    body = convert_lohi(f"{head}\t{tail}".rstrip() if head else "", in_data)
    if head.upper() == "ORG":
        body = "ORG\t" + tail.strip()

    out = f"{label}{gap if label else ''}"
    if not label:
        out = "\t"
    return f"{out}{body}\t{comment}".rstrip() + "\n"


# A FujiNet bank is mapped at $1000, not $F000. Rebasing is a matter of the
# ORG directives alone -- every other address in Combat is symbolic -- but it
# is not cosmetic: the rebased build is the BASELINE tools/check_patch.py
# compares the banked image against, and without it every absolute address's
# high byte reads as an undeclared change, because $F4 really did become $14.
ORGF = re.compile(r"^(\s*)ORG(\s+)\$F([0-9A-F]{3})\b", re.IGNORECASE)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    window = "--window" in sys.argv
    with open(args[0]) as f:
        lines = f.readlines()
    out = [convert(l) for l in lines]
    if window:
        out = [ORGF.sub(lambda m: "%sORG%s$1%s" % (m.group(1), m.group(2), m.group(3)), l)
               for l in out]
    sys.stdout.writelines(out)
    sys.stdout.write("\n\tEND\n")


if __name__ == "__main__":
    main()
