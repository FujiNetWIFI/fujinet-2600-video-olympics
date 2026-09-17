; voloc0.inc -- the local-play mirror, in hole A beside the gate.

; ---------------------------------------------------------------------------
; VOLOC0 -- not in a match: the console drives itself, exactly as it always did.
;
; A CARTRIDGE WITH NO SERVER IS STILL A VIDEO OLYMPICS CARTRIDGE (4.13). Any
; session failure -- no FujiNet, no relay, a refused connection, a protocol
; mismatch -- hands over cold and not networked, and lands here.
;
; This path is byte-for-byte the behaviour `make det` compares against stock,
; and it is why the shim went in before the network did: a build that played
; differently here would make every later difference ambiguous.
;
; The paddles are copied rather than read, for the same reason VOCAP copies
; them: $BB-$BE hold what the kernel filtered at the end of the previous frame,
; which is exactly what $F1B4 would have read this frame if it had never been
; patched.
VOLOC0:
        lda     SWCHA
        sta     VOSWA
        lda     SWCHB
        sta     VOSWB
        ldx     #3
VOLOCP: lda     $BB,x
        sta     VOPAD,x
        dex
        bpl     VOLOCP
        rts

