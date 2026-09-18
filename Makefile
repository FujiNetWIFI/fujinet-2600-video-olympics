# Makefile -- a convenience wrapper. build.sh is the real build.
#
# Networked two-player Video Olympics for the Atari 2600 over the FujiNet
# cartridge mailbox. See the ladder in PORTING.md; every target below is a
# gate, and each one has to pass before the next is worth starting.

FUJI_FIRMWARE ?= $(HOME)/Workspace/fn-2600
MAME_DIR      ?= $(HOME)/Workspace/mame
SECS          ?= 30
PORT          ?= 9600

SRC := $(wildcard src/*.asm src/*.inc)

.PHONY: all disasm verify-org probe echo vo frames det inputs lag sim lobby session rig rig-hold rig-play rig-repair ladder play stop clean \
        relay-c relay-c-strict relay-c-asan server-diff

all: verify-org

# M0a -- the disassembly itself. There was no published commented source for
# Video Olympics, so this project generates one: DiStella over the dump, steered
# by the code/data map in tools/vo.cfg. Regenerating it is cheap and it is NOT
# checked in -- it is the game. See tools/vo.cfg for why the map needs a hand:
# the per-variation routines are reached through JMP ($00A1), which ends a
# trace, so $F337-$F3FF looks like data to every automatic pass.
DISTELLA ?= $(HOME)/Workspace/distella/distella

disasm: rom/video-olympics.asm
rom/video-olympics.asm: rom/Video-Olympics.bin tools/vo.cfg
	$(DISTELLA) -paf -ctools/vo.cfg $< > $@
	@echo "disasm: $$(wc -l < $@) lines"

# M0b -- the conversion gate, and the anti-drift guard for everything
# downstream. The generated disassembly is reassembled through
# tools/dasm2as.py and the result must be the cartridge dump, byte for byte.
# This gate is load-bearing here in a way it was not in Combat, which inherited
# a source somebody else had already proved: it is the only thing standing
# between a wrong code/data split and a build that looks fine until it runs.
verify-org: rom/video-olympics.asm
	FUJI_FIRMWARE="$(FUJI_FIRMWARE)" ./build.sh verify-org

clean:
	rm -rf build

# M1 -- the transaction latency probe. THE FIRST THING THAT RUNS: K and d are
# derived from this number, and PORTING.md §2 exists because a family that
# guessed it was wrong by 3x for years.
probe: build/probe.bin
build/probe.bin: src/probe.asm src/vocore.inc src/vodefs.inc src/fujinet.inc src/vcs.inc build.sh
	FUJI_FIRMWARE="$(FUJI_FIRMWARE)" ./build.sh probe

echo: probe
	SECS=$(SECS) test/run_probe.sh

# M2 -- the bank split. Video Olympics, relaid across three 2K banks and the
# fixed half, with the netcode not yet in it: a split build must play EXACTLY
# like stock, so that any difference later is the netcode's fault and nothing
# else's.
#
# NOTE the IRQ vector. This ROM issues BRK as a two-byte subroutine call three
# times ($F262, $F2C8, $F453) and the handler at $F438 swaps the nibbles of A
# and returns with RTI. In the banked layout the vectors live in the fixed tail,
# so votail.asm must point $1FFE at $1438 -- see PORTING.md 3.1.
vo: build/vo.bin
build/vo.bin: $(SRC) build.sh tools/mkbanks.py tools/patches.py tools/dasm2as.py
	FUJI_FIRMWARE="$(FUJI_FIRMWARE)" VOLAG="$(VOLAG)" ./build.sh vo

build/stock.bin: rom/Video-Olympics.bin
	@mkdir -p build && cp rom/Video-Olympics.bin build/stock.bin

# Every frame the same length, through two bank switches a frame. The gate is a
# LEARNED constant compared against stock, never a spec number -- PORTING.md
# 4.16, measure the frame, do not count it.
frames: build/stock.bin
	ENDPOINT="TCP://127.0.0.1:9699/" FUJI_FIRMWARE="$(FUJI_FIRMWARE)" ./build.sh vo >/dev/null
	SECS=$(SECS) SLOT=a26_2k_4k ./run.sh stock frames | tail -4
	SECS=$(SECS) ./run.sh vo frames | tail -4

# M2/M3 -- the split and the input shim both play like stock, frame for frame.
# DET_QUIET, because injecting stick movement through MAME's ports is not
# frame-exact between two builds; see the note in emu/det.lua.
# ENDPOINT deliberately points at a port nothing listens on. This gate tests the
# LOCAL path -- the claim that a patched Video Olympics plays exactly like the
# -- and it reaches that path through the session's fallback. Built against the
# real endpoint it would depend on whether a relay happened to be running on
# this machine, and `make play` leaves one up.
# KNOWN, AND NOT YET ACTED ON. At SECS=30 this gate fails: past about frame
# 1630 the banked build's filtered paddle cells $BB/$BC differ from stock on
# 1.5% of frames, and the position they drive follows. It is the BANK SPLIT and
# not the netcode -- a build with VOCRC removed and the stock spin inlined
# diverges identically -- and it does not touch lockstep, because two consoles
# run the same build and take the same path. See PORTING.md 3.19.
#
# The gate is left failing rather than narrowed to make it green. A gate that
# has been taught to excuse the thing it found is not a gate.
det: DETENDPOINT = TCP://127.0.0.1:9699/
det: build/stock.bin
	ENDPOINT="$(DETENDPOINT)" FUJI_FIRMWARE="$(FUJI_FIRMWARE)" ./build.sh vo >/dev/null
	DET_QUIET=1 SECS=$(SECS) SLOT=a26_2k_4k ./run.sh stock det 2>/dev/null \
	    | grep -E '^[0-9]+ [0-9A-F]{4}$$' > build/det_stock.txt
	DET_QUIET=1 SECS=$(SECS) ./run.sh vo det 2>/dev/null \
	    | grep -E '^[0-9]+ [0-9A-F]{4}$$' > build/det_split.txt
	python3 tools/ramdiff.py build/det_stock.txt build/det_split.txt

# M3 -- the interception proof, in two forms. The static one is the stronger:
# after the patch the game's own seven input sites are GONE and every read of a
# console port in the whole run comes from VOSHIM.
inputs: build/stock.bin
	ENDPOINT="TCP://127.0.0.1:9699/" FUJI_FIRMWARE="$(FUJI_FIRMWARE)" ./build.sh vo >/dev/null
	SECS=$(SECS) ./run.sh vo inputs 2>/dev/null | sed -n '/^SITES/,$$p'

lag:
	VOLAG=1 $(MAKE) vo
	SECS=$(SECS) ./run.sh vo lag | tail -3
	$(MAKE) vo

# The protocol, with no emulator anywhere. Runs in a second and catches every
# framing and pairing mistake the ROM would otherwise find at fifteen ticks a
# second through two emulators and two fujinet-pc instances.
sim:
	python3 tools/vo_client_sim.py

# The C relay. server/vo_relay_server.py stays canonical; this binary is a
# transliteration of it, and `make server-diff` is what keeps it honest. Every
# gate that starts a relay takes SERVER=c to run this one instead:
#
#   make relay-c && SERVER=c make sim lobby rig
#
relay-c:
	$(MAKE) -C server/c
relay-c-strict:
	$(MAKE) -C server/c strict
relay-c-asan:
	$(MAKE) -C server/c asan

# The differential: both relays through identical scripted scenarios, with
# every frame the clients receive and every line the servers log compared byte
# for byte. A non-empty diff is a bug in the C port.
server-diff: relay-c
	python3 tools/server_diff.py

# The Lobby registration contract, against a MOCK lobby on an ephemeral port.
# Never the real one: this family has rewritten a machine-wide appkey by
# pointing a test at production before now.
lobby:
	python3 tools/test_lobby_pub.py

# M5 -- the session: appkey-less for now, but a real socket, a real HELLO and a
# real wait. One console, because half the handshake needs no opponent.
session:
	SECS=$(SECS) test/run_sess.sh $(SECS)

# M6/M7 -- the whole thing: two consoles, two FujiNets, one relay, one match.
# SELECT then RESET, pressed on the HOST console only, obeyed by both.
rig:
	SECS=$(SECS) test/run_rig.sh $(SECS)

# The case a person found and no scheduled tap-and-release ever did: SELECT HELD
# DOWN for the whole run, so the debounce walks the variation
# onwards again and again. Two consoles that re-arm at even slightly different
# moments end up playing different games -- a maze on one screen and biplanes on
# the other -- while every checksum they exchange agrees, because nothing has
# yet happened to move the tanks apart. Its own gate because it is the one that
# broke, and 45 seconds because five advances is what it takes to show.
rig-hold:
	RIG_HOLD=select SNAPTICK=250 SECS=45 test/run_rig.sh 45

# The gate the rig never had: two consoles with their HANDS ON THE STICK,
# playing asynchronously the way two people do, and a per-tick diff of the sim
# state that names the first cell to disagree. Every switch gate above was green
# while this was broken, because pressing RESET and SELECT only ever exercises
# the two inputs that are ANDed on the wire and identical on both machines by
# construction.
rig-play:
	RIG_LUA=play SECS=40 test/run_rig.sh 40

# Desync REPAIR, which cannot be tested by waiting for a bug: a correct pair
# never diverges. So one console is deliberately corrupted mid-game -- TankY0
# nudged by one scanline, the smallest desync there is -- and the assertion is
# that the two come back together on their own and stay together.
rig-repair:
	RIG_LUA=play PLAY_INJECT=120 SECS=60 test/run_rig.sh 60

# The whole ladder, in order. Each gate has to pass before the next is worth
# starting, which is the only reason the order is what it is.
ladder: verify-org sim lobby vo frames det inputs rig rig-hold rig-play rig-repair
	@echo "LADDER: every gate passed"

# Two consoles, side by side, open-ended. The rig proves it; this is for
# watching it.
play:
	test/run_play.sh
stop:
	test/stop.sh
