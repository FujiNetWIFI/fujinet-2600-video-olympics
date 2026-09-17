#!/usr/bin/env python3
"""recon.py -- the port map for an Atari 2600 ROM.

The 2600 equivalent of intv-baseball-experiment/tools/recon.py, and it exists
for the same reason: the next port should start by running this, not by reading
1991 lines of disassembly. It is calibrated so that it reproduces the Combat map
in PORTING.md exactly -- that calibration is the whole point, because a recon
tool nobody has checked against a known answer is a tool that invents answers.

It reports, for any 2600 ROM:

  * size, the mapper the size implies, and where the vectors point
  * filler runs, which is where injected code can go in a flat image
  * EVERY read of a console input port, with the address of the instruction --
    the patch map's raw material, and the thing that decides how hard the port is
  * which zero-page cells the ROM touches, and therefore which are free
  * the frame's shape: the RIOT timer values written, and the WSYNC count
  * candidates for nondeterminism, which is what decides whether lockstep can
    work at all

The scan is LINEAR, like the firmware's checkrom.py, and for the same reason it
is sound there: the 6507 has no interrupts (MAME's m6507.cpp: "no NMI, no SO, no
SYNC"), so every access a running program makes comes from its own instruction
stream. A linear walk still desynchronises inside data tables, so findings are
reported with their region and a table of contents is printed alongside -- read
it, do not trust it blind.

Usage: recon.py rom.bin [--base $F000]
"""

import sys
from collections import defaultdict

# The classic vcs.h layout, where the TIA READ registers are at $00-$0D and not
# the $30-$3D mirror. Combat's disassembly needs that base and so does the
# FujiNet family's vcs.inc; a ROM built against the mirror will show its input
# reads at $3C/$3D instead and the names below still say which port it is.
PORTS = {
    0x0280: "SWCHA  joysticks, P0 in bits 7-4",
    0x0282: "SWCHB  RESET/SELECT/difficulty/B&W",
    0x000C: "INPT4  P0 trigger", 0x003C: "INPT4  P0 trigger (mirror)",
    0x000D: "INPT5  P1 trigger", 0x003D: "INPT5  P1 trigger (mirror)",
    0x0008: "INPT0  paddle", 0x0009: "INPT1  paddle",
    0x000A: "INPT2  paddle", 0x000B: "INPT3  paddle",
}
TIMERS = {0x0294: "TIM1T", 0x0295: "TIM8T", 0x0296: "TIM64T", 0x0297: "T1024T"}
INTIM, WSYNC, CXCLR = 0x0284, 0x0002, 0x002C

MAPPER = {0x800: "2K flat (mirrored into 4K)", 0x1000: "4K flat",
          0x2000: "8K -- F8, E0, UA or FE", 0x3000: "12K -- FA",
          0x4000: "16K -- F6", 0x8000: "32K -- F4"}

LEN = [1] * 256
for op in (0x69,0x29,0xC9,0xE0,0xC0,0x49,0xA9,0xA2,0xA0,0x09,0xE9,0xA5,0xA6,
           0xA4,0x85,0x86,0x84,0x65,0x25,0x06,0x24,0xC5,0xC6,0x45,0xE6,0x46,
           0x26,0x66,0xE5,0x05,0x75,0x35,0x16,0xD5,0xD6,0x55,0xF6,0x56,0x36,
           0x76,0xF5,0x15,0xB5,0xB4,0x95,0x94,0xB6,0x96,0x61,0x21,0xC1,0x41,
           0xA1,0x01,0xE1,0x81,0x71,0x31,0xD1,0x51,0xB1,0x11,0xF1,0x91,
           0x10,0x30,0x50,0x70,0x90,0xB0,0xD0,0xF0):
    LEN[op] = 2
for op in (0x6D,0x2D,0x0E,0x2C,0xCD,0xEC,0xCC,0xCE,0x4D,0xEE,0x4C,0x20,0xAD,
           0xAE,0xAC,0x4E,0x0D,0x2E,0x6E,0xED,0x8D,0x8E,0x8C,0x7D,0x3D,0x1E,
           0xDD,0xDE,0x5D,0xFD,0xFE,0x5E,0xBD,0xBC,0x3E,0x7E,0x1D,0x9D,0x79,
           0x39,0xD9,0x59,0xB9,0xBE,0x19,0xF9,0x99,0x6C):
    LEN[op] = 3

