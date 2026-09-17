; voboot.asm -- bank 0: cold start and, later, the session.
;
; IT OWNS THE RAM CLEAR. The stock ROM opens with one at $F000, and that is
; where a bank switch lands, so it could not stay there -- and it could not
; simply move down the game bank either, because the game bank is the one with
; no room. Here it costs nothing: this bank is 9 bytes used of 2048.
;
; Moving it also makes the game bank's entry unconditional. Every arrival at
; BANKGAME $1000 is now a warm one, because the only other way in is through
; here.
;
; A CARTRIDGE WITH NO SERVER IS STILL A VIDEO OLYMPICS CARTRIDGE (4.13). That
; is not only the right product behaviour -- it is what keeps `make det` honest,
; because the gate that compares this build against the 1977 ROM reaches the
; game through exactly this path, with ENDPOINT pointed at a port nothing
; listens on.

        CPU     6502
        INCLUDE "vcs.inc"
        INCLUDE "fujinet.inc"
        INCLUDE "cfg.inc"
        INCLUDE "vodefs.inc"
        INCLUDE "tail.inc"

VOBANK  EQU     BANKBOOT

        ORG     $1000

; The boot bank is entered twice: once at power-on, and once more if the peer
; leaves -- because this is the only bank with a text kernel in it, and
; "OPPONENT HAS LEFT" is worth saying in words.
VOBOOT: cld
        lda     VOENT
        and     #VOE_LEFT
        beq     VOBGO
        jmp     CSLEFT
VOBGO:  jmp     VOBENT          ; the session proper, in vosess.inc

        INCLUDE "vosess.inc"
        INCLUDE "vostart.inc"
        INCLUDE "voappk.inc"
        INCLUDE "vodisp.inc"

        IF      * > $1800
        ERROR   "the boot bank has overflowed"
        ENDIF

        END
