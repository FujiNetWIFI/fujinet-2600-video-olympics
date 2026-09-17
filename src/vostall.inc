; vostall.inc -- the stall, and the peer-left exit it saturates into.
; In hole C behind the shim's head, because hole B had room for the mixer and
; the capture and not for this as well.

; ---------------------------------------------------------------------------
; VOSTALL -- the peer's input for this tick has not arrived.
;
; Nothing else happens. The frame still runs: VSYNC has already gone out, the
; band still times out, and the kernel still draws. What does not happen is the
; game logic, so the picture freezes coherently and the display never shudders.
;
; The stall count lives in VOERR's high nibble, saturating. A stall is normal;
; fifteen in a row at VOK frames each is about a second, and that is a peer
; that has gone. Saturation is the ONE "the peer is gone" condition: a clean
; leave arrives as a PEER_LEFT frame and the transport saturates this on the
; spot, and a console that was unplugged saturates it by simply not arriving.
VOSTALL:
        lda     #0
        sta     VOADV
        lda     VOERR
        cmp     #$F0            ; the low nibble is an error code below $10, so
        bcc     VOSTA1          ;   this is exactly "high nibble is already $F"
; The peer is gone. Hand back to the boot bank, which is the only bank with a
; text kernel in it, and let it say so in words. A JUMP, not a call: the
; trampoline resets the stack, so the JSR that reached the shim is abandoned on
; purpose.
        lda     VOENT
        ora     #VOE_LEFT
        sta     VOENT
        lda     #BANKBOOT
        jmp     VOGOTO
VOSTA1: clc
        adc     #$10            ; +1 in the high nibble, low nibble untouched
        sta     VOERR
        rts