# Absolute and absolute,X/Y LOADS -- a read of a port is one of these.
ABS_LOAD = {0xAD: "LDA", 0xAE: "LDX", 0xAC: "LDY", 0x2C: "BIT", 0xCD: "CMP",
            0xEC: "CPX", 0xCC: "CPY", 0x0D: "ORA", 0x2D: "AND", 0x4D: "EOR",
            0x6D: "ADC", 0xED: "SBC",
            0xBD: "LDA,X", 0xB9: "LDA,Y", 0xBE: "LDX,Y", 0xBC: "LDY,X",
            0x3D: "AND,X", 0x39: "AND,Y", 0xDD: "CMP,X", 0xD9: "CMP,Y"}
ZP_LOAD = {0xA5: "LDA", 0xA6: "LDX", 0xA4: "LDY", 0x24: "BIT", 0xC5: "CMP",
           0xE4: "CPX", 0xC4: "CPY", 0x05: "ORA", 0x25: "AND", 0x45: "EOR",
           0x65: "ADC", 0xE5: "SBC",
           0xB5: "LDA,X", 0xB4: "LDY,X", 0xB6: "LDX,Y", 0x35: "AND,X",
           0xD5: "CMP,X"}
ZP_STORE = {0x85: "STA", 0x86: "STX", 0x84: "STY", 0x95: "STA,X",
            0x94: "STY,X", 0x96: "STX,Y", 0xE6: "INC", 0xC6: "DEC",
            0xF6: "INC,X", 0xD6: "DEC,X", 0x06: "ASL", 0x46: "LSR",
            0x26: "ROL", 0x66: "ROR", 0x16: "ASL,X", 0x56: "LSR,X"}
ABS_STORE = {0x8D: "STA", 0x8E: "STX", 0x8C: "STY", 0x9D: "STA,X",
             0x99: "STA,Y", 0xEE: "INC", 0xCE: "DEC"}


