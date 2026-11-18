-- Unit check of one routine of the port against the original: the reference runs to the routine's entry, its
-- memory is copied into the port, the port routine runs, and when the reference returns from the routine the
-- two memories are compared.
--
-- usage: garfield-dev emulator/unit.lua <routine name> <entry pc> <return pc[,pc...]> [skip entries] [max frames] [entity]
--   a last argument 'walk' feeds the scripted joystick (right, right+fire, left, up, down, right+up in 150 frame phases)
--   pcs are absolute (text offset + 0x10100); 'entity' passes the entity a3 points at (port.call_entity)
local name, entry = args[1], tonumber(args[2])
local rets = {}
for v in string.gmatch(args[3] or "", "[^,]+") do rets[tonumber(v)] = true end
local per_entity = args[6] == "entity"
local walk = args[6] == "walk" or args[7] == "walk"
local phases = { 0x08, 0x88, 0x04, 0x01, 0x02, 0x09 }
local function joy(frame)
  if not walk or frame < 30 then return 0 end
  return phases[(frame // 150) % #phases + 1]
end
local skip, max_frames = tonumber(args[4]) or 0, tonumber(args[5]) or 400
assert(name and entry and next(rets), "usage: unit.lua name entry_pc return_pc[,pc] [skip] [max_frames] [entity]")

local MASKED = { { 0x44DA6, 0x450A6 } }   -- the program's stack: the port has none
local state, seen, entity = "waiting", 0, nil
emu.trace("ref", function(pc)
  if state == "waiting" and pc == entry then
    seen = seen + 1
    if seen > skip then
      port.sync("ref")
      if per_entity then
        entity = (emu.regs("ref").a3 - (0x10100 + 0x4E6A)) // 0x1C
        port.call_entity(name, entity)
      else
        port.call(name)
      end
      state = "running"
    end
  elseif state == "running" and rets[pc] then
    state = "done"
    local diffs = compare.ram("port", "ref", MASKED)
    if #diffs == 0 then
      print(string.format("%s%s: identical to the original (entry %d at frame %d)", name, entity and (" entity " .. entity) or "", seen, emu.frames("ref")))
    else
      print(string.format("%s%s: %d ranges differ (frame %d)", name, entity and (" entity " .. entity) or "", #diffs, emu.frames("ref")))
      for i = 1, math.min(#diffs, 14) do
        local d = diffs[i]
        print(string.format("  %06x..%06x (%s): port=%02x original=%02x", d.lo, d.hi, d.field, d.a, d.b))
      end
    end
  end
end)
while state ~= "done" and emu.frames("ref") < max_frames do
  assert(emu.run("ref"), "emulator halted")
  emu.joy("ref", joy(emu.frames("ref") + 1))
  emu.vsync("ref")
end
emu.trace("ref", nil)
if state ~= "done" then print(name .. ": the routine was not reached") end
quit(state == "done" and 0 or 1)
