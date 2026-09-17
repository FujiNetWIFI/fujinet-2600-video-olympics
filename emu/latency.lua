-- latency.lua -- M1: what does one FujiNet mailbox transaction cost, in frames?
--
-- Every latency number in this port hangs off that figure. PORTING.md §2 is
-- emphatic that it must be measured: the Intellivision family believed a
-- documented 30 Hz tick for years, it was 10, and every estimate built on it
-- was three times too optimistic.
--
-- probe.asm publishes a step id into VOST and the frames that step took into
-- VOFCNT. A step id is EVEN when a transaction is launched and ODD when it
-- came back, so a write of an odd id is the moment to read the frame count.
-- Nothing has to be decoded off the screen.
--
--   ./run.sh probe latency

-- Kept in step with the PSTEP/PROUND/VOFCNT equates in
-- src/probe.asm. A drifted address here reads a cell nothing writes and
-- reports a probe that never ran.
local VOST, VOERR, VOFCNT = 0xF7, 0xD5, 0xFA
local PROUND = 0xF8

local NAME = { [3] = "OPEN", [5] = "WRITE", [7] = "STATUS", [9] = "READ" }

local sp = manager.machine.devices[":maincpu"].spaces["program"]
local hist, order = {}, {}
local frames, fails = 0, 0

local function note(kind, n)
    local h = hist[kind]
    if not h then
        h = { n = 0, sum = 0, min = 1e9, max = 0, bins = {} }
        hist[kind] = h
        order[#order + 1] = kind
    end
    h.n = h.n + 1
    h.sum = h.sum + n
    if n < h.min then h.min = n end
    if n > h.max then h.max = n end
    h.bins[n] = (h.bins[n] or 0) + 1
end

local function report()
    local rounds = sp:readv_u8(PROUND) + 256 * sp:readv_u8(PROUND + 1)
    print(string.format("FRAMES %d  ROUNDS %d  ERR $%02X",
                        frames, rounds, sp:readv_u8(VOERR)))
    for _, kind in ipairs(order) do
        local h = hist[kind]
        local bins, keys = {}, {}
        for k in pairs(h.bins) do keys[#keys + 1] = k end
        table.sort(keys)
        for _, k in ipairs(keys) do
            bins[#bins + 1] = string.format("%d:%d", k, h.bins[k])
        end
        print(string.format("%-7s n=%-5d mean=%.2f min=%d max=%d   %s",
                            kind, h.n, h.sum / h.n, h.min, h.max,
                            table.concat(bins, " ")))
    end
    -- One lockstep tick is WRITE + STATUS + READ. This is the number that
    -- decides K and d, and therefore whether this is playable at all.
    local tick = 0
    for _, kind in ipairs({ "WRITE", "STATUS", "READ" }) do
        if hist[kind] then tick = tick + hist[kind].sum / hist[kind].n end
    end
    if tick > 0 then
        print(string.format("TICK %.1f frames for WRITE+STATUS+READ = %.1f Hz",
                            tick, 59.92 / tick))
    end
    if fails > 0 then print(string.format("FAILS %d", fails)) end
end

_G._lat_vs = sp:install_write_tap(0x00, 0x00, "vsync", function(off, data, mask)
    if (data & 0x02) ~= 0 then frames = frames + 1 end
end)

_G._lat_st = sp:install_write_tap(VOST, VOST, "step", function(off, data, mask)
    if data == 0xFF then
        fails = fails + 1
        -- Dump the cartridge's whole status page and the head of the reply
        -- window. The ROM has one byte to report a failure with; the mailbox
        -- has the actual reason, and reading it here costs the ROM nothing.
        local r = {}
        for i = 0, 5 do r[#r + 1] = string.format("%02X", sp:readv_u8(0x1B00 + i)) end
        print(string.format(
            "FAIL frame %d  VOERR=$%02X  ACKSEQ=$%02X STATUS=$%02X ERR=$%02X " ..
            "REPLYCMD=$%02X RXLEN=$%02X%02X  reply[0..5]=%s",
            frames, sp:readv_u8(VOERR),
            sp:readv_u8(0x1F00), sp:readv_u8(0x1F01), sp:readv_u8(0x1F02),
            sp:readv_u8(0x1F03), sp:readv_u8(0x1F05), sp:readv_u8(0x1F04),
            table.concat(r, " ")))
        return
    end
    -- Odd ids are completions; the count in VOFCNT belongs to the launch that
    -- preceded it. Reading it on the launch instead would read the PREVIOUS
    -- transaction's count, which is the same mistake net.inc warns about for
    -- FNRXLO/FNRXHI.
    if data % 2 == 1 and NAME[data] then note(NAME[data], sp:readv_u8(VOFCNT)) end
end)

_G._lat_stop = emu.add_machine_stop_notifier(function() report() end)
