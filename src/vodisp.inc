; vodisp.inc -- the 48-pixel text display, for the boot bank's screens.
;
; Adapted from fn-2600/pico/atari-2600/testrom/fujidisp.inc, which every client
; in this family shares. The cartridge composes the glyphs -- the RP2040 renders
; ASCII into the exact shape a 48-pixel player kernel wants and publishes six
; 128-byte planes -- and this does nothing but stream them into GRP0/GRP1 on a
; cycle-exact schedule.
;
; COPIED, NOT RE-DERIVED. The timing constants below are the coordinates of a
; ONE PIXEL WIDE window, found by byte-comparing the raster against the renderer
; rather than by eye, and every wrong setting still looks like text. The one
; change is structural: fujidisp's DLOOP draws frames forever, and the session
; has to poll a socket between them, so this draws ONE frame and returns.

; ---------------- kernel timing constants ----------------
; The 48-pixel block is six player copies 2.67 cycles apart and the kernel's
; stores are 3 cycles, so where the block sits decides whether each graphics
; update lands between copies or inside one.
CDNOP   EQU     16              ; NOPs before RESP0 (2 cycles each)...
CDR3    EQU     1               ; ...plus this, so RESP0 lands on cycle 38
CDHM0   EQU     15              ; HMP0 = $F0: move P0 right one pixel...
CDHM1   EQU     0               ; ...and leave P1, closing the gap to eight
CDVDEL  EQU     1               ; vertical delay on both players

; FN_T_PLANE(0..5) from fuji_mailbox.h.
CDTP0   EQU     $1800
CDTP1   EQU     $1880
CDTP2   EQU     $1900
CDTP3   EQU     $1980
CDTP4   EQU     $1A00
CDTP5   EQU     $1A80

CDLINES EQU     FNTROW*FNTCLH   ; 21 rows of 6 scanlines = 126

; Combat measures 263 lines VSYNC-to-VSYNC, and the boot bank has to match it:
; the peer-left screen appears in the MIDDLE of a session, long after
; `make frames` stops excusing the handover, and a screen of a different length
; there would be a real finding about a real change of frame length.
;
; 30, not 31. The arithmetic says 3 + 37 + 33 + 126 + 33 + 30 = 262 and the
; measurement says 263, because a frame is measured from one VSYNC WRITE to the
; next and that write sits inside a line of its own. The measurement is the one
; that matters; `make frames` is what settled it.
CDVBL   EQU     37
CDOS    EQU     30
CDPAD   EQU     (192-CDLINES)/2 ; 33 above and below

; Two cells, both free in this bank: the boot bank runs before a match and the
; lockstep's are not live yet.
CDSAVSP EQU     VOADV
CDPAD3  EQU     VOTMP
CDPTR   EQU     FNPTRL          ; the string being rendered

; ---------------------------------------------------------------------------
; CDINIT -- TIA setup and player positioning. Once.
CDINIT:
; Two players, three copies each, eight pixels apart: P0 draws groups 0, 2 and
; 4 and P1 groups 1, 3 and 5, interleaving to 48 contiguous pixels. VDELP is
; what makes the six-write sequence land in the right copies.
        lda     #$03
        sta     NUSIZ0
        sta     NUSIZ1
        lda     #CDVDEL
        sta     VDELP0
        sta     VDELP1
        lda     #$0E            ; white
        sta     COLUP0
        sta     COLUP1
        lda     #$00
        sta     COLUBK
        sta     COLUPF
        sta     PF0
        sta     PF1
        sta     PF2
        sta     GRP0
        sta     GRP1
        sta     ENAM0
        sta     ENAM1
        sta     ENABL

; Positioning, transcribed from batari Basic's proven kernel rather than
; re-derived: RESP0 strobed at cycle 43, RESP1 three cycles later, the motion
; nibbles pulling them to eight apart, and an EARLY HMOVE -- WSYNC then strobe.
; A late one does not apply the nibbles cleanly, and this kernel needs
; one-pixel steps.
        sta     WSYNC
        REPT    CDNOP
        nop
        ENDM
        IF      CDR3
        sta     CDPAD3          ; 3 cycles, so odd delays are reachable
        ENDIF
        sta     RESP0
        sta     RESP1
        lda     #CDHM0*16
        sta     HMP0
        lda     #(CDHM1*16)&$FF
        sta     HMP1
        sta     WSYNC
        sta     HMOVE
        rts

