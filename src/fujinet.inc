; fujinet.inc -- equates for the FujiNet cartridge mailbox.
;
; Hand-mirrored from fujinet-firmware/pico/atari-2600/firmware/include/
; fuji_mailbox.h. tools/checkdefs.py in that tree compares this file against
; that header on every build, so the copy cannot drift -- and drift here is
; invisible: the spec once put the control page at $1D00 while a client's .inc
; said $1C00, and the client came up, drew a screen, and simply never armed
; the mailbox.
;
; This is the 5 Card Stud port's file with the card bed removed and the FUJI
; device's host and boot commands added back, because unlike a game the lobby
; DOES mount and boot: picking a room claims a host slot, writes an appkey and
; swaps the ROM out from under itself.
;
; EQUATES ONLY. This file is included BEFORE the ORG; the code half is
; lblib.inc and goes inside it. Including the code half early instead
; assembles the whole transport at $0000 and every JSR to it becomes
; `20 00 00` -- which assembles cleanly and fails at run time.

; ---------------- cart -> console, painted ----------------
FNTEXT  EQU     $1800           ; six 128-byte text planes
FNRPLY  EQU     $1B00           ; the reply window, 512 bytes
FNACKS  EQU     $1F00           ; echoes SEQ when the reply is ready
FNSTAT  EQU     $1F01           ; bit0 link up, bit1 busy
FNERR   EQU     $1F02           ; FNE* below
FNRCMD  EQU     $1F03           ; $06 ACK / $15 NAK
FNRXLO  EQU     $1F04           ; reply length, little-endian
FNRXHI  EQU     $1F05
FNBST   EQU     $1F06           ; boot state
FNBPC   EQU     $1F07           ; boot progress, 0-100
FNBER   EQU     $1F08           ; boot error
FNMAG0  EQU     $1F09           ; 'F'
FNMAG1  EQU     $1F0A           ; 'N'
FNPVER  EQU     $1F0B
FNSECH  EQU     $1F0C           ; slice echo, published last after a repaint
FNBTXG  EQU     $1F0D           ; last text row rendered, plus a toggle
FNBBNK  EQU     $1F0E           ; the live bank
FNBFLG  EQU     $1F0F           ; bit0 armed, bit1 claim honoured
FNPLNL  EQU     $1F17           ; path buffer length, little-endian
FNPLNH  EQU     $1F18

; ---------------- console -> cart, write-only ----------------
; NEVER READ THESE PAGES. The cart does not drive them -- not driving them is
; exactly what lets it sample the data bus -- so a read returns the floating
; bus and, worse, is decoded as an access. Only STA/STX/STY may target them:
; no INC/DEC/shift/rotate (three bus cycles at one address, and the "last
; value before the address changed" sampling has no defined answer in that
; burst) and no indirect store anywhere (it performs a read at the address it
; writes). tools/checkrom.py fails the build on all of these.
FNRSEL  EQU     $1D00           ; + n (n < $80): arm register n
FNCMT   EQU     $1DFF           ; commit the armed register with this data
FNTX    EQU     $1E00           ; a write anywhere in the page appends

; Registers.
FR_DEV  EQU     $00
FR_CMD  EQU     $01
FR_NPAR EQU     $02
FR_DRST EQU     $05             ; any value: rewind the TX write pointer
FR_RXSL EQU     $06
FR_SEQ  EQU     $10             ; nonzero and != ACKSEQ: launch
FR_BLCK EQU     $11             ; arm the ROM swap

; One-shot operations, in the bit7-set half of the control page.
FH_BANK EQU     $80
FH_TROW EQU     $F0             ; data = row: begin composing it
FH_TCHR EQU     $F1             ; data = char: append (saturates at 12)
FH_TEND EQU     $F2             ; data = attribute (0): render the row
FH_PATHC EQU    $F3             ; data = char: append to the path buffer
FH_PATHO EQU    $F4             ; data = FP_*: act on the path buffer
FH_ARM1 EQU     $FC
FH_ARM2 EQU     $FD
FH_SWAP EQU     $FE

FNAM1   EQU     $B5             ; the arming pair
FNAM2   EQU     $4A
FNBLKM  EQU     $B5             ; the bootlock magic

FNTCOL  EQU     12              ; text columns
FNTROW  EQU     21              ; text rows
FNTCLH  EQU     6               ; scanlines per cell: 5 of ink, 1 of leading

; Path buffer ops. FOUR buffers, and they SURVIVE A CONSOLE RESET -- there is
; no reset line on this connector -- so a cold start must empty all four and
; leave a known one selected.
FP_RST  EQU     0
FP_POP  EQU     1
FP_TX   EQU     2               ; emit, NUL-padded to exactly 256
FP_TXRAW EQU    3               ; emit exactly what is in it
FP_POPCH EQU    4               ; the text editor's backspace
FP_SEL0 EQU     5
FP_SEL1 EQU     6
FP_SEL2 EQU     7
FP_SEL3 EQU     8
FP_COMMIT EQU   9
FP_SEED EQU     10

