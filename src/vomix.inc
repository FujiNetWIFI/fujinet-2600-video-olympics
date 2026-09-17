; vomix.inc -- two consoles' wire bytes into the cells the game reads.
; Reached by JMP from the end of voinput.inc, because no single hole in this
; bank is big enough for both.

; ---------------------------------------------------------------------------
; VOMIX -- two consoles' wire bytes into the six cells the game reads.
;
; Every bit keeps the hardware's own sense on the wire, so this is masks and
; shifts and never an inversion -- which matters more than it sounds, because
; an inversion applied on one console and not the other is a desync that only
; shows when somebody presses something.
VOMIX:
; THE PADDLES. Straight across, into the slots the mapping says: host to player
; 0, guest to player 2.
        lda     VOW2
        sta     VOPAD+0
        lda     VOW3
        sta     VOPAD+2
; Slots 1 and 3 are the second paddles of each port -- the four-player games.
; The wire carries them already; the rings do not yet. Until they do they hold
; the agreed constant VOSTART put there once, and nothing writes them again:
; re-storing a constant every boundary is eight bytes this bank does not have.

; SWCHA -- the four paddle triggers, and nothing else. $F0A1 masks it with the
; table at $F6B4 (`80 40 08 04`), so bits 7 and 6 are the LEFT port's pair and
; bits 3 and 2 the RIGHT port's. Each console sends its own left-port pair in
; bits 7 and 6, so the guest's shift down by four and land on players 2 and 3.
        lda     VOW0
        and     #SA_L
        sta     VOTMP
        lda     VOW1
        and     #SA_L
        lsr     a
        lsr     a
        lsr     a
        lsr     a                ; $C0 -> $0C, the right port's pair
        ora     VOTMP
        ora     #$33             ; the bits that mask never looks at read high
        sta     VOSWA

; SWCHB. RESET and SELECT are ANDed: both bits are active low, so the AND means
; EITHER PLAYER MAY PRESS THEM -- which is the couch semantics, preserved.
; There is no host authority here and none is needed: every bit is a pure
; function of the two wire bytes.
        lda     VOW0
        and     VOW1
        lsr     a                ; IW_RST is bit 4 and SWCHB's reset is bit 0,
        lsr     a                ;   IW_SEL is bit 5 and select is bit 1
        lsr     a
        lsr     a
        and     #$03
        sta     VOTMP

; B&W STAYS LOCAL, and it is safe: the B&W bit reaches only the colour cells
; through the mask at $F26D, and no colour cell reaches physics. Each player
; sees their own set in whichever they chose.
        lda     SWCHB
        and     #$08
        ora     VOTMP
        ora     #$34             ; bits 2, 4 and 5 read high on a real console
        sta     VOTMP

; The difficulty switches, uncrossed. $F2D7 tests SWCHB with BIT: bit 7 through
; N and bit 6 through V, so bit 6 is the left player's and bit 7 the right's.
; The wire carries each console's own in IW_DIFF, bit 3.
        lda     VOW0
        and     #IW_DIFF
        asl     a
        asl     a
        asl     a                ; $08 -> $40, the LEFT difficulty
        ora     VOTMP
        sta     VOTMP
        lda     VOW1
        and     #IW_DIFF
        asl     a
        asl     a
        asl     a
        asl     a                ; $08 -> $80, the RIGHT difficulty
        ora     VOTMP
        sta     VOSWB

; Mark the tick as run -- the next boundary does the increment, immediately
; before it samples, so the offset between a record's stamp and its checksum
; cannot depend on the local stall pattern -- and bump the phase, because this
; frame's logic will run.
        lda     VOENT
        ora     #VOE_RAN
        sta     VOENT
        jmp     VOPHI
