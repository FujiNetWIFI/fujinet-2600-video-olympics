; vocap.inc -- the local capture, the stall, the local-play mirror, and the
; phase step. Everything the gate needed that would not fit beside it.

; ---------------------------------------------------------------------------
; VOCAP -- this console's own input, as the two bytes the wire carries.
;
; Returns the paddle in A and the switch/trigger byte in VOTMP.
;
; THE PADDLE COMES FROM $BB, NOT FROM INPT0. The raw capture is the display
; kernel's job: it counts scanlines until the controller's capacitor charges,
; parks the count in $84/$85, and the two-tap filter at $F643 folds it into
; $BB-$BE on the way out. Reading the FILTERED cell is right for three reasons
; -- it is the number the game would have read, it is one byte instead of a
; scanline loop, and it means both consoles store byte-identical values rather
; than each filtering the other's samples with its own history.
;
; $BB is player 0's, which is this console's own left-port paddle A whatever
; role it is playing. The role only decides where the byte LANDS at the far
; end, and VOMIX does that.
VOCAP:
; The switches and triggers first, because the paddle wants A to itself.
;
; SWCHA's bits 7 and 6 are the left port's two triggers -- this console's own
; pair -- and they go on the wire exactly where they are. A console always
; sends its own left port; the mixer shifts the guest's down to bits 3 and 2.
        lda     SWCHA
        and     #SA_L
        sta     VOTMP

; RESET and SELECT, SHIFTED UP FROM BITS 0 AND 1 TO BITS 4 AND 5.
;
; That shift is the whole of a bug worth remembering. SWCHB puts RESET at bit 0
; and SELECT at bit 1; the wire byte puts them at 4 and 5. Masking SWCHB with
; #$30 and calling it done reads the console's UNUSED bits 4 and 5 instead --
; which read high on real hardware and in MAME, so the wire says "not pressed"
; for ever and NEITHER SWITCH EVER REACHES THE OTHER CONSOLE. The mixer at the
; far end shifts them back down correctly, so both halves look right in
; isolation and the pair is silently broken.
;
; It survives every gate that does not press anything: a verdict which checks
; that the two consoles AGREED is perfectly satisfied by two consoles that both
; ignore SELECT.
;
; SWCHB IS READ ONCE AND PARKED ON THE STACK, not read twice: two `LDA SWCHB`
; are six bytes, one plus a PHA/PLA is five, and this hole is not generous. The
; stack can afford it -- the deepest this path goes is VOGATE -> VOSHIM ->
; VOCAP, which is six bytes of the six between $FA and $FF... and that is
; exactly why VORSY below is a JMP and not a JSR.
        lda     SWCHB
        pha
        and     #$03            ; bit 0 RESET, bit 1 SELECT, both active low
        asl     a
        asl     a
        asl     a
        asl     a               ; -> IW_RST and IW_SEL
        ora     VOTMP
        sta     VOTMP
        pla
        and     #$40            ; SWCHB bit 6: this console's own difficulty
        lsr     a
        lsr     a
        lsr     a               ; $40 -> $08, IW_DIFF
        ora     VOTMP
        ora     #IW_SPARE       ; the unused low bits read high
        sta     VOTMP

;
; THE DESYNC REPAIR IS A SYNTHETIC RESET, pressed into this byte.
;
; A repair has to happen at the SAME TICK on both consoles or it is just a
; second desync. There is no spare byte of zero page to schedule one in, and
; there does not need to be: RESET is already an event the two machines agree
; about to the tick, because it is ANDed on the wire and applied through the
; delay ring like any other input. So the repair is a synthetic press and it
; arrives at both ends by the road every real press takes.
;
; One console arming it is enough, for the same reason either player may start
; a game. Both usually do arm, because each one sees the other's checksum.
;
; One-shot: the bit is cleared as the press is made, so the press lasts exactly
; one tick.
;
; THE SCORES ARE WIPED AND THAT IS CORRECT rather than a wart. The two consoles
; have just disagreed, so their scores may disagree too. The only state agreed
; by construction is the state the game itself builds from nothing.
        ldy     VOENT
        tya
        and     #VOE_RSY
        beq     VOCAP9
        eor     VOENT           ; A was exactly the bit, so this is VOENT with
        sta     VOENT           ;   it cleared -- one-shot, in two instructions
        lda     VOTMP
        and     #$FF-IW_RST     ; RESET is active low, so CLEARING the bit is
        sta     VOTMP           ;   the press
VOCAP9: lda     $BB             ; this console's own paddle A, already filtered
        rts

