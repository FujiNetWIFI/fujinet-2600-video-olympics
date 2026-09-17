#!/usr/bin/env python3
"""zpmap.py -- what of zero page Video Olympics actually uses, and what is left.

Two things make the naive answer wrong, and both cost the Intellivision family
and Combat real time before they were written down:

  * A cell reached only by INDEXING is not free. `LDA $BB,X` with X = 0..3
    touches $BB-$BE, and a scan that only records the operand byte reports
    $BC-$BE as untouched. Every indexed base has to be walked and bounded.

  * THE STACK IS IN ZERO PAGE. The 6507 puts the stack at $0180-$01FF, which
    mirrors onto $80-$FF, so the top of zero page is stack and the real bound
    is the deepest the call graph goes -- plus whatever the kernel's
    stack-pointer-as-TIA-pointer trick does on the way past.

Run it against the dump. It prints the free list the netcode may use.
"""
import sys

BASE = 0xF000
RAM_LO, RAM_HI = 0x80, 0xFF          # the 128 bytes of RIOT RAM

# opcode -> (mnemonic, operand kind). Only the zero-page forms matter here.
ZP   = {0x05:'ORA',0x06:'ASL',0x24:'BIT',0x25:'AND',0x26:'ROL',0x45:'EOR',
        0x46:'LSR',0x65:'ADC',0x66:'ROR',0x84:'STY',0x85:'STA',0x86:'STX',
        0xA4:'LDY',0xA5:'LDA',0xA6:'LDX',0xC4:'CPY',0xC5:'CMP',0xC6:'DEC',
        0xE4:'CPX',0xE5:'SBC',0xE6:'INC'}
ZPX  = {0x15:'ORA',0x16:'ASL',0x35:'AND',0x36:'ROL',0x55:'EOR',0x56:'LSR',
        0x75:'ADC',0x76:'ROR',0x94:'STY',0x95:'STA',0xB4:'LDY',0xB5:'LDA',
        0xD5:'CMP',0xD6:'DEC',0xF5:'SBC',0xF6:'INC'}
ZPY  = {0x96:'STX',0xB6:'LDX'}
IZX  = {0x01:'ORA',0x21:'AND',0x41:'EOR',0x61:'ADC',0x81:'STA',0xA1:'LDA',0xC1:'CMP',0xE1:'SBC'}
IZY  = {0x11:'ORA',0x31:'AND',0x51:'EOR',0x71:'ADC',0x91:'STA',0xB1:'LDA',0xD1:'CMP',0xF1:'SBC'}

SIZES = {**{k:2 for k in list(ZP)+list(ZPX)+list(ZPY)+list(IZX)+list(IZY)}}


def scan(path, listing):
    """Use the ASSEMBLER'S listing for the code/data split -- a linear walk
    through a data table is misaligned garbage and invents references."""
    import re
    code = set()
    pat = re.compile(r'^\s*(\d+)/\s*([0-9A-F]{4})\s*:\s*([0-9A-F][0-9A-F ]*?)\s{2,}(\S.*)$')
    for ln in open(listing, errors='replace'):
        m = pat.match(ln)
        if not m:
            continue
        txt = m.group(4).strip()
        if txt.upper().startswith(('DB', 'DW', 'EQU', 'ORG', 'CPU', 'END')):
            continue
        code.add(int(m.group(2), 16))
    d = open(path, 'rb').read()
    touched = {}          # addr -> set of reasons
    indexed = {}          # base -> set of mnemonics
    for a in sorted(code):
        i = a - BASE
        if not (0 <= i < len(d)):
            continue
        op, operand = d[i], d[i + 1] if i + 1 < len(d) else 0
        if op in ZP:
            touched.setdefault(operand, set()).add('%s $%02X' % (ZP[op], operand))
        elif op in ZPX or op in ZPY or op in IZX or op in IZY:
            tbl = ZPX if op in ZPX else ZPY if op in ZPY else IZX if op in IZX else IZY
            indexed.setdefault(operand, set()).add('%s $%02X,%s' %
                (tbl[op], operand, 'X' if op in ZPX or op in IZX else 'Y'))
    return touched, indexed


def main():
    rom, lst = sys.argv[1], sys.argv[2]
    touched, indexed = scan(rom, lst)
    direct = {a for a in touched if RAM_LO <= a <= RAM_HI}
    print("DIRECT zero-page references (%d cells in RAM):" % len(direct))
    print("   " + " ".join("$%02X" % a for a in sorted(direct)))
    print()
    print("INDEXED BASES -- each one is a RANGE, not a cell. Bound them by hand:")
    for b in sorted(indexed):
        if RAM_LO <= b <= RAM_HI or b < RAM_LO:
            print("   $%02X  %s" % (b, ", ".join(sorted(indexed[b]))))
    print()
    print("A cell is free only if it is in neither list AND no indexed base reaches it.")
    print("See vodefs.inc for the bounded answer and the stack reservation.")


if __name__ == '__main__':
    main()
