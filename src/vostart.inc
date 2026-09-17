; vostart.inc -- the relocated cold start.
;
; The stock ROM opens with this, at $F000:
;
;       SEI / CLD / LDX #$FF / TXS / INX / TXA
; $F007 STA $8D,X / INX / BNE $F007
;
; `STA $8D,X` WRAPS INSIDE PAGE ZERO, so running X from 0 to 255 covers all of
; $00-$FF -- the whole 128 bytes of RIOT RAM *and* a strobe of every TIA
; register, in one nine-byte loop. It is the same deliberate use of the wrap
; Combat documents in 4.11, and it is worth admiring before breaking it.
;
; It cannot stay at $F000 for two reasons, one per half of what it does:
;
;   * $1000 is where a bank switch lands, and this bank is entered twice a
;     frame. The twelve bytes at $1000 have to be the entry dispatcher.
;
;   * The clear would wipe every netcode cell the boot bank has just set up --
;     4.12's sibling trap, whose symptom is a match that pairs, transports
;     cleanly, and never advances a tick, because the game has quietly fallen
;     back to reading its own controllers.
;
; So it moves here and skips the netcode's block. Everything else it used to
; clear, it still clears, including the stack and the whole TIA.
;
; ONE Combat problem does NOT apply here, and it is worth saying so: nothing in
; this ROM is left uninitialised, so there is no equivalent of 4.3's HIRES
; power-on garbage feeding the collision latches on frame one.

; $8D + X, wrapping. The netcode owns $CD-$F9, so the run to skip is
; X = $CD-$8D = $40 through X = $F9-$8D = $6C.
; THE ORDER MATTERS, AND IT IS NOT THE ORDER STOCK USED.
;
; Stock does the whole thing in one nine-byte loop, relying on `STA $8D,X`
; wrapping inside page zero to sweep $00-$FF -- the 128 bytes of RAM and, at
; $0000-$007F where A7 is clear, every TIA register. Beautiful, and it cannot
; be kept: it would wipe the netcode's cells.
;
; Splitting it exposed something the single loop hid. The sweep STROBES CXCLR
; on its way past, because CXCLR is a TIA register like any other -- and
; emu/det.lua counts frames at CXCLR, skipping any where the frame counter at
; $88 reads zero. Stock's order clears $88 near the very end, so the strobe
; lands while $88 still holds power-on garbage and the harness counts a frame
; that never happened. The banked build then ran one frame ahead of stock for
; ever after, and `make det` reported a divergence at frame 1 whose actual
; content was stock's frame 1 arriving a row late.
;
; So: RAM first, TIA last. By the time CXCLR is strobed the frame counter is
; already zero and the phantom frame is skipped on both builds.
; VOSTART is the whole cold path; VOSCLR is the part the SESSION also needs,
; because a match hands over with the same RAM and the same primed shadows and
; differs only in what it then puts in VOENT and the rings.
VOSTART:
        sei
        cld
        jsr     VOSCLR
        lda     #BANKGAME
        jmp     VOGOTO

;
; VOSCLR TOUCHES NEITHER THE STACK POINTER NOR THE STACK, and both halves of
; that cost a debugging session.
;
; It used to open `ldx #$FF / txs`, copied from the stock START it replaces --
; which destroys its own return address, so the RTS below returned into
; whatever was at the top of page one. Nothing needs it to: the tail's cold
; stub sets SP before it switches here and VOGOTO sets it again on every switch
; out, so the stack is already right at both ends.
;
; Then it still cleared $FA-$FF, which is the same mistake wearing the other
; hat. THE 6507 PUTS THE STACK AT $0180-$01FF AND THAT MIRRORS ONTO $80-$FF, so
; those six bytes ARE the stack -- and with SP at $FF the JSR that reached here
; put its return address in the top two of them. Clearing them is clearing the
; way home. The console ran off into page zero and sat at $0032 for ever.
;
; So the stack is left alone. Stock zeroes it and nothing reads it: it is the
; stack, its contents are whatever the last call left, and `make det` does not
; look at it because there is nothing there to agree about.
VOSCLR:
        lda     #0

; RAM $80-$CC. $CD-$F9 is the netcode's and is skipped -- 4.12's sibling trap:
; a clear that ran over it would wipe what the boot bank has just set up, and
; the match would pair, transport cleanly and never advance a tick because the
; game had quietly fallen back to reading its own controllers.
        ldx     #$4C            ; $80+$4C = $CC, counting down to $80
VOSTR1: sta     $80,x
        dex
        bpl     VOSTR1

; $FA-$FF: the stack, which is the top of zero page on this console because the
; 6507 puts the stack at $0180-$01FF and that mirrors onto $80-$FF.
; And the TIA, every register, LAST.
        ldx     #$7F
VOSTR3: sta     $00,x
        dex
        bpl     VOSTR3

; PRIME THE GATE. VOADV is what VOGATE reads at the head of the timed band to
; decide whether the game logic runs, and the shim that sets it runs at the END
; of a frame, in the kernel bank -- so the FIRST frame has no previous decision
; to read and would find the zero the clear just left. VOGATE would jump
; straight to the spin and the build would stall its own first frame.
;
; The symptom was not a stall, which is the point. `make det` reported a
; divergence at frame 1 whose content was stock's frame 1 arriving one row
; late, for ever after -- one lost frame at the start looks exactly like a
; simulation that disagrees.
        lda     #$FF
        sta     VOADV

; PRIME THE SHADOWS BEFORE THE GAME'S FIRST READ.
;
; The mirror runs at the END of a frame, in the kernel bank, so on the very
; first frame there has not been one yet and VOSWA/VOSWB/VOPAD would still be
; whatever the clear left. VOSWB reading zero is not a neutral value: SWCHB is
; ACTIVE LOW, so zero is every console switch held down.
;
; The game notices immediately. $F0D3 reads SWCHB into $8F and $F0D7 does
; `EOR $8F / AND #$02` to find switches that CHANGED -- so a shadow that starts
; at zero and becomes $3F on the second frame is a phantom press on the first,
; which takes the new-game path, resets the frame counter at $F0DF and puts the
; two builds on different sims from frame two. It cost a `make det` run to see
; and one line to fix.
;
; VOPAD is deliberately NOT primed from a constant: it is copied from $BB-$BE,
; which the clear above has just zeroed, so it gets exactly what stock's first
; frame would read there. A paddle position is not active low and zero is a
; legitimate end of travel -- see VOP_IDLE in vodefs.inc.
        lda     SWCHA
        sta     VOSWA
        lda     SWCHB
        sta     VOSWB
        ldx     #3
VOSTP:  lda     $BB,x           ; zeroed by the clear above, which is exactly
        sta     VOPAD,x         ;   what stock's first frame reads there
        dex
        bpl     VOSTP

        rts
