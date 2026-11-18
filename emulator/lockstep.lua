-- Lockstep: pure reference emulator vs. hybrid (lifted C routines replacing original code).
-- usage: garfield-dev emulator/lockstep.lua [frames]
local frames = tonumber(args[1]) or 300
local SNAP_EVERY, RING = 25, 8

-- Scripted joystick, identical on both sides: phases of 150 frames.
local phases = { 0x08, 0x88, 0x04, 0x01, 0x02, 0x09 }
local function joy(frame)
  if frame < 30 then return 0 end
  return phases[(frame // 150) % #phases + 1]
end

local function snapshot(ring, f)
  if #ring == RING then
    local old = table.remove(ring, 1)
    emu.free(old.ref); emu.free(old.hyb)
  end
  ring[#ring + 1] = { frame = f, ref = emu.save("ref"), hyb = emu.save("hyb") }
end

-- Re-runs one machine from a snapshot, reporting every write to addr (with callers) up to frame `to`.
local function replay(which, snap, to, addr)
  emu.restore(which, snap[which])
  local seen = 0
  -- The whole bus is watched: writes arrive at wider sizes, and through the 24-bit aliases above 16 MB.
  emu.watch(which, 0, 0xFFFFFFFF, function(pc, a, size, value)
    a = a & 0xFFFFFF
    if addr < a or addr >= a + size then return end
    seen = seen + 1
    if seen > 6 then return end
    local callers = {}
    for _, c in ipairs(emu.backtrace(which)) do callers[#callers + 1] = string.format("sub_%05x", c.target - 0x10100) end
    print(string.format("  [frame %d] %s: write %d byte(s) to %06x = %x at pc=text+%05x callers: %s",
      emu.frames(which), which, size, a, value, pc - 0x10100, table.concat(callers, " ")))
  end)
  for f = snap.frame, to do
    if not emu.run(which) then break end
    emu.joy(which, joy(emu.frames(which) + 1))
    if f < to then emu.vsync(which) end
  end
  emu.unwatch(which)
  if seen == 0 then print(string.format("  %s: no write to %06x between frames %d and %d", which, addr, snap.frame, to)) end
end

local ring, checked = {}, 0
while emu.frames("ref") < frames do
  if not (emu.run("ref") and emu.run("hyb")) then
    print("emulator halted at frame " .. emu.frames("ref"))
    quit(1)
    return
  end
  local f = emu.frames("ref")
  checked = checked + 1
  -- Memory below the stack pointer is dead: lifted routines skip the original's register pushes.
  local sp = emu.regs("ref").a7
  local diffs = compare.ram("ref", "hyb", sp > 0x800 and sp - 0x800 or 0, sp)
  if #diffs > 0 or emu.pc("ref") ~= emu.pc("hyb") then
    print(string.format("MISMATCH at frame %d (pc ref=%06x hyb=%06x)", f, emu.pc("ref"), emu.pc("hyb")))
    for i = 1, math.min(#diffs, 12) do
      local d = diffs[i]
      print(string.format("  %06x..%06x (%s): ref=%02x hyb=%02x", d.lo, d.hi, d.field, d.a, d.b))
    end
    if #diffs > 0 and #ring > 0 then
      local snap = ring[#ring]
      print(string.format("replaying from snapshot at frame %d, watching %06x", snap.frame, diffs[1].lo))
      replay("ref", snap, f, diffs[1].lo)
      replay("hyb", snap, f, diffs[1].lo)
    end
    quit(1)
    return
  end
  if f % SNAP_EVERY == 0 then snapshot(ring, f) end
  emu.joy("ref", joy(f + 1)); emu.joy("hyb", joy(f + 1))
  emu.vsync("ref"); emu.vsync("hyb")
end

local parts = {}
for name, s in pairs(hybrid.stats()) do parts[#parts + 1] = string.format("%s=%d (fallback %d)", name, s.calls, s.fallbacks) end
table.sort(parts)
print("lifted calls: " .. table.concat(parts, " "))
print(string.format("lockstep ok: %d vsync points identical over %d frames", checked, frames))
quit(0)
