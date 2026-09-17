; vodefs.inc -- the zero page map, the tick constants and the wire format.
;
; EQUATES ONLY. Included BEFORE the ORG, like fujinet.inc. The code half is
; split across voinput.inc, vocap.inc, vocrc.inc, vonet.inc and vosess.inc.
;
; READ THIS FIRST. Everything else in the netcode assumes the layout below and
; the reasoning that produced it.
;
; ===========================================================================
; ZERO PAGE
; ===========================================================================
;
; Video Olympics uses 75 of the 128 bytes of RIOT RAM, which leaves 53 -- but
; the naive count is wrong twice, and tools/zpmap.py exists to say so:
;
;   * A cell reached only by INDEXING is not free. `LDA $BB,X` with X = 0..3
;     touches $BB-$BE and an operand scan reports only $BB. Ten indexed bases
;     had to be walked and bounded by the index range the code actually
;     produces -- $9B,X runs X = 8..0 over the nine-byte dispatch group,
;     $B2,X over a player index, $BB,X and $BF,X over all four paddles.
;
;   * THE STACK IS IN ZERO PAGE. The 6507 puts the stack at $0180-$01FF, which
;     mirrors onto $80-$FF. Six bytes are reserved at the top for it. The
;     stock nesting is only two deep (the frame loop -> LF23A -> LF321), and
;     the display kernel's stack-pointer games cost nothing: it sets SP to
;     $1D, $1F and $26 so that PHP writes to $011D, $011F and $0126, which
;     have A7 CLEAR and are therefore TIA registers -- ENAM0, ENABL and HMM0 --
;     not RAM. That is a 1977 trick for a fast TIA write, not stack usage.
;
; What is left, and all this file may spend:
;
;       $83  $86  $CD-$F9        47 cells
;
; ---------------------------------------------------------------------------
; Per-frame scratch. $83 and $86 are the two cells the game leaves alone
; inside its own working set, so they cost nothing from the block above.
VOADV   EQU     $83             ; nonzero: the sim advances this frame
VOTMP   EQU     $86             ; scratch, dead between routines

; ---------------------------------------------------------------------------
; THE SHADOWS THE PATCHED SITES READ. These are the whole of the interface
; between the netcode and the 1977 game: seven instructions in the dump point
; here instead of at the console, and nothing else changes.
;
; VOPAD is indexed by the game's own player number, so the patch at $F1B4 is
; `LDA VOPAD,X` where stock had `LDA $BB,X` -- two bytes either way.
;
; The role mapping is the physical one: the left port is paddles 0 and 1 and
; the right port 2 and 3, so the HOST's paddle A lands in VOPAD+0 and the
; GUEST's in VOPAD+2. The game confirms it -- $F206 computes $93 = $92 EOR #$02,
; so the two live players in a two-player variation always differ in bit 1.
; Slots 1 and 3 take the second paddles when four-player lands.
;
; NOTE what is NOT patched: the display kernel goes on writing the real paddle
; into $BB-$BE every frame, from this console's own controller. That is
; deliberate. Patching the kernel's store would have to be conditional on
; networked mode and `make det` would stop being a byte-for-byte claim; and the
; capture wants that cell anyway, because $BB-$BE is where the local paddle
; arrives already filtered.
VOPAD   EQU     $CD             ; 4: $CD-$D0, by player      <- patch at $F1B4
VOSWA   EQU     $D1             ; synthetic SWCHA            <- patch at $F0A1
VOSWB   EQU     $D2             ; synthetic SWCHB            <- patches at $F0D3,
                                ;   $F0E9, $F26A, $F2D7, $F528

; ---------------------------------------------------------------------------
; Netcode state.
VOENT   EQU     $D3             ; entry bits, role, and the frame-in-tick PHASE
                                ;   in bits 6-7. See VOE_* below for why the
                                ;   phase lives HERE and not in a game cell.
VOSEQ   EQU     $D4             ; the sequence we are waiting for
VOERR   EQU     $D5             ; low nibble: last transport error. high nibble:
                                ;   the current run of stalled frames.
VOTICK  EQU     $D6             ; the sim tick, low 8 bits. It WRAPS, and every
                                ;   comparison of two ticks is SIGNED so the
                                ;   wrap is invisible across +/-127.
