#!/usr/bin/env bash
# build.sh -- assemble the Atari 2600 networked Video Olympics client.
#
#   ./build.sh verify-org   the conversion gate: stock Video Olympics, rebuilt
#                           from the DASM disassembly through tools/dasm2as.py,
#                           must be byte-identical to the cartridge dump
#
# The client is (N+1) x 2048 bytes: N 2K banks then the 2K fixed half. MAME's
# vcs_cart_slot_device::call_load() accepts only 4096/8192/16384/32768, so N is
# 1, 3, 7 or 15 and nothing between. Video Olympics needs three.
#
# The "FUJI" claim is stamped into the fixed half. Without it the cartridge
# treats the image as a game and the mailbox goes dead the moment it boots.
#
# Env:
#   FUJI_FIRMWARE=...  firmware tree (default ~/Workspace/fn-2600)

set -euo pipefail
cd "$(dirname "$0")"

FUJI_FIRMWARE="${FUJI_FIRMWARE:-$HOME/Workspace/fn-2600}"
VCS="$FUJI_FIRMWARE/pico/atari-2600"

# ---------------------------------------------------------------------------
# THE TELEVISION STANDARD, AND THE ONE IMAGE THIS WILL NOT BUILD.
#
# A 2600 cannot measure which television it is plugged into. The ROM generates
# the video timing itself; there is no register, no interrupt and no external
# reference to read. That is why every 2600 game in history shipped as separate
# NTSC, PAL and SECAM images rather than detecting the standard at runtime, and
# it is why the refusal has to live HERE, at the only moment anyone knows.
#
# SECAM is refused because of the TIA, not the network. A SECAM TIA renders
# eight colours, chosen by the hue nibble, with the luminance bits ignored
# entirely. Video Olympics' object colour table at $F698 reads
#
#     0C 00 0E 06   20 20 30 20
#
# and the first four entries are hue 0 throughout -- they are told apart by
# LUMINANCE ALONE (6, 0, 7, 3). On a SECAM set all four collapse to the same
# colour, and two players end up looking at indistinguishable objects. No
# amount of lockstep fixes that; it simply is not two-player Video Olympics.
case "${TVSTD:-ntsc}" in
    ntsc) tvstd=0 ;;
    pal)  tvstd=1 ;;
    secam)
        cat >&2 <<'SECAM'
build.sh: refusing to build a SECAM image.

  A SECAM TIA renders eight colours, selected by the hue nibble, and ignores
  the luminance bits. Video Olympics' colour table at $F698 is hue 0 for its
  first four entries -- they differ in luminance only -- so on a SECAM set
  those objects collapse to one colour and both players can be looking at
  indistinguishable sprites. That is not a sync problem and lockstep does not
  help it.

  This is refused at build time because a 2600 cannot detect its own television
  standard at runtime: the ROM generates the video timing and there is nothing
  to read. The relay refuses a console that declares SECAM as well.

  TVSTD=ntsc (the default) or TVSTD=pal.
SECAM
        exit 1 ;;
    *)
        echo "build.sh: TVSTD must be ntsc, pal or secam (got '${TVSTD}')" >&2
        exit 1 ;;
esac

if command -v asl >/dev/null 2>&1; then
    AS=asl P2BIN=p2bin
elif [ -x "$HOME/asl/asl" ]; then
    AS="$HOME/asl/asl" P2BIN="$HOME/asl/p2bin"
else
    echo "build.sh: no Macroassembler AS found (tried PATH and ~/asl)" >&2
    exit 1
fi

mkdir -p build

# The "FUJI" claim is stamped post-link because its file offset depends on the
# image size: the fixed half is the LAST 2K, so the offset is
# (size - 2048) + (FN_R_CLAIM - $1800). Without it the cartridge treats the
# image as an ordinary game and the mailbox goes dead the moment it boots --
# which is exactly what must happen for a game, and exactly what must not
# happen for us.
stampclaim() {
    local f=$1 size
    size=$(stat -c%s "$f")
    printf 'FUJI' | dd of="$f" bs=1 \
        seek=$((size - 0x800 + 0x0710)) conv=notrunc status=none
    echo "$f: $size bytes"
}

HERE=$(pwd)

# AS writes its .p and .lst next to the source, so assemble from the source's
# own directory and collect the artefacts into build/. The include path carries
# src/ as well, because a generated source in build/ still includes vcs.inc.
assemble() {  # assemble <basename> <srcdir>
    local b=$1 d=${2:-src}
    ( cd "$d" && "$AS" -q -L -i . -i "$HERE/src" -i "$HERE/build" "$b.asm" )
    if [ "$d" != "build" ]; then
        mv "$d/$b.p" "build/$b.p"
        mv -f "$d/$b.lst" "build/$b.lst" 2>/dev/null || true
    fi
}

