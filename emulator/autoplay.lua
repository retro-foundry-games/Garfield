-- Autoplayer: explores the game on the reference emulator by walking out of every room it finds in every
-- direction (branching from savestates), recording the rooms reached and, optionally, the code executed.
--
-- usage: garfield-dev emulator/autoplay.lua [budget_frames] [coverage_file]
local BUDGET = tonumber(args[1]) or 20000
local COVERAGE = args[2]

local A6 = 0x440F4
local TEXT = 0x10100
local function room() return emu.rd16("ref", A6 + 0xA0) end
local function player() return emu.rd16("ref", TEXT + 0x4F4A), emu.rd16("ref", TEXT + 0x4F4C) end

local covered = {}
if COVERAGE then
  emu.trace("ref", function(pc) covered[pc - TEXT] = true end)
end

local used = 0
-- Runs `frames` frames holding `joy`; returns the room seen at the end.
local function hold(joy, frames)
  local target = emu.frames("ref") + frames
  while emu.frames("ref") < target do
    assert(emu.run("ref"), "emulator halted")
    emu.joy("ref", joy)
    emu.vsync("ref")
  end
  used = used + frames
  return room()
end

local RIGHT, LEFT, UP, DOWN, FIRE = 0x08, 0x04, 0x01, 0x02, 0x80
-- What to try from a fresh room: walk each way, jump over things, climb.
local plans = {
  { "right", { { RIGHT, 200 } } },
  { "left", { { LEFT, 200 } } },
  { "right, jumping", { { RIGHT | UP, 60 }, { RIGHT, 40 }, { RIGHT | UP, 60 }, { RIGHT, 100 } } },
  { "left, jumping", { { LEFT | UP, 60 }, { LEFT, 40 }, { LEFT | UP, 60 }, { LEFT, 100 } } },
  { "up", { { UP, 120 } } },
  { "down", { { DOWN, 120 } } },
}

-- Enter the first room: let the game settle, then walk right until the room is established.
hold(0, 60)
local rooms, frontier = {}, {}
local function discovered(r, via)
  if rooms[r] then return end
  local x, y = player()
  rooms[r] = { via = via, x = x, y = y }
  frontier[#frontier + 1] = { room = r, state = emu.save("ref"), next = 1 }
  print(string.format("room %d reached %s (player at %d,%d), %d frames used", r, via, x, y, used))
end
discovered(room(), "at start")

local i = 1
while #frontier > 0 and used < BUDGET do
  local f = frontier[i]
  if f.next > #plans then
    emu.free(f.state)
    table.remove(frontier, i)
    if i > #frontier then i = 1 end
  else
    local plan = plans[f.next]
    f.next = f.next + 1
    emu.restore("ref", f.state)
    local before = f.room
    local now = before
    for _, seg in ipairs(plan[2]) do
      now = hold(seg[1], seg[2])
      if now ~= before then break end
    end
    if now ~= before then
      discovered(now, string.format("from room %d going %s", before, plan[1]))
    end
    i = i % #frontier + 1
  end
end

local sorted = {}
for r in pairs(rooms) do sorted[#sorted + 1] = r end
table.sort(sorted)
print(string.format("%d rooms reached in %d frames: %s", #sorted, used, table.concat(sorted, " ")))
if COVERAGE then
  emu.trace("ref", nil)
  local f = assert(io.open(COVERAGE, "w"))
  local offs = {}
  for o in pairs(covered) do offs[#offs + 1] = o end
  table.sort(offs)
  for _, o in ipairs(offs) do f:write(string.format("%x\n", o)) end
  f:close()
  print(#offs .. " distinct instruction addresses executed -> " .. COVERAGE)
end
quit(0)
