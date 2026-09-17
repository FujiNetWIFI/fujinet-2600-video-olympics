-- swtest.lua -- can a Lua harness press this console's switches at all?
local sp  = manager.machine.devices[":maincpu"].spaces["program"]
local n = 0
local swb = manager.machine.ioport.ports[":SWB"]
print("PORT :SWB resolves: " .. tostring(swb ~= nil))
if swb then
    for name, f in pairs(swb.fields) do
        print("  field " .. name)
    end
end
local RST = swb and swb.fields["Reset Game"]
local SEL = swb and swb.fields["Select Game"]
print("RST resolves: " .. tostring(RST ~= nil) .. "  SEL: " .. tostring(SEL ~= nil))

_G._t = emu.add_machine_frame_notifier(function()
    n = n + 1
    if n == 60 then
        print(string.format("before press: SWCHB=$%02X", sp:readv_u8(0x0282)))
        if RST then RST:set_value(1) end
    elseif n == 65 then
        print(string.format("during press: SWCHB=$%02X", sp:readv_u8(0x0282)))
    elseif n == 70 then
        if RST then RST:set_value(0) end
    elseif n == 75 then
        print(string.format("after release: SWCHB=$%02X", sp:readv_u8(0x0282)))
        manager.machine:exit()
    end
end)