# ---------------- the conversion gate ----------------
#
# rom/video-olympics.asm is the disassembly `make disasm` produces from the
# dump, and once it verifies it is never edited. Every run regenerates the AS
# translation from it, so a converter change that broke a single byte could not
# survive here -- which is what makes this the anti-drift guard for everything
# downstream, exactly as `make verify-org` is in the Intellivision ports.
#
# Unlike Combat, this disassembly is OURS: there was no published commented
# source for Video Olympics to start from. That makes this gate load-bearing in
# a way it was not there -- it is the only thing standing between a wrong
# code/data split and a build that looks fine until it runs.
if [ "${1:-}" = "verify-org" ]; then
    python3 tools/dasm2as.py rom/video-olympics.asm > build/vo_org.asm
    assemble vo_org build
    "$P2BIN" build/vo_org.p build/vo_org.bin -r '$F000-$F7FF' -l 255 -q
    rm -f build/vo_org.p
    cmp build/vo_org.bin rom/Video-Olympics.bin
    echo "verify-org: byte-identical ($(stat -c%s build/vo_org.bin) bytes)"
    exit 0
fi

# ---------------- M1: the transaction latency probe ----------------
#
# A flat 4K image: one bank of code and the fixed half. The endpoint is
# regenerated every run so a stale value cannot survive an environment change,
# and it is the whole reason this is a build-time string rather than something
# read from an appkey -- the probe has to run before there is a lobby.
if [ "${1:-}" = "probe" ]; then
    {
        echo "; generated by build.sh -- do not edit"
        printf 'UENDPT: DB      "%s"\n' \
            "${ENDPOINT:-N:TCP://127.0.0.1:9605/}"
        echo "        DB      0"
    } > build/endpoint.inc
    printf '; generated by build.sh\nCSHLEN  EQU 0\n' > build/playername.inc

    assemble probe
    python3 tools/checkbanks.py build/probe.lst $((0x1800)) probe
    "$P2BIN" build/probe.p build/probe.bin -r '$1000-$1FFF' -l 255 -q
    rm -f build/probe.p
    stampclaim build/probe.bin
    python3 "$VCS/tools/checkrom.py" build/probe.bin
    exit 0
fi

# ---------------- the client: three banks and the fixed half ----------------
#
# (N+1) x 2048 with N in {1,3,7,15}, because that is all MAME's
# vcs_cart_slot_device::call_load() accepts. Three banks and the fixed half is
# 8192.
BANKS="voboot vogame vokern"

# Build-time switches, regenerated every run so a stale value cannot survive an
# environment change. They must be EQUates and not IFDEFs: AS resolves IF in its
# FIRST PASS, and a condition naming a symbol defined further down the file is
# not a build error -- it quietly takes the branch it should not.
{
    echo "; generated by build.sh -- do not edit"
    printf 'VOLAG   EQU     %s\n' "${VOLAG:-0}"
    printf 'VOTVSTD EQU     %s\n' "$tvstd"
} > build/cfg.inc

# The endpoint and the player name, regenerated every run so a stale value
# cannot survive an environment change. They are build-time strings today
# because the probe has to run before there is a Lobby to read an appkey from;
# the shared username appkey is where the name belongs once the boot bank can
# reach it.
{
    echo "; generated by build.sh -- do not edit"
    printf 'UENDPT: DB      "%s"\n' "${ENDPOINT:-TCP://127.0.0.1:9600/}"
    echo "        DB      0"
} > build/endpoint.inc
{
    name="${PLAYER:-PLAYER1}"
    echo "; generated by build.sh -- do not edit"
    printf 'CSHNLEN EQU     %d\n' $(( ${#name} ))
    printf '        DB      "%s"\n' "$name"
} > build/playername.inc

# vo.inc is GENERATED from the pristine disassembly on every build, through
# the same converter verify-org proves faithful and the declared patch map in
# tools/patches.py. There is no hand-maintained copy of the game in this tree for
# a patch to drift away from.
python3 tools/dasm2as.py rom/video-olympics.asm > build/vo_org.asm
assemble vo_org build
python3 tools/mkbanks.py build/vo_org.lst rom/video-olympics.asm build/vo.inc

# The baseline for the patch audit: the same pristine source, at the address a
# cartridge bank is actually mapped at. See tools/check_patch.py.
python3 tools/dasm2as.py --window rom/video-olympics.asm > build/vo_win.asm
assemble vo_win build
"$P2BIN" build/vo_win.p build/vo_win.bin -r '$1000-$17FF' -l 255 -q
rm -f build/vo_win.p

# THE TAIL IS ASSEMBLED FIRST: it holds the shared transport, and the banks
# reach it through build/tail.inc, generated from the addresses the tail
# actually assembled to. Nothing keeps a list of those by hand.
assemble votail
python3 tools/mktail.py build/votail.lst \
    FNRW,FNARM,FNCHK,FNBEG,FNPB,FNPW,FNGO,FNACK,\
FNROWA,FNCHR,FNENDR > build/tail.inc
tail -1 build/tail.inc | sed 's/^; */  tail: /'

parts=()
for b in $BANKS; do
    assemble "$b"
    python3 tools/checkbanks.py "build/$b.lst" $((0x1800)) "$b"
    "$P2BIN" "build/$b.p" "build/$b.bin" -r '$1000-$17FF' -l 255 -q
    rm -f "build/$b.p"
    parts+=("build/$b.bin")
done

"$P2BIN" build/votail.p build/votail.bin -r '$1800-$1FFF' -l 255 -q
rm -f build/votail.p

cat "${parts[@]}" build/votail.bin > build/vo.bin
rm -f "${parts[@]}" build/votail.bin

stampclaim build/vo.bin
python3 tools/checkrom_filter.py "$VCS/tools/checkrom.py" build/vo.bin
python3 tools/check_patch.py build/vo_win.bin build/vo.bin
