; vokern.asm -- bank 2: Video Olympics' display kernel, and the netcode.
;
; The kernel is 211 bytes and its graphics another 188, so this bank has about
; 1650 spare -- which is why the whole network state machine lives here. The
; mailbox at $1D00-$1FFF is fixed and visible from every bank, so the netcode
; does not care which bank it runs in; it should run where the room is.
;
; THE ENTRY IS NOT A FALL-THROUGH. Combat's frame loop fell into its kernel and
; returned with `JMP MLOOP`, so its bank switch was a tail call and nothing had
; to change shape. Video Olympics CALLS its kernel -- `LDY #$07 / JSR LF5B8 /
; JSR LF585 / JSR LF5D0 / JMP LF00C` at $F22C -- so the call/return has to be
; flattened: a bank switch is a jump and nothing returns through one. This bank
; replays the three calls and switches back, and bank 1 re-enters at $1000 with
; the WARM bit set, which its dispatcher turns into `JMP LF00C`.

        CPU     6502
        INCLUDE "vcs.inc"
        INCLUDE "fujinet.inc"
        INCLUDE "cfg.inc"
        INCLUDE "vodefs.inc"
        INCLUDE "tail.inc"

VOBANK  EQU     BANKKERN

; ---------------------------------------------------------------------------
; The entry. The trampoline enters every bank at $1000, and the kernel is
; pinned at $1585, so everything below it is this bank's to spend.
;
; CLD is insurance, not ceremony: an ADC anywhere in the net machine that ran
; in decimal mode would be a desync that only showed up after a score.
        ORG     $1000

; THE CHECKSUM COSTS NOTHING, BECAUSE IT RUNS IN A LINE THAT WAS GOING SPARE.
;
; Everything else the shim does happens inside a timed band, where the spin at
; the far end absorbs whatever it took -- see the note at the top of
; vogate.inc. VOCRC is the one piece that would not fit in the game bank, and
; nothing out here is timed, so its cycles come straight out of the picture.
;
; THREE PLACEMENTS WERE MEASURED AND TWO WERE WRONG. After the kernel it lands
; in the gap between the sprite kernel''s last WSYNC at $F61F and the frame
; loop''s next at $F00E, and costs a line: 263 against stock''s 262. Before
; LF5B8, in the line $F226 has just started, it costs TWO -- that line was
; fuller than it looks, because the bank switch and the trampoline have already
; spent most of it.
;
; Paying for the line with a blank one -- asking LF5B8 for six instead of seven
; -- does give 262 back, and `make det` fails: the display kernel samples the
; paddle by counting scanlines until the capacitor charges, so moving the
; picture up a line moves the sample. The frame length was right and the game
; was playing differently.
;
; AND A 263-LINE FRAME IS NOT HARMLESS EITHER, which is what a thirty-second
; run finally showed after every ten-second one had passed. Over 1600 frames
; the extra line drifts the paddle sample against the capacitor''s charge until
; the reading lands one unit out, and $BB, $BC and the position $B3 drove began
; to differ from stock. The frame was constant and the game was still playing
; differently.
;
; So the line goes back. VOCRC is unrolled to 58 cycles and called where a
; whole scanline is going spare.
;
; It is safe in this bank at all because nothing between here and the next tick
; boundary writes a cell it covers -- vocrck.inc says which and why.
VOKENT: cld
; ---- the checksum, INSIDE the timed band ----
;
; This bank is entered at the HEAD of the vertical blank's spin, so the timer
; armed at $F0D0 is still running and VOWAIT below still has to wait it out.
; Anything done here comes out of the 960 cycles of slack the game logic left,
; and the spin absorbs it: IT COSTS THE FRAME NOTHING.
;
; That is the third placement tried and the first correct one. After the kernel
; it cost a line; before LF5B8 it cost two; and between LF5B8 and LF5D0 -- a
; genuinely fresh scanline -- it shifted the sprite kernel's start within that
; line, which moves the paddle sample and changes the reading. Here the
; question does not arise, because here the frame's length is decided by a
; timer rather than by how long the code took.
        jsr     VOCRC

; ---- the vertical blank's remaining slack, which is the transport's ----
;
; The timer was armed at $F0D0 with $20, so the band is 2048 cycles and the
; game logic has just spent most of it. What is left is what the network
; machine gets, and it takes it in bounded micro-steps: VOWAIT re-reads INTIM
; before every one and stops when the slack is gone, so IT CANNOT OVERRUN THE
; KERNEL BY CONSTRUCTION. There is no budget to get wrong.
;
; A STALLED FRAME BUYS THE MOST STEPS, which is exactly when they are wanted:
; the game logic did not run, so almost the whole band is still here.
        jsr     VOWAIT

; ---- the four strobes the game bank used to do at $F224 ----
        sta     CXCLR
        sta     WSYNC
        sta     HMOVE
        sta     VBLANK

; ---- the picture ----
        ldy     #$07
        jsr     LF5B8
        jsr     LF585
        jsr     LF5D0
        lda     #BANKGAME
        jmp     VOGOTO

        INCLUDE "vocrck.inc"
        INCLUDE "vonet.inc"

; ---------------------------------------------------------------------------
; The netcode, in the rest of the 1413 bytes in front of the kernel.

        IF      * > $1585
        ERROR   "the network machine has overrun the display kernel at $1585"
        ENDIF

        INCLUDE "vo.inc"

; ---------------------------------------------------------------------------
; The second hole: 236 bytes where the game's tables used to be. VORE3 is the
; end of the kernel's own region.
        ORG     VORE5

        IF      * > $1743
        ERROR   "bank 2's second hole has overrun the straddling run at $1743"
        ENDIF

        END
