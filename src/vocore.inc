; vocore.inc -- the 6502 side of the FujiNet cartridge mailbox: the code that
; is assembled ONCE, into the fixed tail, and reached by every bank.
;
; Copied unchanged from fujinet-lobby/atari-2600/src/lbcore.inc. The family's
; rule is that this file is copied, never re-derived: every line of it was
; learned somewhere else in the family and the comments are the record of
; where. Only the per-row-colour note at the end is Lobby-specific.
;
; Equates are in fujinet.inc, which must be included BEFORE the ORG. This file
; is code and must be included inside it.
;
; A register write is TWO stores on this console, not one:
;
;       lda #value
;       sta FNRSEL+reg          ; arm; this store's data is ignored
;       sta FNCMT               ; commit: register `reg` = value
;
; That is not ceremony. The cartridge port has no R/W line, so the cart cannot
; tell a read of a write port from a write to it -- a stray LDA into page $1D
; is decoded as an access carrying whatever the floating bus held. Arming and
; committing separately means ONE stray mutates nothing: an arm carries no
; value, and a commit with nothing armed is discarded.
;
; The other rule that will bite: NEVER use a read-modify-write instruction on
; the control or TX pages, and never an indirect store anywhere. INC/DEC/ASL/
; LSR/ROL/ROR absolute are three bus cycles at one address -- read, write old,
; write new -- and the cart's "last value before the address changed" sampling
; has no defined answer in that burst; STA (zp),Y performs a read at the
; address it is about to write. Only STA/STX/STY, and only direct.
; tools/checkrom.py fails the build on the rest.
;
; THE TAIL IS 220 BYTES AND THIS IS MOST OF THEM. The sibling ports report
; single-digit headroom, so nothing goes in here that a bank could carry
; instead -- build.sh prints what mktail.py measured on every build.
;
; Combat keeps the text primitives OUT of the tail: only the boot bank draws
; text, and the ~48 bytes they cost are worth more as headroom for a leaner
; per-frame network step that every bank can reach. They are here, guarded,
; so the boot bank can include the same file.

; -----------------------------------------------------------------------
; FNRW -- register X = A. The whole transport is built on this.
;
; `sta FNRSEL,x` is the documented-safe indexed form: the base low byte is $00
; so the index can never carry, which means the dummy read STA abs,X always
; performs lands on the SAME address as the write -- one parked access, not
; two, and an arm ignores its data anyway.
FNRW:   sta     FNRSEL,x
        sta     FNCMT
        rts

; FNARM -- open the decode gate.
; Two stores, in order, carrying two specific values. A 7800's BIOS probes
; cartridge space on startup and cannot produce that, which is the point.
FNARM:  lda     #FNAM1
        sta     FNRSEL+FH_ARM1
        lda     #FNAM2
        sta     FNRSEL+FH_ARM2
        rts

; FNCHK -- Z set if a FujiNet cartridge is answering.
FNCHK:  lda     FNMAG0
        cmp     #'F'
        bne     FNCHK1
        lda     FNMAG1
        cmp     #'N'
FNCHK1: rts

; FNBEG -- begin a transaction described by FNDEV / FNCMD / FNNPR.
; Rewinds the TX stream last, so parameters may be appended immediately.
FNBEG:  lda     #16             ; ~9s; longer than the cart's own 5s budget so
        sta     FNTMO           ;   a real timeout is reported as ITS error
        lda     FNDEV
        ldx     #FR_DEV
        jsr     FNRW
        lda     FNCMD
        ldx     #FR_CMD
        jsr     FNRW
        lda     FNNPR
        ldx     #FR_NPAR
        jsr     FNRW
        lda     #0
        ldx     #FR_DRST
        jmp     FNRW

; FNPB -- append a one-byte parameter with value A.
; The stream is NPARAM x { size byte, then that many value bytes } then the
; payload, identical to every sibling port.
FNPB:   pha
        lda     #1
        sta     FNTX
        pla
        sta     FNTX
        rts

; FNPW -- append a two-byte little-endian parameter. A = low, X = high.
FNPW:   pha
        lda     #2
        sta     FNTX
        pla
        sta     FNTX
        txa
        sta     FNTX
        rts

; FNGO -- commit and wait. Returns A = FNE* (0 = the transaction completed).
;
; THE RULE THAT EVERY PORT IN THIS FAMILY LEARNED THE HARD WAY: the next
; sequence number comes from the CART'S OWN persisted ACKSEQ + 1, never from a
; counter in RAM. A console RESET restarts the client and re-zeroes its
; variables but does NOT reset the cartridge -- there is no reset line on the
; connector and the RESET switch is a bit in a RIOT register the cart never
; sees. A program-local counter would restart at 1, collide with a sequence
; already answered, and the transaction would be dropped in silence.
FNGO:   lda     FNACKS
        clc
        adc     #1
        bne     FNGO1
        lda     #1              ; 0 is reserved as "never used"
FNGO1:  sta     FNSEQ
        ldx     #FR_SEQ
        jsr     FNRW            ; launching it is this single commit
        lda     FNTMO
        sta     FNCNT
FNGO2:  ldy     #0
FNGO3:  ldx     #0
        ; ACKSEQ is published LAST, after the whole reply is in place, so a
        ; match means everything behind it is already readable.
FNGO4:  lda     FNACKS
        cmp     FNSEQ
        beq     FNGO5
        dex
        bne     FNGO4
        dey
        bne     FNGO3
        dec     FNCNT
        bne     FNGO2
        lda     #FNEWAIT
        rts
FNGO5:  lda     FNERR
        rts

; FNACK -- 0 on success, FNENAK if the server said no.
; Call after FNGO returned 0.
FNACK:  lda     FNRCMD
        cmp     #$06
        beq     FNACK1
        lda     #FNENAK
        rts
FNACK1: lda     #FNEOK
        rts

; -----------------------------------------------------------------------
; Text, composed by the cartridge. Three one-shot stores, and the client
; never touches a font: the RP2040 keeps the 3x5 glyphs and renders ASCII
; into the exact shape the 48-pixel player kernel wants.

        IFDEF VOHASTXT

; FNROWA -- begin composing text row A. The cursor resets to column 0.
FNROWA: sta     FNRSEL+FH_TROW
        rts

; FNCHR -- append character A to the row being composed. Saturates at column
; 12, so a caller may stream a string longer than the screen and simply let
; the cartridge drop the tail.
FNCHR:  sta     FNRSEL+FH_TCHR
        rts

; FNENDR -- render the composed row into the planes.
;
; The attribute byte is zero, as it is in every client in the family. Combat's
; boot bank draws plain white rows and carries no per-row colour table, so
; there is nothing to put here.
FNENDR: lda     #0
        sta     FNRSEL+FH_TEND
        rts

        ENDIF
