; probe.asm -- M1: how many video frames does one mailbox transaction cost?
;
; Every latency number in this port hangs off that one figure, and PORTING.md
; §2 is emphatic that it has to be MEASURED. The Intellivision family believed
; a documented 30 Hz tick for years; it was 10, and every latency estimate was
; three times too optimistic. So this runs before any Combat work at all.
;
; A flat 4K client: code in $1000-$17FF, the cartridge's mailbox above it, and
; the claim and vectors in the fixed half. No banking -- there is nothing here
; big enough to need it.
;
; What it does: opens N:TCP:// to tools/latency_probe_server.py, then forever
; WRITE one byte, STATUS until a byte is waiting, READ it back. Each step is
; paced ONE FRAME PER LOOK at the acknowledgement, which is how the real client
; will have to work, so the frame counts this reports are the real currency.
;
; It publishes its progress by storing a step id into PSTEP and the frames that
; step took into VOFCNT. emu/latency.lua taps those two cells against a VSYNC
; tap and prints the distribution; nothing has to be decoded off the screen.

        CPU     6502
        INCLUDE "vcs.inc"
        INCLUDE "fujinet.inc"
        INCLUDE "vodefs.inc"

; Step ids, published to PSTEP. Even = a transaction was launched, odd = it came
; back, so a Lua tap sees a launch/complete pair and the gap between them is
; what a transaction costs.
ST_BOOT EQU     0
ST_OPEN EQU     2
ST_OPND EQU     3
ST_WR   EQU     4
ST_WRD  EQU     5
ST_ST   EQU     6
ST_STD  EQU     7
ST_RD   EQU     8
ST_RDD  EQU     9
ST_FAIL EQU     $FF

; Screen colours: the probe has no text, so its whole UI is the background.
CLBOOT  EQU     $00             ; black: starting
CLOPEN  EQU     $84             ; blue:  opening the socket
CLRUN   EQU     $C4             ; green: rounds are completing
CLFAIL  EQU     $44             ; red:   it stopped

; The probe's own cells, in the game-side union: there are no lockstep rings
; here and nothing in this ROM runs at the same time as a match.
PSTEP   EQU     $F7             ; the step id emu/latency.lua taps
PROUND  EQU     $F8             ; 2: rounds completed, little-endian
; THE PROBE'S OWN FRAME COUNTER, and it is local for a reason. Combat's probe
; reached into cbdefs.inc for this and named a cell that was later renamed out
; from under it -- so `make probe`, the gate that is supposed to run FIRST,
; stopped assembling and nobody noticed, because by then nobody was running it.
; Nothing else in this image needs a netcode equate, so it does not take one.
VOFCNT  EQU     $FA             ; video frames the current transaction has taken

        ORG     $1000

; ---------------------------------------------------------------------------
COLD:   sei
        cld
        ldx     #$FF
        txs
        lda     #0
        ldx     #$7F            ; $80+$7F = $FF down to $80+0 = $80. Counting
CLR:    sta     $80,x           ;   UP from $FF would wrap inside page zero --
        dex                     ;   $80,x is zero-page indexed, so X=$FF
        bpl     CLR             ;   addresses $7F, not $17F
        sta     COLUBK
        sta     PSTEP

; Open the decode gate, then ask whether anything answered. A console with no
; FujiNet cartridge in it reads the floating bus here, and going on to talk to
; it would hang in FNGO for nine seconds a transaction, forever.
        jsr     FNARM
        jsr     FNCHK
        beq     HAVEFN
        jmp     FAIL
HAVEFN:

; ---------------------------------------------------------------------------
; The socket. Access mode 12 is READWRITE -- the same value HTTP overloads as
; "GET, pure and unmolested", which is why every other client in this family
; passes 12 without ever opening a socket.
        lda     #ST_OPEN
        sta     PSTEP
        lda     #CLOPEN
        sta     COLUBK
        lda     #NETDEV
        sta     FNDEV
        lda     #NCOPEN
        sta     FNCMD
        lda     #2
        sta     FNNPR
        jsr     FNBEG
        lda     #NMRDWR         ; ACCESS_MODE READWRITE
        jsr     FNPB
        lda     #NTRNONE
        jsr     FNPB
        jsr     PURL
        jsr     PGO
        bne     TOFAIL
        jsr     FNACK
        bne     TOFAIL
        lda     #ST_OPND
        sta     PSTEP
        lda     #CLRUN
        sta     COLUBK

; ---------------------------------------------------------------------------
; The round: WRITE a byte, STATUS until one is waiting, READ it back. Three
; transactions, which is exactly what one lockstep tick will cost.
ROUND:
; -- WRITE --
        lda     #ST_WR
        sta     PSTEP
        lda     #NETDEV
        sta     FNDEV
        lda     #NCWRITE
        sta     FNCMD
        lda     #1
        sta     FNNPR
        jsr     FNBEG
        lda     #1              ; ONE parameter of TWO bytes: the length
        ldx     #0
        jsr     FNPW
        lda     PROUND          ; the payload: something that changes, so a
        sta     FNTX            ;   stale echo cannot read as a fresh one
        jsr     PGO
        bne     TOFAIL
        jsr     FNACK
        bne     TOFAIL
        lda     #ST_WRD
        sta     PSTEP
        jmp     PSTAT           ; OVER the trampoline, and this jump is the
                                ;   whole point of the comment below

