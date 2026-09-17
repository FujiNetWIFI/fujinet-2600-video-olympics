; vosess.inc -- bank 0: the cold start, the socket, and the handshake.
;
; Everything that happens once, before a match, and nothing that happens during
; one. It is the only bank that may use the session half of the zero-page union
; -- FNDEV, FNCMD, FNNPR, FNTMO, FNCNT and the path cursor all share addresses
; with the lockstep rings, and the two are never live at the same time because
; this bank finishes before the game bank starts.
;
; The screen is a colour and not yet a word. The cartridge composes 3x5 glyphs
; into six text planes and the console streams them into GRP0/GRP1 on a
; cycle-exact schedule; that kernel is the next thing this bank wants and it is
; not here yet. A colour per state is honest in the meantime, and it is what the
; latency probe used.

STARTLEN EQU    15              ; len + type + 13 bytes of START payload

; SAY -- render a string into a text row. Two stores and a call; the cartridge
; does the glyphs.
SAY     MACRO   row,str
        lda     #(str)&$FF
        sta     CDPTR
        lda     #(str)>>8
        sta     CDPTR+1
        lda     #row
        jsr     CDSAY
        ENDM

; ---------------------------------------------------------------------------
VOBENT:
        lda     #0
        ldx     #$7F            ; $80+$7F = $FF down to $80. Counting UP from
CSCLR:  sta     $80,x           ;   $FF would wrap inside page zero and write
        dex                     ;   the TIA's whole register file on the way
        bpl     CSCLR           ;   past -- which strobes WSYNC and RESP0.
        jsr     CDINIT
        jsr     CDBLANK
        SAY     1,STCOMB
        SAY     2,STCOMB2
        SAY     4,STNET
        SAY     8,STCONN
        jsr     CDFRAME

        jsr     FNARM
        jsr     FNCHK
        beq     CSOPEN
        jmp     CSLOCAL         ; no cartridge answered: play alone
CSOPEN:

; ---------------------------------------------------------------------------
; The two appkeys, before anything else needs them. Both are soft: a console
; with no SD card, or a Lobby that never wrote the key, simply uses the string
; baked in at build time.
;
;   key 0          the shared FujiNet username, which every game in the family
;                  reads -- that is the whole point of it
;   key VOAPPKEY   the room's URL, which the Lobby wrote on its way out. There
;                  is no HTTP "join" call anywhere in this family: joining a
;                  room is a local write and a reboot, and this is the read.
        lda     #AKNAMEK
        ldx     #PB_NAME
        jsr     CSAKGET
        lda     #VOAPPKEY
        ldx     #PB_URL
        jsr     CSAKGET

; ---------------------------------------------------------------------------
; The socket. Access mode 12 is READWRITE -- the same value HTTP overloads as
; "GET, pure and unmolested", which is why every other client in this family
; passes 12 without ever opening a socket. No 2600 client has opened one before.
        lda     #NETDEV
        sta     FNDEV
        lda     #NCOPEN
        sta     FNCMD
        lda     #2
        sta     FNNPR
        jsr     FNBEG
        lda     #NMRDWR
        jsr     FNPB
        lda     #NTRNONE
        jsr     FNPB
        jsr     CSURL
        jsr     CSGO
        bne     CSOFJ
        jsr     FNACK
        beq     CSHELO
; A trampoline per stretch, because CSLOCAL is further than a branch reaches
; from most of them -- and each one sits where the straight line cannot run
; into it.
CSOFJ:  jmp     CSLOCAL

; ---------------------------------------------------------------------------
; HELLO. The relay pairs the first two consoles that turn up, so this is the
; whole of matchmaking on the console's side: say who you are and wait.
CSHELO: lda     #NETDEV
        sta     FNDEV
        lda     #NCWRITE
        sta     FNCMD
        lda     #1
        sta     FNNPR
        jsr     FNBEG
; How long is the name? The cartridge republishes its path buffer's length
; whenever it changes, so this costs one read rather than a counted copy.
        lda     #PB_NAME
        sta     FNRSEL+FH_PATHO
        jsr     CSPLEN
        cmp     #NAMEMIN
        bcs     CSHN1           ; unsigned, through the carry, always
        lda     #0              ; too short to be a name: use the baked one
CSHN1:  cmp     #NAMEMAX+1
        bcc     CSHN2
        lda     #0
CSHN2:  sta     CSDLY           ; 0 means "fall back"
        bne     CSHN3
        lda     #CSHNLEN        ; the baked name's length
        sta     CSDLY