VONST   EQU     $D7             ; the network machine's state, NS_* below
VOCRCV  EQU     $D8             ; the sim checksum, SAMPLED AT A TICK BOUNDARY
                                ;   and at no other moment. The invariant the
                                ;   whole desync check rests on: at any instant
                                ;   VOCRCV is the checksum of the state ENTERING
                                ;   VOTICK, so a record stamped Y carries the
                                ;   checksum of Y-d on both consoles whatever
                                ;   either of them stalled through.
VORWAT  EQU     $D9             ; highest remote tick received. Starts at $FE,
                                ;   not $FF: $FF reads as "tick 0 is ready" the
                                ;   instant VOTICK reaches 0.

; THE REMOTE RING IS EIGHT SLOTS, AND THE EIGHT IS LOAD-BEARING. Console B can
; advance to tick T_B only with A's input for T_B, which A sent at its own tick
; T_B - d. So T_B <= T_A + d, and A has sent up to T_A + d <= T_B + 2d: B can be
; holding records for T_B .. T_B + 2d, which is 2d+1 = 5 slots at d = 2. Eight
; is the next power of two, which buys `AND #7` instead of a modulo. A
; four-slot ring silently overwrites a tick nobody has used yet.
VORING  EQU     $DA             ; 16: $DA-$E9, 8 slots x VORS, (tick AND 7)*VORS
VOLOC   EQU     $EA             ; 8:  $EA-$F1, 4 slots x VORS, (tick AND 3)*VORS
VORDN   EQU     $F2             ; records left to consume; also the reply-window
                                ;   cursor, as (VORDN-1)*IR_LEN
VOW0    EQU     $F3             ; the HOST's switch/trigger byte, shim only
VOW1    EQU     $F4             ; the GUEST's
VOW2    EQU     $F5             ; the HOST's paddle A
VOW3    EQU     $F6             ; the GUEST's
VOCRCK  EQU     $F7             ; the checksum the KERNEL bank computed at the
                                ;   end of the last frame. The boundary reads
                                ;   it rather than computing one, because the
                                ;   game bank had no room -- and it is the same
                                ;   number, because nothing between the two
                                ;   instants writes a cell it covers. See
                                ;   vocrck.inc.

; ---------------------------------------------------------------------------
; THE SESSION SIDE, UNIONED OVER THE RINGS. Safe, and checked: vonet.inc never
; touches one of these -- it drives the mailbox registers directly -- and
; FNBEG, which does read them, is only ever called by the boot bank, which has
; handed over long before the first tick. The two are never live at once.
FNDEV   EQU     $DA             ; transaction setup, read by FNBEG
FNCMD   EQU     $DB
FNNPR   EQU     $DC
FNTMO   EQU     $DD             ; timeout in ~0.6s quanta
FNCNT   EQU     $DE             ; timeout quanta remaining
FNPTRL  EQU     $DF             ; reply-window cursor
FNPTRH  EQU     $E0
FNPCNT  EQU     $E1             ; bytes written into the current path payload
INCUR   EQU     $E2             ; input scan, both halves
INPREV  EQU     $E3
CSDLY   EQU     $E4             ; a frame counter for the session's wait loops.
                                ;   IN MEMORY, NOT IN A REGISTER: the text
                                ;   kernel counts scanlines in X and Y and
                                ;   clobbers both, and a caller that counted its
                                ;   own repeats in one of them ran its wait 256
                                ;   times over -- "seventy seconds where
                                ;   seventeen milliseconds were meant, which
                                ;   looks exactly like a server that never
                                ;   answers".
FNSEQ   EQU     VOSEQ           ; shared, because it means the same to both

; $F8-$F9 are spare -- two cells. $FA-$FF is the stack.
;
; THE FOUR-PLAYER UPGRADE COSTS ONE UNION. Going to two paddles per console
; widens VORS from 2 to 3, which takes the two rings from 24 bytes to 36 and
; the total from 36 cells to 48 against the 47 available. One session cell has
; to move under the rings, or VOWIN drops from 3 to 2. Nothing else changes:
; the wire already carries the second paddle.

; ===========================================================================
; THE TICK
; ===========================================================================
;
; Both of these are BUILD KNOBS and both start at Combat's measured values.
; Video Olympics is a far twitchier game than Combat and the intention is to
; bring VOD down once `make rig-play` is green -- but not before `make echo`
; has measured the transaction latency on this machine, because that is what
; says whether a tick can be shorter at all. PORTING.md 2, and the reason it
; is the first gate: a family that guessed this number was wrong by 3x for
; years, and every latency estimate built on it was three times too optimistic.
VOK     EQU     4               ; frames per tick. 4 at 60 Hz = 15 Hz.
VOKM    EQU     VOK-1           ; ...as a mask, which is why it is a power of two
VOD     EQU     2               ; ticks of input delay. 2 ticks = 8 frames =
                                ;   ~133 ms symmetric.
