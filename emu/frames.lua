-- frames.lua -- every frame must be the same length, through bank switches.
--
-- Not by counting frames spent in a bank: that measures where the program is,
-- not whether anything is on screen. Not by sampling the screen either --
-- screen:pixel() reads a bitmap MAME updates on its own schedule. What is
-- unambiguous is the program's own VSYNC. Tap the write, measure the gap to
-- the previous one, and a frame of the wrong length is a frame that moved the
-- picture.
--
-- Combat is NOT 262 lines: it has no overscan, its kernel runs to the last
-- line of the frame, and the stock game measures 259. So this asserts a
-- CONSTANT, learned from the run, rather than a number from a specification --
-- and it reports the constant, because that number changing between builds is
-- itself the finding.
--
--   ./run.sh combat frames

local SECS = tonumber(os.getenv("SECS") or "20")
local FRAME = 1 / 59.92
local LINE  = FRAME / 262          -- a scanline is a scanline whatever the frame

local sp = manager.machine.devices[":maincpu"].spaces["program"]
local hist, n, last = {}, 0, nil
local badat = {}
local banks, bankseq = {}, {}
local switches = 0

local function report()
    local keys = {}
    for k in pairs(hist) do keys[#keys + 1] = k end
    table.sort(keys, function(a, b) return hist[a] > hist[b] end)
    local mode = keys[1]
    local out, bad = {}, 0
    table.sort(keys)
    for _, k in ipairs(keys) do
        out[#out + 1] = string.format("%d:%d", k, hist[k])
        if k ~= mode then bad = bad + hist[k] end
    end
    print(string.format("LINES %s", table.concat(out, " ")))
    print(string.format("FRAMES %d, mode %d lines, %d not the mode", n, mode, bad))
    local bs = {}
    for k, v in pairs(banks) do bs[#bs + 1] = string.format("%d:%d", k, v) end
    table.sort(bs)
    print(string.format("BANK SWITCHES %d (%.2f per frame)  to %s",
                        switches, n > 0 and switches / n or 0, table.concat(bs, " ")))
    -- The boot bank paints its own frames while it opens a socket and waits
    -- for an opponent, and the one frame in which it hands over to the game
    -- bank belongs to neither. A transient there is expected; one anywhere
    -- else is the thing this gate exists to catch.
    local late = 0
    for _, b in ipairs(badat) do
        if b.lines ~= mode and b.t > 3.0 then
            late = late + 1
            if late <= 5 then
                print(string.format("BAD FRAME: %d lines at %.2fs", b.lines, b.t))
            end
        end
    end
    if bad > 0 and late == 0 then
        print(string.format("%d odd frame(s), all in the first 3s -- the boot "
                            .. "bank handing over", bad))
    end
    if late == 0 and n > 100 then print("FRAMES PASS") else print("FRAMES FAIL") end
end

-- FN_HOT_BANK is $1D80+b, a one-shot: one store, bank in the address.
_G._fr_bank = sp:install_write_tap(0x1D80, 0x1D8F, "bank", function(off, data, mask)
    local b = off - 0x1D80
    banks[b] = (banks[b] or 0) + 1
    switches = switches + 1
end)

_G._fr_vs = sp:install_write_tap(0x00, 0x00, "vsync", function(off, data, mask)
    if (data & 0x02) == 0 then return end
    local t = manager.machine.time:as_double()
    if last then
        local lines = math.floor((t - last) / LINE + 0.5)
        hist[lines] = (hist[lines] or 0) + 1
        n = n + 1
        badlines = lines
        badat[#badat + 1] = { t = t, lines = lines }
        if #badat > 400 then table.remove(badat, 1) end
    end
    last = t
end)

_G._fr_stop = emu.add_machine_stop_notifier(function() report() end)
