-- sess.lua -- where does the boot bank get to?
-- The session has no text yet, so its state IS the background colour. Tapping
-- COLUBK says which one it reached, and the mailbox says why it stopped.
local sp = manager.machine.devices[":maincpu"].spaces["program"]
local NAME = { [0x00]="BOOT", [0x84]="CONN", [0x1E]="WAIT", [0xC4]="PLAY", [0x44]="FAIL" }
local seen, order = {}, {}
_G._s1 = sp:install_write_tap(0x09, 0x09, "colubk", function(off, data, mask)
    local n = NAME[data]
    if n and not seen[n] then seen[n] = true; order[#order+1] = n
        print(string.format("STATE %s at %.2fs", n, manager.machine.time:as_double())) end
end)
_G._s2 = emu.add_machine_stop_notifier(function()
    print("STATES " .. table.concat(order, " -> "))
    print(string.format("MAILBOX ackseq=$%02X status=$%02X err=$%02X replycmd=$%02X rxlen=$%02X%02X",
        sp:readv_u8(0x1F00), sp:readv_u8(0x1F01), sp:readv_u8(0x1F02),
        sp:readv_u8(0x1F03), sp:readv_u8(0x1F05), sp:readv_u8(0x1F04)))
    local r = {}
    for i = 0, 15 do r[#r+1] = string.format("%02X", sp:readv_u8(0x1B00 + i)) end
    print("REPLY " .. table.concat(r, " "))
    print(string.format("VOENT=$%02X VOERR=$%02X", sp:readv_u8(0xE6), sp:readv_u8(0xE8)))
end)
