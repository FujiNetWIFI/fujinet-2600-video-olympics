-- rawdump.lua -- a window of frames' zero page, byte for byte.
-- A checksum says two runs differ; this says which cell.
--   RAW_FROM=205 RAW_FRAMES=212 ./run.sh vo rawdump
-- It needs det.lua's input stream to reach anything interesting, so it loads
-- it: a divergence that only happens once a tank is moving cannot be found by
-- a run where nothing is pressed.
dofile(os.getenv("A2600_EMU") .. "/det.lua")
local sp = manager.machine.devices[":maincpu"].spaces["program"]
local frame = 0
local N = tonumber(os.getenv("RAW_FRAMES") or "3")
-- THE SAME SKIP det.lua USES, and it has to be. The RAM clear sweeps the TIA
-- on its way past and strobes CXCLR, so one strobe per 256 frames is not a
-- frame -- but the real reason this is here is that a tool which numbers frames
-- DIFFERENTLY from the gate it is explaining is worse than no tool at all.
-- Without it, "first divergence at F1632" and ramdiff's "diverged at frame
-- 1624" are two different frames, and an afternoon goes into reconciling a
-- contradiction that was never in the ROM.
_G._rd = sp:install_write_tap(0x2C, 0x2C, "cxclr", function(off, data, mask)
    if sp:readv_u8(0x88) == 0 then return end
    frame = frame + 1
    local from = tonumber(os.getenv("RAW_FROM") or "1")
    if frame < from or frame > N then return end
    local out = {}
    for a = 0x80, 0xFF do out[#out + 1] = string.format("%02X", sp:readv_u8(a)) end
    print(string.format("F%d %s", frame, table.concat(out, " ")))
end)
