; votail.asm -- the fixed half: the trampoline, the shared transport, and the
; cold stub.
;
; $1800-$1F1F is the cartridge's mailbox -- text planes, reply window, control
; page, TX page, status -- and the cartridge paints it. The client owns
; $1F20-$1FFB, which fuji_mailbox.h calls the fixed tail, plus the vectors.
;
; Everything here has to be at an address that does not move, for three
; different reasons:
;
;   * The store that switches bank is the LAST instruction fetched from the
;     old bank and the very next fetch comes from the new one, so the jump
;     after it cannot live in a bank.
;   * This console has no reset line to the cartridge. The RESET switch
;     restarts the 6507 with whatever bank was last selected still mapped, so
;     a cold stub living in bank 0 would simply not be there when it was
;     needed.
;   * The transport is the same bytes in all three banks, and a bank is 2048.
;     Here it is one copy that all of them can reach.

        CPU     6502
        INCLUDE "vcs.inc"
        INCLUDE "fujinet.inc"
        INCLUDE "cfg.inc"
        INCLUDE "vodefs.inc"

; The tail has no bank identity -- it IS the shared copy -- but it is assembled
; next to the same equates every bank uses.
VOBANK  EQU     BANKBOOT
VOHASTXT EQU    1               ; the tail carries the text primitives for now;
                                ;   if it ever runs short they move into bank 0,
                                ;   which is the only bank that draws text

; VOGOTO is not a label: vodefs.inc gives it a fixed address and this ORG is
; what makes that true, so the two cannot drift apart. It must be FIRST in the
; tail, because it is the one address a bank has to know before build/tail.inc
; exists.
        ORG     VOGOTO

; ---------------------------------------------------------------------------
; VOGOTO -- select bank A and enter it at $1000.
;
; ONE store. FN_HOT_BANK lives in the bit-7-set half of the control page, which
; is the one-shot half: the bank number is in the ADDRESS and the data is
; ignored. A store to $1DFF afterwards would be FN_H_COMMIT, and it would
; commit whatever FN_REG_* was last armed, carrying this store's value.
;
; `sta FNRSEL,x` is the documented-safe indexed form: the base low byte is $00,
; so the index cannot carry and the dummy read that STA abs,X always performs
; lands on the same address as the write -- one parked access, not two.
        clc
        adc     #FH_BANK
        tax
        sta     FNRSEL,x
; RESET THE STACK. A bank switch is a JUMP and nothing ever returns through
; one, so every switch abandons whatever return addresses were on the stack.
; Video Olympics wants SP = $FF at the top of its frame loop anyway -- START
; does exactly this -- so here the reset is both free and correct.
;
; The store above has already switched the bank; these instructions are fetched
; from the FIXED tail, which is not banked, so they still execute.
        ldx     #$FF
        txs
        jmp     $1000

; ---------------------------------------------------------------------------
; The shared transport. tools/mktail.py turns the addresses these assemble to
; into build/tail.inc, which is what the banks include.
        INCLUDE "vocore.inc"

; ---------------------------------------------------------------------------
; VOCOLD -- power-on and RESET.
;
; Deliberately tiny. All that has to be here is what cannot be anywhere else:
; the arming pair, because banking is a control-page operation and that page
; decodes nothing until an ordered pair of stores carrying two specific values
; arrives -- and the bank switch itself.
;
; It forces a cold entry by zeroing VOENT. This console does not clear its RAM
; on a reset, so the byte that says what the bank being entered should do still
; holds whatever the last frame set it to; without this, a RESET taken mid-match
; comes back into the frame loop with a sim tick, a ring and a socket that no
; longer mean anything.
VOCOLD: sei
        cld
        ldx     #$FF
        txs
        lda     #0
        sta     VOENT
        lda     #FNAM1
        sta     FNRSEL+FH_ARM1
        lda     #FNAM2
        sta     FNRSEL+FH_ARM2
        lda     #BANKBOOT
        jmp     VOGOTO

; ---------------------------------------------------------------------------
; THE VECTORS, AND THE ONE THAT IS NOT SPARE.
;
; Combat pointed both of these at its cold stub, because Combat never issues a
; BRK. Video Olympics issues three -- $F262, $F2C8 and $F453 -- and they are
; not faults. BRK pushes PC+2, so the $EA that follows each one is skipped and
; RTI returns past it: it is a TWO-BYTE SUBROUTINE CALL, one byte cheaper than
; JSR, and the routine it calls is the four `ASL / ADC #$00` pairs at $F438
; that rotate A left by four -- swap the nibbles.
;
; In a flat 2K cartridge $FFFE is the game's own vector and this is invisible.
; Here the vectors live in the fixed tail, so it has to be pointed back into
; the bank by hand. All three call sites and the handler are in BANKGAME, so
; whichever bank is mapped when a BRK executes is always the one holding
; $1438, and the vector can be a constant.
;
; Get this wrong and the first nibble swap enters a cold boot in the middle of
; a frame, which looks like nothing in the netcode and is very hard to read.
        ORG     $1FFC
        DW      VOCOLD          ; RESET
        DW      VOBRKH          ; IRQ/BRK -- the nibble-swap call, in BANKGAME

        END
