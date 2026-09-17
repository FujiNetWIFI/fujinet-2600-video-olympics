; vocrc.inc -- the sim checksum, the desync repair, and the peer-left exit.

; ---------------------------------------------------------------------------
; VOCRC -- one byte over the authoritative simulation state.
;
; Rotate-and-add, not a plain sum: a plain sum cannot see two bytes that
; swapped, and two players that swapped is exactly the failure this is for. The
; carry out of each ASL feeds the ADC, which is what makes it a rotate rather
; than a shift and stops high bits falling off the end.
;
; COMPUTED IN THE KERNEL BANK, ONCE A FRAME, AND READ AT THE TICK BOUNDARY.
;
; It is the only piece of the shim that is not in the game bank, and it is
; there for room -- the game bank's three holes come to 360 bytes and the shim
; wanted 379. It is also SAFE, which is why this is the piece that moved:
;
;   the kernel bank runs at the END of frame N and the boundary is at the START
;   of frame N+1, and NOTHING BETWEEN THEM TOUCHES A CELL THIS COVERS. The
;   display kernel writes $84/$85 (the raw paddle), $BB-$BE (the filtered one)
;   and $81 (the saved stack pointer), and none of those is in the list below.
;
; So VOCRCK at the end of frame N is bit-for-bit the checksum the boundary
; would have computed, and the invariant the desync check rests on is intact.
;
; SAMPLED AT A TICK BOUNDARY AND NOWHERE ELSE. The game logic runs on every
; frame and only the input changes at the tick rate, so the state moves VOK
; times a tick; a checksum taken wherever the transport happened to get to is a
; different quantity on each machine every single time, and the symptom is one
; console reporting a value that varies wildly while the other reports a
; constant.
;
; WHAT IT COVERS IS THE POINT. 4.21: the checksum must cover every cell that
; SELECTS behaviour, not only the cells behaviour writes. Two consoles on
; different variations draw visibly different games while agreeing perfectly on
; a checksum of positions alone, because nothing has yet happened to move the
; players apart -- so $96 goes in first.
;
;   $96        the variation, 0-49
;   $88        the frame counter, which paces the serve
;   $8D $8E    the two scores
;   $B2-$B6    the sprite Y positions and the ball
;
; AND NOT $BB-$BE. Those are the filtered LOCAL paddle positions -- what this
; console's own controller is doing, written by the display kernel every frame.
; They are input, not simulation, and they differ between two consoles by
; design: that is the whole point of there being two players. Checksumming them
; would report a mismatch on every tick of a perfectly synchronised pair. What
; IS agreed is VOPAD, which both consoles compute from the same two wire bytes
; -- and that is covered by the positions it drives.
;
; THE GAP TO BE HONEST ABOUT: the ball's HORIZONTAL position is not in RAM at
; all. It lives in the TIA and is applied incrementally by HMOVE -- $F65F
; writes HMM0 -- so no checksum can see it, exactly as no checksum could see
; Combat's tank X. A ball-X-only divergence is invisible until it moves
; something that IS covered. That is the same fact that rules out a state-push
; repair and the reason VORSY restarts the round instead.
VOCRC:
        lda     $96             ; the variation -- FIRST, and 4.21 is why
        asl     a
        adc     $88             ; the frame counter, which paces the serve
        ldx     #1
VOCRC1: asl     a
        adc     $8D,x           ; the two scores
        dex
        bpl     VOCRC1
        ldx     #4
VOCRC2: asl     a
        adc     $B2,x           ; the Y positions and the ball
        dex
        bpl     VOCRC2

        sta     VOCRCK
        rts