CSHN3:
;
; THE WRITE'S LENGTH IS THE WHOLE FRAME ON THE WIRE -- the len byte, the type,
; the version and the name -- while the LEN BYTE counts only what follows it.
; They differ by exactly one, and getting them equal sends a frame one byte
; short of what its own header promises, which the relay waits for for ever.
        lda     CSDLY
        clc
        adc     #4
        ldx     #0
        jsr     FNPW
; The frame: len (counting type and payload), type, protocol version, the
; television standard, then the name.
;
; THE STANDARD IS DECLARED, NOT MEASURED. A 2600 generates its own video timing
; and has no register, no interrupt and no external reference that reveals which
; kind of television it is plugged into -- which is why every 2600 game ever
; shipped as separate NTSC, PAL and SECAM images rather than detecting it. So
; the console says what it was BUILT as, and build.sh will not build a SECAM
; one. The relay refuses the declaration too, because a hand-built image could
; say anything.
        lda     CSDLY
        clc
        adc     #3
        sta     FNTX
        lda     #WT_HELO
        sta     FNTX
        lda     #WPROTO
        sta     FNTX
        lda     #VOTVSTD
        sta     FNTX
        lda     #PB_NAME
        sta     FNRSEL+FH_PATHO
        jsr     CSPLEN
        cmp     #NAMEMIN
        bcc     CSHLOB
        cmp     #NAMEMAX+1
        bcs     CSHLOB
        lda     #FP_TXRAW       ; the appkey's name, straight out of the buffer
        sta     FNRSEL+FH_PATHO
        jmp     CSHLO2
CSHLOB: ldx     #0
CSHLO1: lda     CSHNAME,x
        sta     FNTX
        inx
        cpx     #CSHNLEN
        bne     CSHLO1
CSHLO2:
        jsr     CSGO
        bne     CSFAILJ
        jsr     FNACK
        bne     CSFAILJ

        sta     COLUBK

        SAY     8,STWAIT
        SAY     10,STOPP

; ---------------------------------------------------------------------------
; Wait for START.
;
; It is fifteen bytes and this bank knows it, so there is no settle rule here
; and no whole-frame arithmetic: ask STATUS until fifteen are waiting, then read
; exactly fifteen. That is the luxury of not being under time pressure, and it
; is why only the frames the GAME bank can receive are padded to a fixed size.
CSWAIT:
        lda     #NETDEV
        sta     FNDEV
        lda     #NCSTAT
        sta     FNCMD
        lda     #2
        sta     FNNPR
        jsr     FNBEG
        lda     #1
        jsr     FNPB
        lda     #0
        jsr     FNPB
        lda     #1
        jsr     FNPB
        lda     #0
        jsr     FNPB
        jsr     CSGO
        bne     CSFAILJ
        jsr     FNACK
        bne     CSFAILJ
        lda     FNRPLY+NSDEVST  ; SUCCESS is 1, not 0, and this is the only
        cmp     #1              ;   place a transport error is visible
        bne     CSFAILJ
; Unsigned, through the carry. The Channel F port's settle loop used the sign
; bit and a settled 181-byte reply read as short of a one-byte minimum.
        lda     FNRPLY+NSAVHI
        bne     CSREAD
        lda     FNRPLY+NSAVLO
        cmp     #STARTLEN
        bcs     CSREAD
        jsr     CDFRAME         ; keep the picture alive and ask again
        jmp     CSWAIT

CSFAILJ: jmp    CSLOCAL

CSREAD:
        lda     #NETDEV
        sta     FNDEV
        lda     #NCREAD
        sta     FNCMD
        lda     #1
        sta     FNNPR
        jsr     FNBEG
        lda     #STARTLEN
        ldx     #0
        jsr     FNPW
        jsr     CSGO
        bne     CSFAILJ
        jsr     FNACK
        bne     CSFAILJ
        lda     FNRPLY+1        ; the type
        cmp     #WT_START
        bne     CSFAILJ

; ---------------------------------------------------------------------------
; START: role, seed lo, seed hi, delay, variation, then eight bytes of the
; opponent's name. The seed is not used -- Combat has no random number
; generator at all, which is the single luckiest fact about it as a netplay
; target -- and the name has nowhere to be drawn yet.
; THE ROLE GOES STRAIGHT INTO VOENT, not into a scratch cell to be picked up
; later. It used to park in VOTMP -- and VOTMP is $86, which the RAM clear the
; handover runs sweeps on its way past, so the bit was gone by the time
; anything read it. BOTH consoles then came up as the host, neither applied the
; role swap, and each put its OWN paddle in player 0's slot: a pair that agreed
; about every other byte and disagreed about which end of the court each player
; was at. `make rig-play` named $CD and $CF, VOPAD0 and VOPAD2, swapped.
;
; VOENT is $D3, inside the block VOSCLR skips, so a value put here survives the
; clear by construction -- which is the whole reason that block exists.
        lda     FNRPLY+2        ; the role, kept as a bit of VOENT
        and     #1
        asl     a
        asl     a               ; -> VOE_ROLE
        sta     VOENT
