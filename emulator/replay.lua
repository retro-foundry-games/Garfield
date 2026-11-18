-- Replays the joystick events that autolock.lua printed ("events = {{frame,value},...}") from the start, port against
-- reference at every sync point, to reproduce a failure without the exploration that led to it.
--
-- usage: garfield-dev emulator/replay.lua <autolock log> [extra frames after the last event, default 60]
local log, extra = args[1], tonumber(args[2]) or 60
assert(log, "usage: replay.lua <autolock log> [extra frames]")
local text
for line in io.lines(log) do
  local t = line:match("^events = (.*)$")
  if t then text = t end
end
assert(text, "no events line in " .. log)
local events = assert(load("return " .. text))()

local SYNC_PC = { [0x1089A] = 0, [0x172F4] = 1, [0x103B4] = 2 }
local MASKED = { { 0x44DA6, 0x450A6 } }   -- the program's stack: the port has none
local last_frame = events[#events][1] + extra
local next_event, joy, arrivals = 1, 0, 0

port.on_sync(function(id)
  arrivals = arrivals + 1
  if not emu.run("ref") then print("reference halted"); port.stop(); return end
  local pc = emu.pc("ref")
  if SYNC_PC[pc] ~= id then
    print(string.format("CONTROL FLOW: the port is at sync %d, the reference at pc=%06x (sync %s), frame %d", id, pc, tostring(SYNC_PC[pc]), emu.frames("ref")))
    port.stop(); return
  end
  local diffs = compare.ram("port", "ref", MASKED)
  if #diffs > 0 then
    print(string.format("MISMATCH at arrival %d (sync %d, frame %d): %d ranges", arrivals, id, emu.frames("ref"), #diffs))
    for i = 1, math.min(#diffs, 40) do
      local d = diffs[i]
      print(string.format("  %06x..%06x (%s): port=%02x ref=%02x", d.lo, d.hi, d.field, d.a, d.b))
    end
    port.stop(); return
  end
  if compare.palette("ref") > 0 then print(string.format("PALETTE MISMATCH at arrival %d, frame %d", arrivals, emu.frames("ref"))); port.stop(); return end
  local f = emu.frames("ref")
  if f >= last_frame then
    print(string.format("port and reference identical at %d sync points over %d frames", arrivals, f))
    port.stop(); return
  end
  if id == 2 then
    while events[next_event] and events[next_event][1] <= f do joy = events[next_event][2]; next_event = next_event + 1 end
  end
  port.joy(joy); emu.joy("ref", joy)
  emu.vsync("ref")
end)
port.run("extracted")
quit(0)
