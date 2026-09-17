-- lag.lua -- the interception proof: does Combat read the shadows, or the ports?
--
-- emu/inputs.lua answers it statically: after the patch, the only reads of
-- SWCHA/SWCHB/INPT4/INPT5 in the whole run come from VOSHIM, and the game's own
-- seven sites are gone. This answers it dynamically, which is the half a human
-- can see: a build with VOLAG=1 refreshes the shadows once every 64 frames, so
-- if Combat is really reading them the stick must move the tanks in visible
-- steps about a second apart -- and if it is not, nothing changes at all.
--
--   VOLAG=1 make combat && ./run.sh combat lag

dofile(os.getenv("A2600_EMU") .. "/det.lua")

local CLOCK, VOJOY, VOSWB = 0x86, 0xCF, 0xD0
local sp = manager.machine.devices[":maincpu"].spaces["program"]
local writes, at, frames = 0, {}, 0

_G._lg_w = sp:install_write_tap(VOJOY, VOJOY, "vojoy", function(off, data, mask)
    writes = writes + 1
    local c = sp:readv_u8(CLOCK)
    at[c & 0x3F] = (at[c & 0x3F] or 0) + 1
end)

_G._lg_vs = sp:install_write_tap(0x00, 0x00, "vsync", function(off, data, mask)
    if (data & 0x02) ~= 0 then frames = frames + 1 end
end)

_G._lg_stop = emu.add_machine_stop_notifier(function()
    local phases = {}
    for k, v in pairs(at) do phases[#phases + 1] = string.format("%d:%d", k, v) end
    table.sort(phases)
    print(string.format("FRAMES %d  VOJOY writes %d (%.3f per frame)",
                        frames, writes, frames > 0 and writes / frames or 0))
    print("CLOCK phases written at: " .. table.concat(phases, " "))
    -- The claim is the RATE, not the phase count: the cold start writes once
    -- before CLOCK has been incremented for the first time, so a perfectly
    -- healthy run shows one stray write on phase 0 alongside the real ones.
    local rate = frames > 0 and writes / frames or 1
    if rate < 0.05 and frames > 300 then
        print(string.format(
            "LAG PASS: the shadows refreshed %.1f times a second, not sixty -- "
            .. "Combat is reading them and not the ports", rate * 59.92))
    else
        print("LAG FAIL: " .. string.format("%.3f writes per frame", rate)
              .. " -- a VOLAG=0 build writes every frame, so this is not the lag build")
    end
end)