; The delay is CHECKED, not obeyed. It sizes the local ring at assembly time,
; and a console that honoured a delay its ring could not hold would overwrite a
; tick it had not used yet -- which is a desync that only appears under load.
        lda     FNRPLY+5
        cmp     #VOD
        bne     CSFAILJ

; The opponent's name is eight bytes at the end of START, still sitting in the
; cartridge's reply window. It is rendered straight out of there -- nothing is
; copied into console RAM, because there is none to copy it into.
        SAY     8,STPLAY
        lda     #(FNRPLY+7)&$FF
        sta     CDPTR
        lda     #(FNRPLY+7)>>8
        sta     CDPTR+1
        lda     #10
        jsr     CDSAY
        jsr     CDFRAME
        jsr     CDFRAME

        jsr     VOSCLR

; Hand over. Everything the game bank needs, and nothing left of the session.
;
; VOSCLR is the same clear and the same shadow-priming the cold path runs -- the
; game has to start from the state its own RAM clear would have left, and this
; console has spent the last few seconds using that RAM for a socket. It skips
; $CD-$F9, so the rings seeded below and the VOENT set after it survive.
; SEED BOTH RINGS WITH IDLE, NOT WITH ZERO -- and with the RIGHT idle, which is
; two different constants now.
;
; A record is stamped VOTICK+d, so the first one anybody sends is for tick d
; and the slots for ticks 0..d-1 are never sent by anyone. Both consoles have
; to agree about ticks neither has heard anything about, so both fill them from
; the same constants.
;
; For the switch byte the constant is not zero: the wire keeps the hardware's
; ACTIVE-LOW sense, so a zero byte reads as every console switch pressed and
; both triggers held. For the paddle it IS zero -- a paddle position is not
; active low, zero is one end of travel, and it is exactly what the RAM clear
; leaves in $BB-$BE at power-on, so the unsent ticks agree with what stock's
; first frame would have read.
        ldx     #(8*VORS)+(4*VORS)-1    ; both rings; they are contiguous
CSZRING:
        txa
        lsr     a                       ; carry = bit 0 of the offset
        lda     #VOP_IDLE               ; even: a paddle byte. LDA leaves C
        bcc     CSZR1
        lda     #IW_IDLE                ; odd: the switches and the triggers
CSZR1:  sta     VORING,x
        dex
        bpl     CSZRING
; VOTICK starts at $FF and VOE_RAN is SET, so the very first boundary advances
; to tick 0 and takes its first checksum and its first capture there -- the same
; path every later tick takes, rather than a special case that would have to be
; kept in step with it.
        lda     #$FF
        sta     VOTICK
        lda     #0
        sta     VOERR
; The remote watermark starts one behind tick zero -- $FE against a VOTICK that
; the first boundary will make 0 -- so the first tick stalls until the peer's
; first record actually arrives, rather than running on a ring nobody has
; written yet. $FF would read as "tick 0 is ready" the moment VOTICK reached 0.
        lda     #$FE
        sta     VORWAT
        lda     #NS_WISS
        sta     VONST
; NETWORKED, and with VOE_RAN set so the first boundary advances $FF -> 0 and
; takes its first checksum and capture through the same path every later tick
; takes. The game itself needs no separate initialisation: this cartridge's
; START is nothing but the RAM clear VOSCLR has just done, and everything else
; the game sets up on its own way through the frame loop.
        lda     VOENT           ; the role bit, put here at START and kept
        ora     #VOE_NET|VOE_RAN ;   across the clear because $D3 is skipped
        sta     VOENT
        lda     #BANKGAME
        jmp     VOGOTO

; ---------------------------------------------------------------------------
; CSLOCAL -- anything went wrong, so just play Video Olympics.
;
; A cartridge with no FujiNet behind it, a relay that is not running, an
; opponent who never turned up: none of them is a reason for this to stop being
; a Video Olympics cartridge. It hands over NOT NETWORKED, which is the
; unpatched game in every respect that matters -- both paddles from this
; console, `make det` comparing byte for byte against the 1977 ROM.
;
; The reason is kept, in the low nibble of VOERR, where the rig reads it: a
; console that silently fell back to playing alone looks exactly like a console
; that never got a turn, and the rig has to be able to tell them apart.
CSLOCAL:
        sta     VOERR
        jsr     CDBLANK
        SAY     1,STCOMB
        SAY     2,STCOMB2
        SAY     8,STNONET
        SAY     10,STALONE
        lda     #90             ; a second and a half, long enough to read
        sta     CSDLY           ; NOT in X: CDFRAME counts scanlines in it
