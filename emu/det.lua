-- det.lua -- a deterministic input stream and a per-frame state checksum.
--
-- Two jobs, and they are the same job. It proves the banked build plays
-- EXACTLY like stock (M2), and it is the determinism rig two consoles have to
-- pass before lockstep can work at all (M4): drive identical inputs, print a
-- checksum of the sim state every frame, and require the two streams to be
-- byte-identical.
--
--   SLOT=a26_2k_4k ./run.sh stock det > build/det_stock.txt
--   ./run.sh vo det > build/det_split.txt
--   python3 tools/ramdiff.py build/det_stock.txt build/det_split.txt
--
-- The input has to be generated HERE and not by hand, because the whole point
-- is that both runs see the same bytes at the same frames. It is a plain
-- function of a counter of SIM frames -- no randomness, nothing read back off
-- the screen, nothing that depends on how fast the emulator ran.
--
-- WHAT THE CHECKSUM COVERS IS THE WHOLE DESIGN OF IT. Not "all of the game's
-- variables": some cells differ between two builds that are playing
-- identically, and including any of them makes the gate cry wolf forever.
--
--   * $80, $87 are the kernel's and the frame loop's scratch, dead between
--     routines and rebuilt every frame. Functions of the state, not the state.
--   * $81 IS STACK-DERIVED. The display kernel saves the stack pointer there
--     (`TSX / STX $81` at $F5D0, restored at $F63E) so that it can set SP to
--     $1D, $1F and $26 and use PHP as a fast write to ENAM0, ENABL and HMM0.
--     Stock reaches the kernel through a JSR and the banked build reaches it
--     through a bank switch, which is a JUMP that resets the stack -- so the
--     saved value differs by construction and means the same thing.
--   * $83 and $86 are the netcode's own per-frame cells (VOADV, VOTMP). They
--     are zero in a local build and will not be later; they are not sim state
--     in either.
--   * $84, $85 are the RAW paddle captures, written by the kernel from this
--     console's own controller before the two-tap filter lands the result in
--     $BB-$BE. The filtered cells are the state; the raw ones are the input.
--
-- What is left is the authoritative sim state: the frame counters, the scores,
-- the variation and its decoded flags, the dispatch group, the sprite control
-- cells, the Y positions, the filtered paddle positions and the triggers.
--
-- The ball's HORIZONTAL position is NOT in RAM at all -- it lives in the TIA
-- and is applied incrementally by HMOVE ($F65F writes HMM0) -- so no checksum
-- can see it. That is the same fact that rules out a state-push repair, and
-- the reason the desync repair is a restart.
local RANGES = {
    { 0x82, 0x82 },   -- a live flag byte
    { 0x88, 0xCC },   -- the frame counters, scores, variation, flags, the
                      --   dispatch group, positions, paddles and triggers
}

-- The frame counter the ROM increments once per frame at $F07B.
local FRCNT = 0x88

local sp = manager.machine.devices[":maincpu"].spaces["program"]
local frame = 0
local held = {}

-- CACHED ONCE. A field looked up through manager.machine.ioport.ports at the
-- moment of pressing it is a fresh wrapper, and set_value on a fresh wrapper is
-- lost -- the raw port never changes, and nothing reports an error.
local FIELDS = {}
local function field(tag, name)
    local key = tag .. "|" .. name
    if FIELDS[key] == nil then
        local p = manager.machine.ioport.ports[tag]
        FIELDS[key] = (p and p.fields[name]) or false
    end
    return FIELDS[key]
end

local function set(tag, name, on)
    local f = field(tag, name)
    if f then f:set_value(on and 1 or 0) end
end

-- THE PADDLES. Video Olympics is a paddle game, so run.sh puts `pad` in both
-- controller slots and the analog fields are:
--
--   :joyport1:pad:POTX "Paddle"    paddle 0   left port, player 0
--   :joyport1:pad:POTY "Paddle 2"  paddle 1   left port, player 1
--   :joyport2:pad:POTX "Paddle 3"  paddle 2   right port, player 2
--   :joyport2:pad:POTY "Paddle 4"  paddle 3   right port, player 3
--
-- which is exactly the 0/1-left, 2/3-right mapping the ROM assumes and the
-- netcode's role mapping follows.
local PADDLE = {
    { ":joyport1:pad:POTX", "Paddle" },
    { ":joyport1:pad:POTY", "Paddle 2" },
    { ":joyport2:pad:POTX", "Paddle 3" },
    { ":joyport2:pad:POTY", "Paddle 4" },
}

-- A position in the middle of travel, away from either end, so the kernel's
-- scanline count lands nowhere near a boundary.
local PADHOLD = 128

local function setpaddle(i, v)
    local f = field(PADDLE[i][1], PADDLE[i][2])
    if f then f:set_value(v) end
end

