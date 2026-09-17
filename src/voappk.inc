; voappk.inc -- the two appkeys, read into cartridge path buffers.
;
; Nothing here touches console RAM, and it cannot: a username is eight bytes and
; a server URL sixty-four, against twenty-six free in the whole machine. The
; cartridge keeps four 256-byte path buffers for exactly this, and the reply
; window the appkey arrives in is repainted by the next transaction anyway -- so
; the data goes straight from one to the other and is emitted from the buffer
; when the request that wants it is being built.
;
; Every failure here is soft. A console with no SD card, a Lobby that never
; wrote the key, a name somebody typed in lower case: all of them fall back to
; the string baked in at build time, because a cartridge that refused to play
; because it could not read a preference would be a worse cartridge.

; ---------------------------------------------------------------------------
; CSAKOPEN -- open appkey (creator 1, app 1, key A) for reading.
CSAKOPEN:
        sta     CSDLY           ; the key, parked where FNBEG cannot reach it
        lda     #FNDEVF
        sta     FNDEV
        lda     #FCAKOPN
        sta     FNCMD
        lda     #0
        sta     FNNPR
        jsr     FNBEG
        lda     #AKCREAT
        sta     FNTX
        lda     #0
        sta     FNTX            ; creator, high
        lda     #AKAPP
        sta     FNTX
        lda     CSDLY
        sta     FNTX            ; the key
        lda     #AKMRD
        sta     FNTX
        lda     #0
        sta     FNTX            ; THE RESERVED SIXTH BYTE. Five leaves the
                                ;   firmware waiting for one that never comes,
                                ;   and that reads back as a timeout.
        jmp     CSAKGO

; CSAKREAD -- read it. No parameters, no payload; the value comes back in the
; reply window as a two-byte little-endian length followed by the data.
CSAKREAD:
        lda     #FNDEVF
        sta     FNDEV
        lda     #FCAKRD
        sta     FNCMD
        lda     #0
        sta     FNNPR
        jsr     FNBEG
        jmp     CSAKGO

; CSAKCLOSE -- and a close that failed is not worth a screen.
CSAKCLOSE:
        lda     #FNDEVF
        sta     FNDEV
        lda     #FCAKCLS
        sta     FNCMD
        lda     #0
        sta     FNNPR
        jsr     FNBEG
        jsr     CSAKGO
        lda     #0
        rts

; CSAKGO -- launch and check. The tail's blocking FNGO, not the display-paced
; twin: this runs before there is anything on screen worth keeping.
CSAKGO: jsr     FNGO
        cmp     #FNEOK
        bne     CSAKG9
        jsr     FNACK
CSAKG9: rts

; ---------------------------------------------------------------------------
; CSAKGET -- read appkey A into path buffer X. Returns A = 0 on success.
;
; On any failure the buffer is left EMPTY rather than half-filled, so a caller
; can tell "there was no key" from "here is a key" by its length alone.
CSAKGET:
        stx     FNPCNT          ; the buffer selector
        pha
        lda     FNPCNT          ; select and empty it first: a stale buffer is
        sta     FNRSEL+FH_PATHO ;   indistinguishable from a fresh one
        lda     #FP_RST
        sta     FNRSEL+FH_PATHO
        pla
        jsr     CSAKOPEN
        bne     CSAKG8
        jsr     CSAKREAD
        bne     CSAKG7
; The length. Anything over 255 is not a name and not a URL either -- the
; cartridge's buffer is 256 and the Lobby's own URL field is 64.
        lda     FNRPLY+1
        bne     CSAKG6
        lda     FNRPLY+0
        beq     CSAKG6          ; an empty key is a key that was never written
        sta     CSDLY
        ldx     #0
CSAKG1: lda     FNRPLY+2,x
        beq     CSAKG2          ; a NUL ends it early, and that is fine
        sta     FNRSEL+FH_PATHC
        inx
        cpx     CSDLY
        bne     CSAKG1
CSAKG2: jsr     CSAKCLOSE
        lda     #0
        rts
; Any failure past the open still has to close, or the next open finds the
; device busy and every appkey after it fails too.
CSAKG6: jsr     CSAKCLOSE
        lda     #$FF
        rts
CSAKG7: jsr     CSAKCLOSE
CSAKG8: lda     #$FF
        rts

; ---------------------------------------------------------------------------
; CSPLEN -- A = the selected path buffer's length, or 0.
;
; FN_B_PATHLEN is little-endian and republished whenever the buffer changes.
; Anything over 255 cannot be a name, and for a URL the high byte is the
; caller's problem -- but neither of ours is ever that long.
CSPLEN: lda     FNPLNH
        bne     CSPL9
        lda     FNPLNL
        rts
CSPL9:  lda     #0
        rts