; Every step of the round branches here rather than to FAIL: the round is
; longer than a branch reaches. It sits in the middle of the round because
; that is the only place every branch can reach -- which means the straight
; line through the round runs INTO it, and the jump above is what stops that.
; Without it the WRITE step falls through to FAIL carrying A = ST_WRD, and the
; symptom is a failure reported at the right moment with a nonsense error code
; that is really just the step id of the step that had already succeeded.
TOFAIL: jmp     FAIL

; -- STATUS, until the echo is back --
PSTAT:  lda     #ST_ST
        sta     PSTEP
        lda     #NETDEV
        sta     FNDEV
        lda     #NCSTAT
        sta     FNCMD
        lda     #2
        sta     FNNPR
        jsr     FNBEG
        lda     #0
        jsr     FNPB
        lda     #0
        jsr     FNPB
        jsr     PGO
        bne     TOFAIL
        jsr     FNACK
        bne     TOFAIL
        lda     #ST_STD
        sta     PSTEP
; The fourth status byte is nDevStatus_t and SUCCESS is 1, not 0. It is the
; only place a transport error is visible: a dead socket still reports a
; perfectly plausible zero bytes waiting.
        lda     FNRPLY+NSDEVST
        cmp     #1
        bne     TOFAIL
        lda     FNRPLY+NSAVLO   ; one byte is all we sent, so avail >= 1 is the
        ora     FNRPLY+NSAVHI   ;   whole settle condition here
        beq     PSTAT

; -- READ --
        lda     #ST_RD
        sta     PSTEP
        lda     #NETDEV
        sta     FNDEV
        lda     #NCREAD
        sta     FNCMD
        lda     #1
        sta     FNNPR
        jsr     FNBEG
        lda     #1
        ldx     #0
        jsr     FNPW
        jsr     PGO
        bne     TOFAIL
        jsr     FNACK
        bne     TOFAIL
        lda     #ST_RDD
        sta     PSTEP

        inc     PROUND
        bne     ROUND1
        inc     PROUND+1
ROUND1: jmp     ROUND

; ---------------------------------------------------------------------------
FAIL:   sta     VOERR
        lda     #ST_FAIL
        sta     PSTEP
        lda     #CLFAIL
        sta     COLUBK
FAIL1:  jsr     PFRAME
        jmp     FAIL1

; ---------------------------------------------------------------------------
; PGO -- FNGO, paced by the display instead of by a spin loop.
;
; The launch is identical to the tail's and has to be: the next sequence number
; comes from the CART'S OWN persisted ACKSEQ + 1, never a counter in RAM,
; because a console RESET restarts this client without resetting the cartridge.
;
; ONE FRAME BEFORE THE FIRST LOOK. In emulation the cartridge answers inside
; the commit itself, so without it every transaction would report zero frames
; and the measurement would say the transport is free. A frame per step is what
; hardware costs anyway.
;
; VOFCNT counts the frames this transaction took, and is what M1 is for.
PGO:    lda     FNACKS
        clc
        adc     #1
        bne     PGO1
        lda     #1              ; 0 is reserved as "never used"
PGO1:   sta     FNSEQ
        ldx     #FR_SEQ
        jsr     FNRW            ; launching it is this single commit
        lda     #0
        sta     VOFCNT
        lda     FNTMO
        sta     FNCNT
PGO2:   ldy     #PQUANT
PGO3:   jsr     PFRAME
        inc     VOFCNT
        lda     FNACKS
        cmp     FNSEQ
        beq     PGO5
        dey
        bne     PGO3
        dec     FNCNT
        bne     PGO2
        lda     #FNEWAIT
        rts
PGO5:   lda     FNERR
        rts

PQUANT  EQU     34              ; frames per FNTMO quantum, ~0.57s

; ---------------------------------------------------------------------------
; PURL -- stream the devicespec straight into the TX page.
;
; Nothing is assembled in RAM because there is no RAM to assemble it in: this
; console has 128 bytes and a devicespec is longer than the free part of them.
PURL:   ldx     #0
PURL1:  lda     UENDPT,x
        beq     PURL2
        sta     FNTX
        inx
        bne     PURL1
PURL2:  rts

        INCLUDE "endpoint.inc"

; ---------------------------------------------------------------------------
; PFRAME -- one 262-line frame of blank screen.
;
; Counted in WSYNCs rather than timed with the RIOT, because a probe that has
; nothing to draw does not need the timer and a miscounted frame here would be
; visible instantly as a rolling picture.
PFRAME: lda     #2
        sta     VBLANK
        sta     WSYNC
        sta     VSYNC
        sta     WSYNC
        sta     WSYNC
        sta     WSYNC
        lda     #0
        sta     VSYNC
        ldx     #37
PFVB:    sta     WSYNC
        dex
        bne     PFVB
        sta     VBLANK          ; A is 0: picture on
        ldx     #192
PFVIS:    sta     WSYNC
        dex
        bne     PFVIS
        lda     #2
        sta     VBLANK
        ldx     #30
PFOVS:    sta     WSYNC
        dex
        bne     PFOVS
        rts

        INCLUDE "vocore.inc"

; ---------------------------------------------------------------------------
; The claim. An image carrying "FUJI" here promises it is a FujiNet client and
; the mailbox stays live after it boots; a game carries no such promise and the
; cartridge goes dead for the session. build.sh stamps it post-link, because
; its file offset depends on the image size.
        ORG     $1FFC
        DW      COLD
        DW      COLD

        END
