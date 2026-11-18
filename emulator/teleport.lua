-- Port against reference after sending the player through a door to a given room, in lockstep: the current room's
-- door record and the player are set the same way on both sides, so the rooms the autoplayer cannot walk to still
-- get their routines compared.
--
-- usage: garfield-dev emulator/teleport.lua <room> [frames to run after the door, default 300] [force]
--   force 1 / 2 / 3: 30 frames after the door the player is put next to object 28, facing right / left / right nearer, with the carry flag set
local target, extra, force = tonumber(args[1]), tonumber(args[2]) or 300, tonumber(args[3])
assert(target, "usage: teleport.lua <room> [frames]")

local SYNC_PC = { [0x1089A] = 0, [0x172F4] = 1, [0x103B4] = 2 }
local MASKED = { { 0x44DA6, 0x450A6 } }   -- the program's stack: the port has none
local A6, TEXT = 0x440F4, 0x10100
local PLAYER = TEXT + 0x4E6A + 8 * 0x1C
local arrivals, door_frame, f_inject = 0, nil, nil

local function both_wr16(addr, value) port.wr16(addr, value); emu.wr16("ref", addr, value) end

port.on_sync(function(id)
  arrivals = arrivals + 1
  if not emu.run("ref") then print("reference halted"); port.stop(); return end
  if SYNC_PC[emu.pc("ref")] ~= id then
    print(string.format("CONTROL FLOW: the port is at sync %d, the reference at pc=%06x, frame %d", id, emu.pc("ref"), emu.frames("ref")))
    port.stop(); return
  end
  local diffs = compare.ram("port", "ref", MASKED)
  if #diffs > 0 then
    print(string.format("room %d: MISMATCH at arrival %d (sync %d, frame %d): %d ranges", target, arrivals, id, emu.frames("ref"), #diffs))
    for i = 1, math.min(#diffs, 12) do
      local d = diffs[i]
      print(string.format("  %06x..%06x (%s): port=%02x ref=%02x", d.lo, d.hi, d.field, d.a, d.b))
    end
    port.stop(); return
  end
  if os.getenv("INJECT_SCREEN") and f_inject == nil and emu.frames("ref") >= 100 then f_inject = true; port.wr8(0xF8000 + 4000, port.rd8(0xF8000 + 4000) ~ 0x10) end
  local wrong = compare.screen("port", "ref")
  if wrong > 0 then print(string.format("room %d: FRAMEBUFFER MISMATCH at frame %d: %d bytes", target, emu.frames("ref"), wrong)); port.stop(); return end
  local pixels = compare.render("ref")
  if pixels > 0 then print(string.format("room %d: RENDER MISMATCH at frame %d: %d pixels", target, emu.frames("ref"), pixels)); port.stop(); return end
  if compare.palette("ref") > 0 then print(string.format("room %d: PALETTE MISMATCH at frame %d", target, emu.frames("ref"))); port.stop(); return end
  local f = emu.frames("ref")
  if id == 2 and not door_frame and f >= 120 and emu.rd16("ref", A6 + 0xA0) ~= 6 then
    local here = emu.rd16("ref", A6 + 0xA0)
    both_wr16(TEXT + 0x3404 + here * 20 + 8, target)            -- the room's door leads to the target
    both_wr16(PLAYER, 0xB0)                                       -- the player stands at it
    both_wr16(PLAYER + 6, emu.rd16("ref", PLAYER + 6) | 0x08)     -- and goes through
    door_frame = f
  end
  if force and door_frame and f == door_frame + 30 and id == 2 then
    local ENT = TEXT + 0x4E6A + 11 * 0x1C
    local c84 = emu.rd8("ref", A6 + 0xC84)
    port.wr8(A6 + 0xC84, c84 | 0x20); emu.wr8("ref", A6 + 0xC84, c84 | 0x20)
    both_wr16(PLAYER + 2, 0x78)
    if force == 3 then both_wr16(PLAYER, 0x70) end
    both_wr16(PLAYER + 6, force == 2 and 1 or 0)
    both_wr16(ENT, (force == 2 or force == 3) and 0x90 or 0xD0); both_wr16(ENT + 2, 0x90); both_wr16(ENT + 4, 0x1C); both_wr16(ENT + 6, 2)
  end
  if door_frame and f >= door_frame + extra then
    print(string.format("room %d: identical at %d sync points over %d frames (now in room %d; thrown-object flag %d, entity 11 flags %x)", target, arrivals, f, emu.rd16("ref", A6 + 0xA0), emu.rd8("ref", A6 + 0xC87) & 4, emu.rd16("ref", TEXT + 0x4E6A + 11 * 0x1C + 6)))
    port.stop(); return
  end
  local joy = (f >= 60 and not door_frame) and 0x08 or 0
  port.joy(joy); emu.joy("ref", joy)
  emu.vsync("ref")
end)
port.run("extracted")
quit(0)
