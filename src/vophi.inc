; vophi.inc -- the frame-in-tick phase, in the 51 bytes in front of the
; blank-line generator. It is eight bytes; that hole is the smallest in the
; bank and this is the smallest thing that has to be in this bank.

; ---------------------------------------------------------------------------
; VOPHI -- advance the frame-in-tick phase, and return.
;
; FOUR INSTRUCTIONS OF WORK. The phase is the top two bits of VOENT, so adding
; $40 increments it and the carry out of bit 7 is simply discarded -- the low
; bits, which are the entry and role flags, cannot be disturbed because nothing
; carries into them. Masking would be three instructions of ceremony.
;
; THE SIM CLOCK MUST NOT BE A CELL THE GAME CAN WRITE, which is why the phase
; lives in VOENT and not in the game's own frame counter at $88. $88 is reset
; at $F0DF on a console-switch change -- on exactly the frames two consoles are
; least likely to be in step -- and a netcode whose clock the simulation can
; reset is measuring a disagreement with an instrument derived from it.
VOPHI:
        lda     VOENT
        clc
        adc     #VOE_PH1
        sta     VOENT
        rts