CSLOC1: jsr     CDFRAME
        dec     CSDLY
        bne     CSLOC1
        lda     #0
        sta     VOENT
        jmp     VOSTART         ; clear, prime the shadows, hand over

; ---------------------------------------------------------------------------
; CSGO -- FNGO, paced by the display instead of by a spin loop.
;
; The launch is identical to the tail's and has to be: the next sequence number
; comes from the CARTRIDGE'S OWN persisted ACKSEQ + 1, never a counter in RAM,
; because a console RESET restarts this client without resetting the cartridge.
;
; ONE FRAME BEFORE THE FIRST LOOK. In emulation the cartridge answers inside the
; commit itself, so without it a whole transaction and the next one's setup pile
; into a single frame.
CSQUANT EQU     34              ; frames per FNTMO quantum, ~0.57s

CSGO:   lda     FNACKS
        clc
        adc     #1
        bne     CSGO1
        lda     #1              ; 0 is reserved as "never used"
CSGO1:  sta     FNSEQ
        ldx     #FR_SEQ
        jsr     FNRW
        lda     FNTMO
        sta     FNCNT
CSGO2:  lda     #CSQUANT
        sta     CSDLY           ; NOT in Y, for the same reason
CSGO3:  jsr     CDFRAME
        lda     FNACKS
        cmp     FNSEQ
        beq     CSGO5
        dec     CSDLY
        bne     CSGO3
        dec     FNCNT
        bne     CSGO2
        lda     #FNEWAIT
        rts
CSGO5:  lda     FNERR
        rts

; ---------------------------------------------------------------------------
; CSURL -- stream the devicespec straight into the TX page.
;
; Nothing is assembled in RAM because there is no RAM to assemble it in: this
; console has 128 bytes and a devicespec is longer than the free part of them.
;
; The "N:" is always emitted from here, and the rest is either the Lobby's URL
; out of the cartridge's path buffer or the string baked in at build time. The
; Lobby publishes a bare "TCP://host:port/" -- the device prefix is the
; console's business, not the room's.
CSURL:  lda     #'N'
        sta     FNTX
        lda     #':'
        sta     FNTX
        lda     #PB_URL
        sta     FNRSEL+FH_PATHO
        jsr     CSPLEN
        beq     CSURLB          ; no key: the baked-in endpoint
        lda     #FP_TXRAW       ; emit exactly what is in the buffer
        sta     FNRSEL+FH_PATHO
        rts
CSURLB: ldx     #0
CSURL1: lda     UENDPT,x
        beq     CSURL2
        sta     FNTX
        inx
        bne     CSURL1
CSURL2: rts

        INCLUDE "endpoint.inc"

; ---------------------------------------------------------------------------
; The screens themselves. Twelve columns is the whole width, so every line here
; is twelve characters or fewer and none of them is abbreviated for fun.
STCOMB: DB      "VIDEO",0
STCOMB2: DB     "OLYMPICS",0
STNET:  DB      "NET PLAY",0
STCONN: DB      "CONNECTING",0
STWAIT: DB      "WAITING FOR",0
STOPP:  DB      "AN OPPONENT",0
STPLAY: DB      "PLAYING",0
STVS:   DB      "VS",0
STNONET: DB     "NO NETWORK",0
STALONE: DB     "LOCAL PLAY",0
STLEFT: DB      "OPPONENT",0
STGONE: DB      "HAS LEFT",0
STRST:  DB      "PRESS RESET",0

; ---------------------------------------------------------------------------
; The fallback name, for a console whose username appkey cannot be read.
CSHNAME:
        INCLUDE "playername.inc"

; ---------------------------------------------------------------------------
; CSLEFT -- the peer left the match.
;
; The game bank has no text kernel -- Combat's kernel is in one bank and this
; one's is in another, and neither has room for both -- so the gate switches
; back here to say it. There is nothing to recover: the socket is gone, the
; sim would stall forever, and a screen that says why is worth more than a
; tank that stopped.
CSLEFT:
        jsr     CDINIT
        jsr     CDBLANK
        SAY     1,STCOMB
        SAY     8,STLEFT
        SAY     10,STGONE
        SAY     19,STRST
CSLEFT1: jsr    CDFRAME
        lda     SWCHB           ; the RESET SWITCH, read locally: there is no
        and     #$01            ;   peer left to agree with, and this console
        bne     CSLEFT1         ;   is the only thing that can decide anything
; A cold start, all the way back through the fixed tail's stub, so the
; cartridge's sequence and the path buffers are dealt with exactly as they are
; at power-on rather than approximately.
        lda     #0
        sta     VOENT
        jmp     VOBENT