; $1F20-$1FFB is the client's fixed tail: the cold stub and the bank
; trampoline, which must be at the same address seen from every bank.
FNTAIL  EQU     $1F20

; ---------------- the blit port ----------------
; Six stores move a screenful of bytes the console has neither the RAM nor the
; raster time to move itself.
FH_BSL  EQU     $F5             ; source offset into the reply, low
FH_BSH  EQU     $F6             ; ...high
FH_BDL  EQU     $F7             ; destination, low (a text ROW for TEXT/PATH)
FH_BDH  EQU     $F8             ; ...high
FH_BCNT EQU     $F9             ; count
FH_BGO  EQU     $FA             ; data = the transform; fire
FB_RAW  EQU     0
FB_TEXT EQU     1               ; cnt ASCII bytes of the reply -> text row dst
FB_PATH EQU     7               ; cnt chars of the path buffer -> text row dst
FB_TCELL EQU    8               ; ONE character (low byte of src) into ONE
                                ;   cell, dst = row * FNTCOL + col

; ---------------- error codes ----------------
FNEOK   EQU     0
FNEWAIT EQU     $FF             ; the cart never answered at all
FNENAK  EQU     $EE             ; the server said NAK
FNEBADL EQU     $FC             ; the reply is shorter than its header implies
FNEBAD  EQU     $02             ; a transport step failed; counted, not fatal
FNEPEER EQU     $03             ; the peer left the match
FNENOC  EQU     $FD             ; no cartridge answered -- deliberately not 1,
                                ;   which is FN_ERR_NOLINK

; ---------------- zero page, the transport's ----------------
; NOT HERE. The Lobby's copy of this file pins them at $80-$8C and $A8-$A9,
; which on this port would be BINvar, CLOCK and MVadjA -- live Combat
; variables, every one. Combat has twenty-six free bytes and they are mapped,
; with their reasoning, in vodefs.inc. Nothing in the mailbox spec constrains
; where a client keeps them.

; ---------------- input ----------------
; Joystick 0 in SWCHA bits 7-4 and the console switches in SWCHB, all active
; low, folded by INSCAN into one active-high byte.
IN_UP   EQU     $01
IN_DOWN EQU     $02
IN_LEFT EQU     $04
IN_RIGHT EQU    $08
IN_FIRE EQU     $10
IN_SEL  EQU     $20
IN_RST  EQU     $40
INDIRS  EQU     IN_UP|IN_DOWN|IN_LEFT|IN_RIGHT

; ---------------- N: device ----------------
NETDEV  EQU     $71             ; N1:
NCOPEN  EQU     $4F             ; 'O'
NCCLOSE EQU     $43             ; 'C'
NCREAD  EQU     $52             ; 'R'
NCWRITE EQU     $57             ; 'W' -- no other 2600 client sends one: every
                                ;   shipping port is an HTTP GET
NCSTAT  EQU     $53             ; 'S'
NMHTTPG EQU     $0C             ; open mode: HTTP GET
NMRDWR  EQU     $0C             ; open mode: READWRITE. The same value -- what
                                ;   HTTP overloads as "GET, pure and
                                ;   unmolested" is ACCESS_MODE READWRITE to
                                ;   every other protocol, TCP included.
NTRNONE EQU     0               ; translation: none

; NET_STATUS reply: 4 bytes in slice 0.
NSAVLO  EQU     0               ; bytes waiting, low
NSAVHI  EQU     1               ; ...high
NSCONN  EQU     2               ; connected flag
NSDEVST EQU     3               ; nDevStatus_t; 1 = SUCCESS

; ---------------- FUJI device ----------------
FNDEVF  EQU     $70
FCAKOPN EQU     $DC
FCAKCLS EQU     $DB
FCAKRD  EQU     $DD
FCAKWR  EQU     $DE
FCMHST  EQU     $F9             ; MOUNT_HOST
FCMIMG  EQU     $F8             ; MOUNT_IMAGE
FCSDFP  EQU     $E2             ; SET_DEVICE_FULLPATH
FCRHST  EQU     $F4             ; READ_HOST_SLOTS
FCWHST  EQU     $F3             ; WRITE_HOST_SLOTS -- all eight, always

HOSTSLN EQU     32              ; MAX_HOSTNAME_LEN: the stride in the reply
NHOSTS  EQU     8               ; MAX_HOST_SLOTS
DEVSLOT EQU     0               ; this console has one thing to mount into
FMREAD  EQU     1               ; disk access mode: read

; FN_R_BOOT_STATE values.
FNBIDL  EQU     0
FNBXFR  EQU     1
FNBRDY  EQU     2
FNBFAIL EQU     $80

; The shared FujiNet username: creator 1, app 1, key 0. Every game in the
; family reads the same slot, which is the point of it -- and the lobby writes
; the CHOSEN ROOM'S URL to the same creator and app under the record's own
; game_type, which is how the booted game learns where to connect.
AKCREAT EQU     1
AKAPP   EQU     1
AKKEY   EQU     0
AKMRD   EQU     0
AKMWR   EQU     1