-- 1 IS A PRESS, for the console switches too. SWCHB is active low on the
-- hardware and MAME applies that inversion itself, so a harness that writes 0
-- to "press" SELECT is really releasing it, and the 1 it writes to "release"
-- is a press that then never ends.
--
-- DET_QUIET: start a game and then touch nothing.
--
-- It exists because injecting input through MAME's ports is NOT frame-exact
-- between two builds: the two boot through different amounts of code, so a
-- change can land on one side of one build's read and the other side of the
-- other's. A quiet run removes the variable -- the input streams are then
-- identical by construction, so any difference in state is the bank split's
-- and nothing else's -- and the whole frame loop, the timers, the collisions,
-- the sound and the kernel all still run.
local QUIET = os.getenv("DET_QUIET") ~= nil

local function drive(f)
    local want = {}

    -- HOLD ALL FOUR PADDLES AT A FIXED POSITION, ALWAYS -- including in a quiet
    -- run, and especially in a quiet run.
    --
    -- Hygiene, not a fix, and the distinction is worth the lines.
    --
    -- A determinism gate should not leave an analog input floating, so these
    -- are held. It does NOT resolve the divergence past frame 1560 that
    -- PORTING.md 3.20 describes -- the reading is $62 either way, and the
    -- oscillation between $62 and $64 continues at the same beat -- so the pot
    -- POSITION is not what drives it. That was worth finding out and is worth
    -- writing down, because the obvious next thing anybody tries is a different
    -- hold value.
    for i = 1, 4 do setpaddle(i, PADHOLD) end

    if f >= 40 and f < 48 then want[":SWB|Reset Game"] = true end
    if f >= 90 and not QUIET then
        -- Both paddles, on different periods, so the two players are never
        -- doing the same thing and a swap between them would show. A triangle
        -- wave rather than a ramp, because a paddle that wrapped from one end
        -- of travel to the other in a frame is not a thing a hand can do.
        local function tri(period, phase)
            local x = ((f + phase) % period) * 2
            if x >= period then x = period * 2 - x end
            return math.floor(x * 255 / period)
        end
        setpaddle(1, tri(180, 0))
        setpaddle(3, tri(140, 37))
        if (f // 53) % 3 == 0 then want[":joyport1:pad:JOY|P1 Button 1"] = true end
        if (f // 71) % 3 == 0 then want[":joyport2:pad:JOY|P3 Button 1"] = true end
    end
    for k in pairs(held) do
        if not want[k] then
            local tag, name = k:match("^(.-)|(.*)$")
            set(tag, name, false)
            held[k] = nil
        end
    end
    for k in pairs(want) do
        if not held[k] then
            local tag, name = k:match("^(.-)|(.*)$")
            set(tag, name, true)
            held[k] = true
        end
    end
end

-- INPUT IS DRIVEN FROM A FRAME NOTIFIER, NOT FROM THE MEMORY TAP BELOW.
-- set_value called from inside a tap is silently lost -- the tap runs in the
-- CPU's execution context and port state settles at frame boundaries -- so the
-- raw port never changes and nothing reports an error. A harness that pressed
-- RESET this way for a hundred runs never started a game once, and every
-- comparison still passed, because two builds sitting in attract mode agree
-- just as well as two builds playing.
_G._det_drive = emu.add_machine_frame_notifier(function()
    drive(frame)
end)

-- SAMPLED AT CXCLR, NOT AT VSYNC.
--
-- $F224 strobes CXCLR once a frame, immediately after the vertical blank's
-- timer runs out and before the kernel draws -- which is AFTER the whole
-- game-logic chain has run. VSYNC is the other end of the frame, and the sim
-- clock will move behind the stall gate, so a checksum taken at VSYNC would
-- read the frame counter one lower in the banked build than in stock. Nothing
-- in the game sees that; an observer at the wrong end of the frame does, and
-- reports a divergence that is entirely its own.
--
-- CXCLR is also right on its own merits: it is "the state this frame
-- computed", which is exactly what two consoles have to agree about.
_G._det_vs = sp:install_write_tap(0x2C, 0x2C, "cxclr", function(off, data, mask)
    -- The RAM clear is not a frame. It runs `STA $8D,X` with X = 0..255, which
    -- WRAPS INSIDE PAGE ZERO and therefore sweeps $0000-$007F as well -- and
    -- A7 clear there means TIA, so it STROBES CXCLR on its way past. The tap
    -- would see a frame that never happened with the state still zeroed.
    -- Skipping every frame where the counter reads zero costs one real sample
    -- per 256, symmetrically on both builds.
    if sp:readv_u8(FRCNT) == 0 then return end
    frame = frame + 1
    -- Rotate-and-add, not a plain sum: a plain sum cannot see two bytes that
    -- swapped, and two players that swapped is exactly the failure this is for.
    local c = 0
    for _, r in ipairs(RANGES) do
        for a = r[1], r[2] do
            c = ((c << 1) | (c >> 15)) & 0xFFFF
            c = (c + sp:readv_u8(a)) & 0xFFFF
        end
    end
    print(string.format("%d %04X", frame, c))
end)