VOLM    EQU     3               ; local ring mask; the ring is 4, and 4 > VOD
VORM    EQU     7               ; remote ring mask; the ring is 8 >= 2*VOD+1

; The SLACK THRESHOLD the transport steps behind, in INTIM units. (Named for
; what it measures rather than for what it gates: AS is case-insensitive and
; VOGATE is the routine at the head of the timed band.) INTIM decrements every
; 64 cycles in the vertical-blank band, so INTIM >= VOSLACK guarantees at least
; (VOSLACK-1)*64 cycles remain. The hook re-reads INTIM before every bounded
; micro-step and stops when the slack is gone, so IT CANNOT OVERRUN THE KERNEL
; BY CONSTRUCTION -- there is no budget to get wrong.
VOSLACK EQU     8

; ===========================================================================
; THE WIRE
; ===========================================================================
;
; One console's input for one tick is THREE bytes on the wire and TWO in the
; rings. The third is the second paddle, sent as idle until four-player lands:
; sizing the wire for it now means that upgrade is a mixer change and not a
; protocol version bump.
VOTS    EQU     3               ; bytes per tick ON THE WIRE
VORS    EQU     2               ; bytes per tick IN THE RINGS

; Byte 0 is the paddle position, straight through. It is the display kernel's
; own line counter at the moment the controller's capacitor charged, low-pass
; filtered by the two-tap average at $F643, so it is already the number the
; game reads -- no scaling, no sense to invert.
;
; VOP_IDLE IS ZERO, AND THAT IS NOT THE MISTAKE 4.8 WARNS ABOUT. That rule is
; about ACTIVE-LOW inputs, where a zero byte reads as every switch pressed and
; the stick shoved in all four directions. A paddle position is not active low:
; zero is one end of travel, and it is exactly what the stock RAM clear leaves
; in $BB-$BE at power-on. Both consoles seeding the unsent ticks 0..d-1 with
; zero therefore agree with each other AND with what stock does.
VOP_IDLE EQU    $00

; Byte 1 keeps the hardware's own active-low senses, so the mixer is masks and
; shifts and never an inversion.
IW_TRGA EQU     $80             ; 1 = released   (SWCHA bit 7)
IW_TRGB EQU     $40             ; 1 = released   (SWCHA bit 6)
IW_SEL  EQU     $20             ; 1 = not pressed
IW_RST  EQU     $10             ; 1 = not pressed
IW_DIFF EQU     $08             ; 1 = A / Pro
IW_SPARE EQU    $07             ; read high
IW_IDLE EQU     IW_TRGA|IW_TRGB|IW_SEL|IW_RST|IW_DIFF|IW_SPARE

; SWCHA's paddle trigger bits, which $F0A1 masks with the table at $F6B4
; (`80 40 08 04`): bits 7 and 6 are the LEFT port's two triggers and bits 3
; and 2 the RIGHT port's. So the host's two bits go to 7/6 and the guest's to
; 3/2, which lands them on players 0/1 and 2/3 -- the same mapping VOPAD uses,
; for free.
SA_L    EQU     $C0             ; left port's pair
SA_R    EQU     $0C             ; right port's pair

; A record: a window of VOWIN ticks, newest first.
;
; A TRANSPORT THAT SENDS EXACTLY ONE TICK PER PACKET IS BETTING THAT IT NEVER
; MISSES A BEAT, AND IT WILL. The transport free-runs and stamps whatever tick
; the sim is on when its WRITE step comes round; a cycle that runs long means
; the sim advanced twice between two WRITEs and a tick's record is never sent
; at all. You cannot send N ticks in fewer than N writes, so there is no
; scheduling fix -- the record has to carry more than one tick. Three is not
; arbitrary: the local ring is four slots, so t, t-1 and t-2 are all still
; live.
VOWIN   EQU     3

