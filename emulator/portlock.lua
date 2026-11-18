-- Port vs. reference, in lockstep: the port runs the game in C; at every point where the original takes an
-- interrupt the reference runs to the same point and all of memory is compared.
--
-- usage: garfield-dev emulator/portlock.lua [frames]
local frames = tonumber(args[1]) or 50

local SYNC_PC = { [0x1089A] = 0, [0x172F4] = 1, [0x103B4] = 2 }   -- wait for a tick, wait for a fade, main loop top
-- Left out of the comparison: the program's stack (the port has none).
local MASKED = { { 0x44DA6, 0x450A6 } }   -- the program's stack: the port has none

local phases = { 0x08, 0x88, 0x04, 0x01, 0x02, 0x09 }
local function joy(frame)
  if frame < 30 then return 0 end
  return phases[(frame // 150) % #phases + 1]
end

local arrivals = 0
port.on_sync(function(id)
  arrivals = arrivals + 1
  if not emu.run("ref") then print("reference halted"); port.stop(); return end
  local pc = emu.pc("ref")
  if SYNC_PC[pc] ~= id then
    print(string.format("CONTROL FLOW: the port is at sync %d, the reference at pc=%06x (sync %s)", id, pc, tostring(SYNC_PC[pc])))
    port.stop(); return
  end
  local diffs = compare.ram("port", "ref", MASKED)
  if #diffs > 0 then
    print(string.format("MISMATCH at arrival %d (sync %d, frame %d): %d ranges", arrivals, id, emu.frames("ref"), #diffs))
    for i = 1, math.min(#diffs, 16) do
      local d = diffs[i]
      print(string.format("  %06x..%06x (%s): port=%02x ref=%02x", d.lo, d.hi, d.field, d.a, d.b))
    end
    port.stop(); return
  end
  local colours = compare.palette("ref")
  if colours > 0 then print(string.format("PALETTE MISMATCH at arrival %d: %d colours", arrivals, colours)); port.stop(); return end
  if emu.frames("ref") >= frames then
    print(string.format("port and reference identical at %d sync points over %d frames", arrivals, emu.frames("ref")))
    port.stop(); return
  end
  local j = joy(emu.frames("ref") + 1)
  port.joy(j); emu.joy("ref", j)
  emu.vsync("ref")
end)
port.run("extracted")
quit(0)