def runs(img, val, least=6):
    out, start = [], None
    for i, b in enumerate(img):
        if b == val:
            if start is None:
                start = i
        elif start is not None:
            if i - start >= least:
                out.append((start, i - 1))
            start = None
    if start is not None and len(img) - start >= least:
        out.append((start, len(img) - 1))
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    img = open(args[0], "rb").read()
    # A 2K image is mirrored into $F000-$FFFF and the 6507 fetches its vectors
    # from the top -- but disassemblies (and this project) map it at $F000,
    # because that is where the code believes it is. Defaulting to $F800 would
    # print every address half a page from where the source says it is.
    base = 0xF000 if len(img) == 0x800 else 0x10000 - len(img)
    if "--base" in sys.argv:
        base = int(sys.argv[sys.argv.index("--base") + 1].lstrip("$"), 16)

    print("recon: %s, %d bytes" % (args[0], len(img)))
    print("  mapper by size : %s" % MAPPER.get(len(img), "unknown"))
    print("  mapped at      : $%04X-$%04X" % (base, base + len(img) - 1))
    if len(img) >= 6:
        rst = img[-4] | (img[-3] << 8)
        nmi = img[-6] | (img[-5] << 8)
        print("  RESET vector   : $%04X%s" % (
            rst, "" if base <= rst <= base + len(img) - 1 else "  (OUTSIDE the image!)"))
        print("  NMI vector     : $%04X  (the 6507 has no NMI; PlusROM keeps its"
              " host and path here)" % nmi)

    print("\nfiller runs -- where injected code fits in a flat image")
    any_run = False
    for val in (0xFF, 0x00):
        for lo, hi in runs(img, val):
            print("  $%04X-$%04X  %4d bytes of $%02X"
                  % (base + lo, base + hi, hi - lo + 1, val))
            any_run = True
    if not any_run:
        print("  none of six bytes or more -- this ROM is full")

    ports = defaultdict(list)
    timers = defaultdict(list)
    zp_read, zp_write = defaultdict(int), defaultdict(int)
    indexed = set()
    wsync = intim = cxclr = 0
    pc = 0
    while pc < len(img) - 2:
        op = img[pc]
        n = LEN[op]
        addr = base + pc
        if n == 3:
            tgt = img[pc + 1] | (img[pc + 2] << 8)
            if op in ABS_LOAD:
                if tgt in PORTS:
                    ports[tgt].append((addr, ABS_LOAD[op]))
                if tgt == INTIM:
                    intim += 1
            if op in ABS_STORE:
                if tgt in TIMERS:
                    timers[tgt].append(addr)
                if tgt == WSYNC:
                    wsync += 1
                if tgt == CXCLR:
                    cxclr += 1
        elif n == 2:
            tgt = img[pc + 1]
            if op in ZP_LOAD:
                if 0x80 <= tgt:
                    zp_read[tgt] += 1
                    if "," in ZP_LOAD[op]:
                        indexed.add(tgt)
                elif tgt in PORTS:
                    ports[tgt].append((addr, ZP_LOAD[op]))
            if op in ZP_STORE:
                if 0x80 <= tgt:
                    zp_write[tgt] += 1
                    if "," in ZP_STORE[op]:
                        indexed.add(tgt)
                elif tgt == WSYNC:
                    wsync += 1
                elif tgt == CXCLR:
                    cxclr += 1
        pc += n

    print("\nINPUT SURFACE -- the patch map's raw material")
    if not ports:
        print("  none found (a ROM that reads no port is a ROM this scan"
              " mis-walked, or a demo)")
    for p in sorted(ports):
        for addr, how in ports[p]:
            print("  $%04X  %-6s %s" % (addr, how, PORTS[p]))
    print("  %d distinct sites. Every one has to be patched for lockstep --"
          % sum(len(v) for v in ports.values()))
    print("  INCLUDING the local player's, because in lockstep both inputs are")
    print("  applied at the same tick on both consoles.")

    print("\nFRAME SHAPE")
    for t in sorted(timers):
        for addr in timers[t]:
            print("  $%04X  writes %s" % (addr, TIMERS[t]))
    print("  %d WSYNC strobes, %d INTIM reads, %d CXCLR strobes" % (wsync, intim, cxclr))
    if cxclr:
        print("  CXCLR is the frame's natural sampling point: it is strobed once,")
        print("  after the game logic and before the picture.")

    print("\nZERO PAGE")
    touched = sorted(set(zp_read) | set(zp_write))
    free = [a for a in range(0x80, 0x100) if a not in touched]
    print("  touched directly : %d cells" % len(touched))
    print("  free, UPPER BOUND: %d cells -- %s" % (len(free), fmt_ranges(free)))
    print("  indexed bases    : %s" % (fmt_ranges(sorted(indexed)) or "none"))
    print()
    print("  THE FREE LIST IS AN UPPER BOUND AND USUALLY A BAD ONE. A scan sees")
    print("  the BASE of `LDA DIRECTN,X` and not the range X takes, so every")
    print("  array in the program reads as one cell. On Combat this prints 62")
    print("  free where the true answer is 26: HIRES alone is sixteen bytes")
    print("  behind a single base. Walk the indexed bases above, bound each")
    print("  one's index by hand, and subtract. There is no shortcut, and a tool")
    print("  that pretended otherwise would cost more than it saved.")
    print("  The stack lives at the top of these; subtract its depth too.")

    print("\nNONDETERMINISM -- what decides whether lockstep can work")
    print("  There is no RNG instruction to look for on a 6502, so this is a")
    print("  checklist rather than a scan:")
    print("   * a read of INTIM whose value is USED (not just polled to zero)")
    print("   * anything derived from a collision latch read at a variable point")
    print("   * RESPx/RESMPx strobes, which fix an object's X from the raster")
    print("     position and so depend on the cycle they execute at")
    print("   * cells the ROM never initialises: compare the write set above")
    print("     against the range its clear loop actually covers")
    print("  Combat has none of the first two, one of the third (InitPF's RESP0,")
    print("  anchored by a WSYNC that looks stray and is not) and five of the")
    print("  fourth. See PORTING.md.")


def fmt_ranges(cells):
    out, i = [], 0
    while i < len(cells):
        j = i
        while j + 1 < len(cells) and cells[j + 1] == cells[j] + 1:
            j += 1
        out.append("$%02X" % cells[i] if i == j else "$%02X-$%02X" % (cells[i], cells[j]))
        i = j + 1
    return " ".join(out)


if __name__ == "__main__":
    main()
