-- slack.lua -- how much vertical blank is left when the kernel starts waiting?
--
-- This is the number the whole netcode is budgeted against, and it is measured
-- rather than assumed. Combat has NO OVERSCAN -- its kernel runs to the last
-- line of the frame -- so the only place injected code can go is VOUT's wait
-- for the vertical-blank timer to expire, and how much of it there is depends
-- on what the game logic did that frame. A SELECT press runs ClearMem and
-- InitPF as well, and that is the worst frame in the game.
--
-- INTIM counts down every 64 cycles, so the value read AT THE GATE times 64 is
-- the cycles remaining. The gate itself is VOGATE = 8, chosen so that a step
-- worth about 310 cycles always fits in the 448 the gate guarantees.
--
--   ./run.sh combat slack

dofile(os.getenv("A2600_EMU") .. "/det.lua")

local GATE = 8
local sp = manager.machine.devices[":maincpu"].spaces["program"]
local cpu = manager.machine.devices[":maincpu"]

local first, hist, frames = {}, {}, 0
local steps, stepframes = 0, {}

-- The FIRST read of INTIM in a frame is the one that matters: it is the slack
-- the whole hook has to live inside. Later reads are the loop going round.
local seen_this_frame = false
_G._sl_intim = sp:install_read_tap(0x0284, 0x0284, "intim", function(off, data, mask)
    if not seen_this_frame then
        seen_this_frame = true
        hist[data] = (hist[data] or 0) + 1
    end
end)

-- CXCLR is strobed once a frame, right after the wait ends.
_G._sl_frame = sp:install_write_tap(0x2C, 0x2C, "cxclr", function(off, data, mask)
    frames = frames + 1
    seen_this_frame = false
end)

_G._sl_stop = emu.add_machine_stop_notifier(function()
    local keys, total, n, lo, hi = {}, 0, 0, 255, 0
    for k, v in pairs(hist) do
        keys[#keys + 1] = k; total = total + k * v; n = n + v
        if k < lo then lo = k end
        if k > hi then hi = k end
    end
    table.sort(keys)
    local out, below = {}, 0
    for _, k in ipairs(keys) do
        out[#out + 1] = string.format("%d:%d", k, hist[k])
        if k < GATE then below = below + hist[k] end
    end
    print("FRAMES " .. frames)
    print("INTIM AT THE GATE " .. table.concat(out, " "))
    if n > 0 then
        print(string.format(
            "SLACK min=%d (%d cycles)  mean=%.1f (%d cycles)  max=%d (%d cycles)",
            lo, lo * 64, total / n, math.floor(total / n) * 64, hi, hi * 64))
        print(string.format(
            "FRAMES BELOW THE GATE (%d): %d of %d (%.1f%%) -- these run no "
            .. "network step at all, which is the right answer: a tick may be a "
            .. "frame late and the lockstep is built to absorb exactly that.",
            GATE, below, n, 100.0 * below / n))
    end
end)