IR_TICK EQU     0               ; the tick the window ENDS at (= VOTICK + VOD)
IR_IN   EQU     1               ; VOWIN * VOTS bytes, newest first
IR_CRC  EQU     IR_IN+VOWIN*VOTS        ; = 10
IR_CHK  EQU     IR_CRC+1                ; = 11
IR_PAD  EQU     IR_CHK+1                ; = 12: two bytes of padding follow, so
                                        ;   that a frame is exactly IR_LEN on
                                        ;   the wire in BOTH directions and the
                                        ;   relay's pad-on-the-way-out is a
                                        ;   no-op rather than a reshape
IR_PAY  EQU     14                      ; payload bytes after the type

; ON THE WIRE, AND A POWER OF TWO ON PURPOSE. Every frame a console can receive
; during play is exactly IR_LEN bytes, so it reads (avail AND IR_MASK) and a
; partial frame is simply left where it is. That makes misframing
; STRUCTURALLY UNREACHABLE -- the 2600 has no buffer to reassemble a split
; frame in -- and it keeps NParser::read's overrun for 0 < avail < length out
; of reach, since we never ask for more than the last STATUS reported.
IR_LEN  EQU     16
IR_MASK EQU     $F0
IR_SEED EQU     $A5             ; the check byte's salt

; ---------------------------------------------------------------------------
; THE FRAME TYPES
;
; frame := len(1) type(1) payload(len-1). `len` counts the type and the
; payload, so a frame is never zero-length and the reader can reject a bad one
; on sight.
WT_HELO EQU     $01
WT_LOBBY EQU    $03
WT_JOIN EQU     $04
WT_START EQU    $05
WT_INPUT EQU    $06
WT_CRC  EQU     $07
WT_STATE EQU    $08
WT_RESYN EQU    $09
WT_BYE  EQU     $0A
WT_LEFT EQU     $0B
WT_PING EQU     $0C
WT_PONG EQU     $0D

WPROTO  EQU     3               ; HELLO's version byte.
                                ;   2 put the television standard between the
                                ;   version and the name.
                                ;   3 widened a tick's input from one joystick
                                ;   byte to three paddle-game bytes, which
                                ;   moved the checksum inside the record. An
                                ;   older relay would pair the wrong bytes and
                                ;   report mismatches on a synchronised pair,
                                ;   so the version has to move with the shape.

; ===========================================================================
; VOENT -- the entry byte
; ===========================================================================
;
; TWO ENTRY BITS, NOT ONE ENTRY VALUE. They were one value at first -- 0 cold,
; 1 local, 2 playing -- and the boot bank handing over with 2 meant the game
; bank read "not cold" and jumped straight into the frame loop, so START never
; ran and the game came up with every variable zero.
VOE_WARM EQU    $01             ; reserved. The game bank's entry is
                                ;   UNCONDITIONAL -- the boot bank is the only
                                ;   way into it and always hands over with RAM
                                ;   set up -- so nothing tests this today. The
                                ;   bit is kept because 4.12's lesson is about
                                ;   conflating "has the game been initialised"
                                ;   with "is this a match", and the day a
                                ;   second entry path appears is the day that
                                ;   distinction is needed again.
VOE_NET EQU     $02             ; this is a match, not local play
VOE_ROLE EQU    $04             ; set = guest, and it drives players 2/3
VOE_RAN EQU     $08             ; the last boundary ran its tick, so the next
                                ;   one may advance VOTICK. See 4.14: the tick
                                ;   is incremented at the HEAD of the next
                                ;   boundary, right before the checksum is
                                ;   sampled, so the offset between a record's
                                ;   stamp and its checksum cannot depend on the
                                ;   local stall pattern.
VOE_LEFT EQU    $10             ; the peer left; the boot bank says so in words
VOE_RSY EQU     $20             ; a checksum mismatch was seen: press RESET into
                                ;   this console's own capture, so the repair
                                ;   reaches both ends by the road every real
                                ;   press takes and lands on the same tick.

; THE SIM CLOCK MUST NOT BE A CELL THE GAME CAN WRITE, and this is where it
; lives. Combat's boundary was `CLOCK AND 3` until held SELECT proved that
; ClrGam resets CLOCK on every variation advance -- so the netcode's own clock
; was a cell the simulation cleared, on exactly the frames the two consoles
; were least likely to be in step. Two consoles ended up playing different
; games while the relay verified tens of thousands of checksums without one
; mismatch. Here the phase is two bits at the top of VOENT, which nothing in
; the 1977 ROM has an instruction that touches, stepped once a frame by adding
; $40 -- which steps a two-bit counter without disturbing the flags below it,
; without a mask, and wraps by discarding the carry out of bit 7.
VOE_PH  EQU     $C0
VOE_PH1 EQU     $40             ; one phase step

