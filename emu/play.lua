-- play.lua -- the rig with its HANDS ON THE STICK.
--
-- emu/rig.lua presses RESET and SELECT and nothing else, and every gate it runs
-- is green. A person then started a game and the two consoles came apart inside
-- a couple of seconds, with the relay reporting thousands of mismatches it had
-- never reported in a rig run. PORTING.md 4.18 said a harness that presses
-- nothing proves nothing; this is the same lesson one notch further in. THE RIG
-- HAD NEVER MOVED A PADDLE, so the only inputs it ever exercised were the two
-- that are ANDed on the wire and identical on both machines by construction.
--
-- So this one plays. Each console drives its own LEFT port -- which is what a
-- player's hands do -- on a schedule that is DELIBERATELY DIFFERENT on the two
-- machines and deliberately NOT aligned to anything either console shares.
--
-- That asynchrony is not sloppiness, it is the property under test. A real
-- player's thumb has no idea what tick it is. The design's whole claim is that
-- it does not need to: each console captures its OWN stick whenever it likes,
-- stamps the capture with a tick, and the peer applies exactly the byte that
-- was sent. If asynchronous input can desync the pair, that is a bug in the
-- ROM and not an artefact of the harness -- which is the opposite of the
-- situation emu/det.lua is in, where two BUILDS have to see identical bytes.
--
-- It prints one line per tick of the authoritative sim state, sampled at the
-- one instant the two consoles are comparable, for tools/playdiff.py to line up
-- and diff. The point is not to know THAT they diverged -- the relay says that
-- already -- but WHICH CELL went first, and at which tick.

-- Keep in step with src/vodefs.inc.
local VOENT, VOTICK, VOERR, VONST = 0xD3, 0xD6, 0xD5, 0xD7
-- This game's frame counter, which now increments only on a frame the sim
-- advanced -- see the $F07A patch. It stands in for Combat's CLOCK.
local CLOCK = 0x88

local sp = manager.machine.devices[":maincpu"].spaces["program"]

-- CACHED ONCE: a field looked up at the moment of pressing it is a fresh
-- wrapper and set_value on it is lost.
local FIELDS = {}
local function field(k)
    if FIELDS[k] == nil then
        local tag, name = k:match("^(.-)|(.*)$")
        local p = manager.machine.ioport.ports[tag]
        FIELDS[k] = (p and p.fields[name]) or false
    end
    return FIELDS[k]
end

local held = {}
local function apply(want)
    for k in pairs(held) do
        if not want[k] then
            local f = field(k); if f then f:set_value(0) end
            held[k] = nil
        end
    end
    for k in pairs(want) do
        if not held[k] then
            local f = field(k); if f then f:set_value(1) end
            held[k] = true
        end
    end
end

local ticks, lasttick, frames = 0, nil, 0

-- THE PADDLE, not a stick. run.sh puts `pad` in both controller slots, and a
-- console drives its OWN left-port paddle A -- which is player 0 on the host
-- and player 2 on the guest, exactly as the netcode's role mapping says.
local PADDLE = { ":joyport1:pad:POTX", "Paddle" }
local TRIG = ":joyport1:pad:JOY|P1 Button 1"

local function setpaddle(v)
    local p = manager.machine.ioport.ports[PADDLE[1]]
    local f = p and p.fields[PADDLE[2]]
    if f then f:set_value(v) end
end

