; voinput.inc -- the shim, the capture boundary and the mixer. Runs in the
; KERNEL bank, from its entry. See vogate.inc for the half that cannot.

        IF      VOLAG
; ---------------------------------------------------------------------------
; The interception proof (`make lag`): refresh the shadows once every VOLAGM+1
; frames. The game then answers the paddle in visible steps about a second
; apart -- and if it does not, something is still reading a live port.
VOLAGM  EQU     $3F
        ENDIF

; ---------------------------------------------------------------------------
; VOSHIM -- once a frame. Returns with VOADV nonzero if the sim should advance.
VOSHIM:
        lda     #$FF
        sta     VOADV            ; assume it does

        IF      VOLAG
        lda     $88              ; the frame counter, incremented at $F07B
        and     #VOLAGM
        beq     VOSHIM1
        rts
VOSHIM1:
        ENDIF

        lda     VOENT
        and     #VOE_NET
        bne     VONET
        jmp     VOLOC0           ; not in a match: mirror the real console

; ---------------------------------------------------------------------------
; In a match.
VONET:
; Only a tick boundary has anything to decide; the other frames of a tick
; re-use the shadows that are already there. The phase is two bits of VOENT and
; NOT a game cell -- see the note on VOE_PH in vodefs.inc.
        lda     VOENT
        and     #VOE_PH
        beq     VONT1
        jmp     VOPHI            ; mid-tick: bump the phase and run

VONT1:
; A RECORD, ONCE SENT, IS IMMUTABLE. Everything in this block happens ONLY on a
; boundary that actually advanced the tick.
;
; The transport free-runs: it sends the record for VOTICK+d whenever it reaches
; its WRITE step, several times per tick, and a duplicate is harmless only
; because it is IDENTICAL. Re-capturing on a stalled boundary would make it not
; identical -- the peer would already have stored and SIMULATED the first
; version of a tick while this console went on to use the second.
;
; It is invisible while nobody touches anything, because every capture returns
; the same idle bytes. The moment somebody presses SELECT during a stall, one
; console counts the press and the other does not, and two consoles end up on
; different variations of a fifty-variation cartridge.
;
; So: advance the tick, sample the checksum and capture the console as one
; indivisible act, or do none of them.
        lda     VOENT
        and     #VOE_RAN
        bne     VONT1A
        jmp     VONT1B           ; the tick has not moved: nothing to re-commit
VONT1A:
        inc     VOTICK
        lda     VOENT
        and     #$FF-VOE_RAN
        sta     VOENT

; The checksum, of the state ENTERING the tick just advanced to. The boundary
; is the one instant two consoles can agree on: the game logic runs on every
; frame and only the input changes at the tick rate, so the state moves VOK
; times a tick and a checksum taken wherever the transport got to is a
; different quantity on each machine. The record stamped Y then carries the
; checksum of Y-d, on both, whatever either stalled through.
;
; It is READ rather than computed, from the cell the kernel bank filled at the
; end of the previous frame -- the same instant by the argument in vocrck.inc,
; and twenty-two bytes this bank does not have.
        lda     VOCRCK          ; computed in the kernel bank at the end of the
        sta     VOCRCV          ;   last frame -- the same instant, see vocrck.inc

; And this console's own paddle and switches, for the tick VOD ahead. Captured
; once, here, and never revised. VOCAP leaves the paddle in A and the switch
; byte in VOTMP.
        jsr     VOCAP
        pha
        lda     VOTICK
        clc
        adc     #VOD
        and     #VOLM
        asl     a                ; a slot is VORS = 2 bytes
        tay
        pla
        sta     VOLOC,y
        lda     VOTMP
        sta     VOLOC+1,y
VONT1B:

; Has the peer's input for this tick arrived? VORWAT is the highest remote tick
; received.
;
; THIS IS THE ONE SIGNED COMPARE IN THE NETCODE, and it is signed on purpose:
; the tick is eight bits and wraps, and a window of +-127 either side is what
; makes the wrap invisible. Every other comparison here goes through the carry.
        lda     VORWAT
        sec
        sbc     VOTICK
        bpl     VONT2
        jmp     VOSTALL

VONT2:
; ---- the tick runs ----
        lda     VOERR            ; a tick that ran clears the stall counter
        and     #$0F
        sta     VOERR

; Our own bytes for this tick were captured VOD ticks ago and have been sitting
; in the local ring ever since. That is the whole of delay-based lockstep: our
; input is delayed exactly as far as the peer's, so both players feel the same
; lag and neither is ahead of the other.
        lda     VOTICK
        and     #VOLM
        asl     a
        tay
        lda     VOLOC,y
        sta     VOW2             ; ours: the paddle
        lda     VOLOC+1,y
        sta     VOW0             ; ours: the switches and triggers

; The peer's, for the same tick, out of the remote ring.
        lda     VOTICK
        and     #VORM
        asl     a
        tay
        lda     VORING,y
        sta     VOW3
        lda     VORING+1,y
        sta     VOW1

; The HOST drives player 0 -- the left port's paddle A -- and the guest player
; 2, the right port's. A guest's own bytes are therefore the guest's, and the
; two pairs have to change places. After this, VOW0/VOW2 are the host's and
; VOW1/VOW3 the guest's on BOTH consoles, so everything below computes the same
; answer on each without a protocol for it.
; A BRANCH, NOT A JUMP, ONLY WHERE IT CAN REACH. VOMIX is in another hole
; several hundred bytes away -- the two halves of the shim do not fit in one --
; so the test is inverted and the long way out is a JMP.
        lda     VOENT
        and     #VOE_ROLE
        bne     VOSWAP
        jmp     VOMIX
VOSWAP:
        lda     VOW0
        ldy     VOW1
        sty     VOW0
        sta     VOW1
        lda     VOW2
        ldy     VOW3
        sty     VOW2
        sta     VOW3

        jmp     VOMIX