; ===========================================================================
; THE NETWORK MACHINE
; ===========================================================================
; Dispatched by the RTS-trick jump table: 26 cycles flat, and importantly
; CONSTANT, which is what lets the INTIM gate bound a step by construction.
NS_IDLE EQU     0               ; no socket, or no match: do nothing
NS_WISS EQU     2               ; issue the WRITE and launch it
NS_WACK EQU     4               ; wait for the cartridge to acknowledge
NS_SISS EQU     6               ; issue the STATUS
NS_SACK EQU     8               ; ...and read how many bytes are waiting
NS_RISS EQU     10              ; issue the READ
NS_RACK EQU     12              ; ...and turn its length into a record count
NS_CONS EQU     14              ; consume one record from the reply window
NS_FAIL EQU     16              ; count it and start the cycle again
NS_LAST EQU     NS_FAIL         ; the dispatch table's last entry

; ---------------------------------------------------------------------------
; APPKEYS
;
; Two of them, both under the family's shared creator and app:
;
;   key 0            the FujiNet USERNAME, which every game in the family reads
;                    -- that is the whole point of it
;   key VOAPPKEY     the room's own URL, which the Lobby writes there on the way
;                    out and the booted game reads on the way in. There is no
;                    HTTP "join a room" call anywhere in this family; joining is
;                    a local write and a reboot.
;
; Both are read into a CARTRIDGE PATH BUFFER and never into console RAM. That
; is not thrift, it is necessity: a name is eight bytes and a URL is sixty-four,
; against twenty-six free in the whole machine -- and the reply window they
; arrive in is repainted by the very next transaction.
VOAPPKEY EQU    24              ; pending provisioning on the firmware wiki
AKNAMEK EQU     0
PB_NAME EQU     FP_SEL0         ; path buffer 0: the username
PB_URL  EQU     FP_SEL1         ; path buffer 1: the server URL
NAMEMIN EQU     2
NAMEMAX EQU     8

; ===========================================================================
; THE BANKS
; ===========================================================================
BANKBOOT EQU    0               ; cold start, the session, the WAITING screen
BANKGAME EQU    1               ; the frame loop, the game logic, the gate
BANKKERN EQU    2               ; the display kernel and the network machine

; THE IRQ VECTOR IS NOT SPARE ON THIS CARTRIDGE. Video Olympics issues BRK as a
; TWO-BYTE SUBROUTINE CALL at $F262, $F2C8 and $F453 -- BRK pushes PC+2, so the
; $EA after it is skipped and RTI returns past it -- and the handler at $F438
; is four `ASL / ADC #$00` pairs, a true four-bit rotate, i.e. swap the nibbles
; of A. One byte cheaper than JSR, three times.
;
; In a flat 2K cartridge that vector is the game's own. Here the vectors live
; in the fixed tail, so votail.asm must point $1FFE at the handler. All three
; call sites and the handler are in BANKGAME, so the bank mapped when a BRK
; executes is always the one holding $1438.
VOBRKH  EQU     $1438           ; the nibble-swap handler, in BANKGAME

; ===========================================================================
; TWO ADDRESSES IN THE GAME BANK THAT THE GATE JUMPS TO
; ===========================================================================
;
; The lockstep gate replaces `STA TIM64T` at $F0D0 -- three bytes, and the head
; of the timed band the whole game logic runs inside. It arms the timer itself,
; decides whether the sim advances, and then either falls into the logic or
; jumps over it to the spin that ends the band. A stalled frame therefore runs
; the band nearly empty and the spin absorbs the difference, so the FRAME IS
; THE SAME LENGTH either way -- which `make frames` checks.
;
; DiStella only labels what something branches to, and nothing branches to
; either of these, so they are spelled out. They cannot drift: the dump is
; pinned by `make verify-org`, and tools/patches.py asserts the opcode byte at
; each one so a typo here is a build error rather than a jump into the middle
; of an instruction.
VOLOGIC EQU     $10D3           ; LDA SWCHB -- the first byte of the game logic
VOSPIN  EQU     $121F           ; LDA INTIM -- the spin that ends the band

; VOGOTO -- the bank trampoline, at a FIXED address in the tail rather than a
; label, because it is the one address a bank has to know before build/tail.inc
; exists. votail.asm ORGs to it, so the two cannot drift apart.
VOGOTO  EQU     FNTAIL