-- INPUT IS DRIVEN FROM A FRAME NOTIFIER, NEVER FROM A MEMORY TAP.
_G._pl_drive = emu.add_machine_frame_notifier(function()
    frames = frames + 1
    local raw = sp:readv_u8(VOTICK)
    if lasttick == nil then ticks = raw
    elseif raw ~= lasttick then ticks = ticks + ((raw - lasttick) & 0xFF) end
    lasttick = raw

    local ent = sp:readv_u8(VOENT)
    local in_match = (ent & 0x02) ~= 0
    local host = in_match and (ent & 0x04) == 0

    local want = {}
    if not in_match then return end

    -- The host starts the game, once, the way a person would.
    if host and ticks >= 30 and ticks < 40 then
        want[":SWB|Reset Game"] = true
    end

    -- Then both players play. KEYED TO THE EMULATOR'S FRAME COUNTER, not to any
    -- tick: this is a hand, and a hand is asynchronous. The two consoles get
    -- different periods so the two players are never doing the same thing and
    -- a swap between them would show as plainly as a desync.
    --
    -- A TRIANGLE, not a ramp or a random walk. A paddle that jumped from one
    -- end of its travel to the other in a frame is not a thing a hand can do,
    -- and the ROM low-pass filters the reading anyway ($F643), so a
    -- discontinuity would be smoothed differently depending on where in the
    -- kernel it landed -- a harness artefact that looks exactly like a desync.
    if ticks >= 45 then
        local period = host and 180 or 140
        local phase = host and 0 or 37
        local x = ((frames + phase) % period) * 2
        if x >= period then x = period * 2 - x end
        setpaddle(math.floor(x * 255 / period))
        local b = host and (frames // 41) % 3 or (frames // 31) % 3
        if b == 0 then want[TRIG] = true end
    end
    apply(want)
end)

-- One line per tick, at the phase-0 frame -- the only instant at which two
-- consoles are comparable, and the same instant VOCRC is sampled at. The phase
-- comes from VOENT because CLOCK is a cell ClrGam rewrites.
--
-- THE LINE IS NUMBERED BY COUNTING BOUNDARIES HERE, not by the extended tick
-- the frame notifier maintains. This tap fires exactly once per tick, on the
-- frame that completed the boundary -- a stalled boundary leaves the phase at
-- zero and does not print -- so a count kept in the tap IS the tick, with no
-- wrap and no sampling error.
--
-- The notifier's counter is a video-frame sample of VOTICK, and two consoles in
-- simulation lockstep are not in wall-clock lockstep, so it labels a boundary
-- one out whenever the notifier last ran on the other side of it. Two lines
-- then collide on one number, one number goes missing, and the diff reports
-- every downstream cell as divergent: at the first "divergence" CLOCK differed
-- by EXACTLY FOUR, which is one tick of sim frames and the signature of a
-- mislabelled line rather than of a desync. Same lesson as the variation trace,
-- one file along: anything the harness measures with its own clock, it is
-- measuring wrong.
-- PLAY_WINDOW=lo,hi: every FRAME in that tick range, not every tick. A tick is
-- four frames and the thing being chased is a frame-count divergence, so the
-- per-tick dump can only ever say that one happened, never where.
local wlo, whi
do
    local w = os.getenv("PLAY_WINDOW")
    if w then wlo, whi = w:match("^(%d+),(%d+)$") end
    wlo, whi = tonumber(wlo or ""), tonumber(whi or "")
end

-- PLAY_INJECT=<tick>: deliberately corrupt this console's simulation at that
-- tick, on CONSOLE 1 ONLY (PLAY_INJECT is passed to one of them). One added to
-- TankY0 is the smallest desync there is -- one scanline -- and it is exactly
-- the kind a real one starts as.
--
-- This is the only way to test a REPAIR. Detection can be tested by waiting for
-- a bug; recovery cannot, because a correct pair never diverges. So the harness
-- has to break one on purpose, and then assert that the two consoles come back
-- together on their own.
local inject = tonumber(os.getenv("PLAY_INJECT") or "")
local injected = false

-- THE REPAIR PATH, STEP BY STEP. Recovery took 8 ticks in one run and 232 in
-- another, and "it came back eventually" is not a diagnosis. These three taps
-- say which of the four steps is slow: the mismatch being NOTICED (VOE_RSY
-- goes up), the press being MADE (VOE_RSY comes down, in VOCAP), the press
-- reaching the game's own switch shadow, and the game acting on it.
local rsy = false
_G._pl_rsy = sp:install_write_tap(VOENT, VOENT, "voent", function(off, data, mask)
    local now = (data & 0x20) ~= 0
    if now ~= rsy then
        rsy = now
        print(string.format("RSY %s t%d raw%d ph%02X",
            now and "ARMED" or "PRESSED", ticks, sp:readv_u8(VOTICK),
            sp:readv_u8(VOENT) & 0xC0))
    end
end)

-- The mixed switch byte the game actually reads. Bit 0 is RESET, active low,
-- so a zero there is the press arriving -- from either console's wire byte.
local lastswb = nil
_G._pl_swb = sp:install_write_tap(0xD2, 0xD2, "voswb", function(off, data, mask)
    local down = (data & 0x01) == 0
    local was = lastswb
    lastswb = down
    if down and was ~= true then
        print(string.format("RESET-ON-WIRE t%d raw%d", ticks, sp:readv_u8(VOTICK)))
    end
end)

local nb = 0
_G._pl = sp:install_write_tap(0x2C, 0x2C, "cxclr", function(off, data, mask)
    if wlo and ticks >= wlo and ticks <= whi then
        -- adv is VOADV, the gate's own verdict for this frame: $FF ran the
        -- logic chain, $00 stalled. err's high nibble is the stall run-length.
        -- The wire, not just the consequence. loc/ring are the exact two
        -- bytes VOMIX combined for this tick -- the local ring slot the
        -- capture of d ticks ago went into, and the remote slot the peer's
        -- record for this tick went into -- and swb is what came out. Two
        -- consoles at the same CLOCK with different swb have been handed
        -- different inputs, and these say which side handed them over.
        local t = sp:readv_u8(VOTICK)
        local ring, loc, pad = {}, {}, {}
        -- BOTH RINGS IN FULL, two bytes a slot: paddle then switches.
        for i = 0, 15 do
            ring[#ring + 1] = string.format("%02X", sp:readv_u8(0xDA + i))
        end
        for i = 0, 7 do
            loc[#loc + 1] = string.format("%02X", sp:readv_u8(0xEA + i))
        end
        for i = 0, 3 do
            pad[#pad + 1] = string.format("%02X", sp:readv_u8(0xCD + i))
        end
        print(string.format(
            "F t%d raw%d ph%02X adv%02X err%02X fr%02X var%02X swa%02X swb%02X rwat%d pad%s | L%s R%s",
            ticks, t, sp:readv_u8(VOENT) & 0xC0,
            sp:readv_u8(0x83), sp:readv_u8(VOERR), sp:readv_u8(CLOCK),
            sp:readv_u8(0x96), sp:readv_u8(0xD1), sp:readv_u8(0xD2),
            sp:readv_u8(0xD9), table.concat(pad, ""),
            table.concat(loc, " "), table.concat(ring, " ")))
    end
    if (sp:readv_u8(VOENT) & 0xC0) ~= 0x40 then return end
    nb = nb + 1
    if inject and not injected and nb >= inject then
        injected = true
        -- $8D IS A SCORE, and the choice matters more than it looks.
        --
        -- A Y position was the obvious pick and it is useless: $B2 is
        -- recomputed from the paddle every frame, so a corruption there is
        -- gone by the next tick without anything having repaired it. The
        -- harness duly reported "recovered after 1 tick" and the relay had
        -- seen nothing at all -- a test that passes itself.
        --
        -- A score PERSISTS. It is in the checksum, nothing rewrites it, and
        -- one is the smallest desync there is: exactly the size a real one
        -- starts at.
        sp:write_u8(0x8D, (sp:readv_u8(0x8D) + 1) & 0xFF)   -- Score0
        print(string.format("INJECT tick %d: TankY0 nudged by one scanline", nb))
    end
    local b = {}
    -- $80-$B6 is the whole of the game's own working set that both consoles
    -- must agree about, plus VOPAD at $CD-$D0, which is what they compute from
    -- the same two wire bytes.
    --
    -- $BB-$BE are DELIBERATELY ABSENT. They are the filtered LOCAL paddle
    -- positions, written by the display kernel from this console's own
    -- controller, and with two hands on two paddles they differ on every
    -- single tick of a perfectly synchronised pair.
    for a = 0x80, 0xB6 do
        -- $84 and $85 are the kernel's RAW captures of this console's own two
        -- paddles, before the filter. Local input, like $BB-$BE, and they
        -- differ between two consoles for the same reason.
        if a ~= 0x84 and a ~= 0x85 then
            b[#b + 1] = string.format("%02X", sp:readv_u8(a))
        end
    end
    for a = 0xCD, 0xD0 do b[#b + 1] = string.format("%02X", sp:readv_u8(a)) end
    -- The ROM's own raw tick rides along so the two numbering schemes can be
    -- checked against each other rather than trusted.
    print(string.format("S %d %d %s", nb, sp:readv_u8(VOTICK),
                        table.concat(b, "")))
end)

_G._pl_stop = emu.add_machine_stop_notifier(function()
    print(string.format("PLAY tick=%d err=$%02X state=%d ent=$%02X frames=%d",
        ticks, sp:readv_u8(VOERR), sp:readv_u8(VONST), sp:readv_u8(VOENT),
        frames))
end)
