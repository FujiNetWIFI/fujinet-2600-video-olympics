; vogate.inc -- the thirteen bytes of the lockstep gate that HAVE to be in the
; game bank, because they decide whether the game logic runs.
; WHY THE SHIM RUNS HERE AND NOT AT THE END OF THE FRAME.
;
; It was in the kernel bank first, called from that bank's entry after the
; three kernel routines -- which is the freshest possible capture and costs no
; patch site. `make frames` refused it: stock measures 262 lines and that build
; measured 263, every frame, because the shim ran in UNBUDGETED time between
; the last line the kernel drew and the `STA WSYNC` at the top of the frame
; loop. Thirty cycles of local-play mirror was enough to push past a line.
;
; Constant is not the danger; VARIABLE is. In a match the boundary work runs on
; one frame in four and is a hundred and fifty cycles longer, so the frame
; would have been two lines longer every fourth frame and the picture would
; roll. The only safe place for work of a varying length is INSIDE a timed
; band, where the spin at the far end absorbs whatever it took.
;
; So the whole shim is in this bank, inside the 2048-cycle band armed on the
; line above, and $F21F's spin pays for it.

; ---------------------------------------------------------------------------
; VOGATE -- the patch target. Replaces `STA TIM64T` at $F0D0, IN THE GAME BANK.
;
; THE TIMER IS ARMED FIRST, before anything else happens. Everything downstream
; of here -- the shim, the whole game logic, the spin at $F21F -- is inside one
; 2048-cycle band, and the band's length is what makes a frame a frame. Arming
; it late would shorten the band by however long the shim took, and `make
; frames` would report a frame that changed length depending on whether a tick
; boundary fell in it.
;
; A STALL IS A FRAME THAT RUNS NORMALLY WITH THE GAME LOGIC SKIPPED. The band
; still runs, the spin at the end still absorbs whatever is left of the 2048
; cycles, VSYNC has already gone out, and the kernel still draws the picture
; from unchanged state. The frame is the same length; the display simply does
; not move.
;
; That is nearly free on this console, and the collision latches are why:
; $F224 strobes CXCLR every frame and the kernel refills them by drawing the
; same picture from the same state, so N frozen frames leave exactly what one
; would. Two consoles that stalled for different numbers of frames resume in
; agreement -- which is the whole reason lockstep can absorb a late record.
;
; X IS PRESERVED ACROSS THE SHIM, AND THAT IS NOT DEFENSIVE. The instruction
; this replaces sits nine bytes before `STX $88` at $F0DF, and the X it stores
; is the zero left by `INX` at $F0C9 on the way out of the paddle-trigger loop.
; The shim counts to four in X -- the four paddles -- so without this it
; returns $FF, the frame counter is set to $FF instead of 0 on the first frame
; a console switch changes, and the two builds are on different sims from there
; on. `make det` diverged at frame 2 and the cell it named was $88.
;
; PORTING.md 4.15 is the same lesson from the other side: there, a shared
; display kernel ate the CALLER's loop counter. Here the shim eats the game's.
; Anything inserted into a 1977 frame loop inherits its register conventions,
; and those conventions are not written down anywhere.
VOGATE:
        sta     TIM64T          ; the displaced instruction, and it goes FIRST
        txa
        pha
        jsr     VOSHIM
        pla
        tax
        lda     VOADV
        bne     VOGRUN
        jmp     VOSPIN          ; stalled: straight to the end of the band
VOGRUN:
; The sim advances, so the frame counter does. It lives in the dead bytes the
; $F21F patch leaves at $F224 -- see tools/patches.py -- because this hole has
; no nine bytes to spare and that one has twenty-two nobody can reach.
        jsr     VOFCNT
        jmp     VOLOGIC         ; run it