; ---------------------------------------------------------------------------
; CDFRAME -- draw ONE frame of text, then return.
CDFRAME:
        lda     #2
        sta     VSYNC
        sta     WSYNC
        sta     WSYNC
        sta     WSYNC
        lda     #0
        sta     VSYNC

        lda     #2
        sta     VBLANK
        ldx     #CDVBL
CDVBLP: sta     WSYNC
        dex
        bne     CDVBLP
        lda     #0
        sta     VBLANK

        ldx     #CDPAD
CDPADA: sta     WSYNC
        dex
        bne     CDPADA

; ---------------- the kernel ----------------
; 53 cycles of the 76 available. The stack pointer is a spare register here and
; that is not a flourish: the six copies are 2.67 cycles apart while a
; zero-page store is 3, so the four late writes drift a full cycle across the
; block. Recovering a byte from zero page costs 3 cycles and `tsx` costs 2, and
; that one cycle is the difference between a schedule that closes and one that
; does not. Nothing here may touch the stack -- no JSR -- between the txs and
; the restore.
        tsx
        stx     CDSAVSP
        ldy     #0
CDKERN: sta     WSYNC
        lda     CDTP0,y         ; 4
        sta     GRP0            ; 3  ->  7   new GRP0 = group 0
        lda     CDTP1,y         ; 4  -> 11
        sta     GRP1            ; 3  -> 14   displays group 0
        lda     CDTP2,y         ; 4  -> 18
        sta     GRP0            ; 3  -> 21   displays group 1
        lda     CDTP4,y         ; 4  -> 25
        tax                     ; 2  -> 27
        txs                     ; 2  -> 29   park group 4 in S
        lda     CDTP3,y         ; 4  -> 33
        tax                     ; 2  -> 35
        lda     CDTP5,y         ; 4  -> 39
        stx     GRP1            ; 3  -> 42   displays group 2
        tsx                     ; 2  -> 44   recover group 4
        stx     GRP0            ; 3  -> 47   displays group 3
        sta     GRP1            ; 3  -> 50   displays group 4
        sty     GRP0            ; 3  -> 53   displays group 5; value unused
        iny
        cpy     #CDLINES
        bne     CDKERN
        ldx     CDSAVSP
        txs

; Blank both players. THREE writes, not two: with VDELP set, a write to GRP0
; only reloads the delayed GRP1 and vice versa. Toggling VDELP off and back on
; instead -- the obvious-looking version -- restores the stale delayed
; registers, and the padding lines above the text redraw the last kernel line.
        lda     #0
        sta     GRP0
        sta     GRP1
        sta     GRP0

        ldx     #CDPAD
CDPADB: sta     WSYNC
        dex
        bne     CDPADB

        lda     #2
        sta     VBLANK
        ldx     #CDOS
CDOSLP: sta     WSYNC
        dex
        bne     CDOSLP
        rts

; ---------------------------------------------------------------------------
; CDSAY -- render the NUL-terminated string at CDPTR into text row A.
;
; The cartridge saturates at twelve columns, so a caller may stream a longer
; string and simply let the tail be dropped.
CDSAY:
        jsr     FNROWA
        ldy     #0
CDSAY1: lda     (CDPTR),y       ; an indirect LOAD is legal; it is indirect
        beq     CDSAY2          ;   STORES that are banned outright, because
        jsr     FNCHR           ;   they read the address they write
        iny
        bne     CDSAY1
CDSAY2: jmp     FNENDR

; CDBLANK -- clear every row. A screen change has to erase what was there: the
; cartridge keeps the planes, so a shorter message leaves the tail of a longer
; one behind it.
CDBLANK:
        ldx     #FNTROW-1
CDBL1:  txa
        jsr     FNROWA
        jsr     FNENDR
        dex
        bpl     CDBL1
        rts
