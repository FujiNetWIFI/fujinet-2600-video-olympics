; vogame.asm -- bank 1: Video Olympics' frame loop, game logic and the gate.
;
; Everything the game does between VSYNC and the display kernel. The kernel
; itself is in bank 2, because Video Olympics is exactly 2048 bytes and a bank
; is exactly 2048 bytes, so something had to move to make room for netcode --
; and the kernel is the half with no netcode in it.
;
; Every byte here keeps the address it has in the cartridge dump, rebased
; $F000 -> $1000. THREE holes are left by the regions that went to bank 2 --
; three and not two, because the two BOTH regions are emitted HERE as well and
; are therefore not free:
;
;   $1585-$15B7   51 bytes, in front of the blank-line generator
;   $15D0-$1657  136 bytes, where the sprite kernel was
;   $1753-$17FF  173 bytes, where its graphics were
;
; Each is entered by its region-end label rather than a literal, so a change to
; tools/vo.cfg that moves a boundary moves these with it.
;
; THIS BANK OWNS THE BRK HANDLER. $1438 is four `ASL / ADC #$00` pairs that
; rotate A left by four -- the nibble swap the game calls three times with a
; two-byte BRK instead of a three-byte JSR. votail.asm points $1FFE at it. All
; three call sites are in this bank too, so the bank mapped when a BRK executes
; is always this one.

        CPU     6502
        INCLUDE "vcs.inc"
        INCLUDE "fujinet.inc"
        INCLUDE "cfg.inc"
        INCLUDE "vodefs.inc"
; The shared transport's addresses, generated from the tail's own listing so
; there is no hand-kept list to go stale.
        INCLUDE "tail.inc"

VOBANK  EQU     BANKGAME

        INCLUDE "vo.inc"

; ---------------------------------------------------------------------------
; The first hole: 211 bytes where the display kernel used to be. VORE2 is the
; end of the region the generator just emitted, so this cannot go stale when a
; patch changes the length of the game code in front of it.
; The 51 bytes in front of the blank-line generator: the phase step and the
; stall, which are the two smallest things here.
; The 51 bytes in front of the blank-line generator. THIRTEEN of them are all
; this bank needs: the gate's decision, and nothing else. Everything that
; computes VOADV runs in the kernel bank -- see the note at the top of
; vogate.inc.
; THE SHIM IS LAID OUT BY SIZE ACROSS THREE HOLES, none of them big enough for
; all of it. That is the character of this bank and it is why every hole has a
; guard on it.
;
;   A  $1585-$15B7   51 bytes   the gate, the phase step, the local mirror
;   B  $15D0-$1657  136 bytes   the mixer and the capture
;   C  $1753-$17FF  173 bytes   the shim's head and the stall
;
; The checksum is the one piece that is NOT here: it is twenty-two bytes and
; there were not twenty-two to spare, so it runs in the kernel bank instead --
; see vocrck.inc for why that is the same number.
        ORG     VORE2
        INCLUDE "vogate.inc"
        INCLUDE "vophi.inc"
        INCLUDE "voloc0.inc"

        IF      * > $15B8
        ERROR   "hole A has overrun LF5B8 at $15B8"
        ENDIF

; ---------------------------------------------------------------------------
; The second hole: 188 bytes where the kernel's playfield and score graphics
; used to be. They are bank 2's now, and it is the only bank that draws.
;
; Unlike a flat image there are no vectors at $17FC to avoid -- in a banked
; cartridge the vectors live in the fixed tail -- so the hole runs to $1800.
; The 173 bytes where the kernel's graphics were. Unlike a flat image there are
; no vectors at $17FC to avoid -- in a banked cartridge the vectors live in the
; fixed tail -- so the hole runs to the end of the bank.
; The 136 bytes where the sprite kernel was, and the 173 where its graphics
; were, are both SPARE in this bank now. They are the room the four-player
; upgrade will want.
        ORG     VORE4
        INCLUDE "vomix.inc"
        INCLUDE "vocap.inc"


        IF      * > $1658
        ERROR   "hole B has overrun the sprite kernel's region at $1658"
        ENDIF

        ORG     VORE7
        INCLUDE "voinput.inc"
        INCLUDE "vostall.inc"

        IF      * > $1800
        ERROR   "hole C has overrun the end of the bank"
        ENDIF

        END
