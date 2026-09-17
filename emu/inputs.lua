-- inputs.lua -- every read of the console's input ports, with the PC that did it.
--
-- This is the 2600 form of the jzIntv write-watch that found Baseball's hidden
-- third input surface. The disassembly says this game's whole input surface is
-- seven reads; this is what turns that from a claim into a measurement, and it
-- is how the patch map is proved COMPLETE rather than merely plausible.
--
-- WHAT A PASS LOOKS LIKE HERE IS NOT WHAT IT LOOKED LIKE IN COMBAT, and the
-- difference is the whole shape of this port:
--
--   * SWCHA and SWCHB must be read from ONE place each -- VOSHIM, the mirror.
--     The game's own six sites are patched and must be gone.
--
--   * INPT0 and INPT2 must STILL be read from the display kernel, at $F62C and
--     $F633. That is not a miss. The paddle is sampled inside the kernel by
--     counting scanlines until the controller's capacitor charges, and that
--     capture is how this console learns its OWN paddle position -- which is
--     what it then sends. Patching it away would leave nothing to transmit.
--     The netcode intercepts the CONSUMER at $F1B4 instead.
--
-- THE MIRRORS MATTER. The TIA decodes only A0-A3 for a read, so INPT0 answers
-- at $08, $18, $28, $38 and so on, and this ROM uses $38/$3A. A tap placed
-- only on $08-$0B reports no paddle reads at all, which reads exactly like a
-- game that does not use paddles -- and Combat's recon.py does precisely that.
--
-- It also logs the value, keyed to the game's own frame counter, so two builds'
-- input streams can be compared directly -- which is the only way to tell an
-- input-timing artefact in the harness from a real divergence in the game.
--
--   ./run.sh vo inputs 2>/dev/null | grep '^IN ' > build/in_split.txt

dofile(os.getenv("A2600_EMU") .. "/det.lua")

local CLOCK = 0x88              -- the frame counter at $F07B
local PORTS = { [0x0280] = "SWCHA", [0x0282] = "SWCHB",
                [0x0038] = "INPT0", [0x003A] = "INPT2",
                [0x0008] = "INPT0(lo)", [0x000A] = "INPT2(lo)",
                [0x000C] = "INPT4", [0x000D] = "INPT5" }

local sp = manager.machine.devices[":maincpu"].spaces["program"]
local cpu = manager.machine.devices[":maincpu"]
local sites = {}
local frame, lastclock = 0, nil

for addr, name in pairs(PORTS) do
    _G["_in_" .. name] = sp:install_read_tap(addr, addr, name,
        function(off, data, mask)
            local pc = cpu.state["PC"].value
            -- The read instruction started three bytes back for an absolute
            -- read, two for zero-page; report where the operand is so it lines
            -- up with the patch map's addresses.
            sites[name .. " @" .. string.format("$%04X", pc)] =
                (sites[name .. " @" .. string.format("$%04X", pc)] or 0) + 1
            print(string.format("IN %d %s %02X %04X", frame, name, data, pc))
        end)
end

_G._in_vs = sp:install_write_tap(0x2C, 0x2C, "cxclr", function(off, data, mask)
    local c = sp:readv_u8(CLOCK)
    if lastclock == nil then frame = c
    elseif c ~= lastclock then frame = frame + ((c - lastclock) & 0xFF) end
    lastclock = c
end)

_G._in_stop = emu.add_machine_stop_notifier(function()
    local keys = {}
    for k in pairs(sites) do keys[#keys + 1] = k end
    table.sort(keys)
    print("SITES " .. #keys)
    for _, k in ipairs(keys) do print(string.format("  %-22s %d reads", k, sites[k])) end
end)
