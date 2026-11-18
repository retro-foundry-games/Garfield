-- Reference-only breadth-first search over (room, carried items): finds joystick inputs that carry an item from where
-- it lies to where it is needed. Prints the winning inputs as an 'events = ...' line for replay.lua.
--
-- usage: garfield-dev emulator/itemsearch.lua <goal: room> [budget frames] [log]
local GOAL, BUDGET, LOG = tonumber(args[1]), tonumber(args[2]) or 600000, args[3]
local A6, TEXT = 0x440F4, 0x10100
local function room() return emu.rd16("ref", A6 + 0xA0) end
local function items() return emu.rd16("ref", A6 + 0xA8) end
local R, L, U, D, F = 0x08, 0x04, 0x01, 0x02, 0x80
local plans = {}
for d = 4, 220, 10 do
  plans[#plans + 1] = { { R, d }, { D, 40 } }
  plans[#plans + 1] = { { L, d }, { D, 40 } }
end
for d = 22, 38, 3 do
  plans[#plans + 1] = { { R, d }, { F | U, 40 } }
  plans[#plans + 1] = { { L, d }, { F | U, 40 } }
end
plans[#plans + 1] = { { R, 200 } }
plans[#plans + 1] = { { L, 200 } }
plans[#plans + 1] = { { R | U, 60 }, { R, 100 } }
plans[#plans + 1] = { { L | U, 60 }, { L, 100 } }
plans[#plans + 1] = { { U, 120 } }

local out = io.open(LOG or "/dev/null", "a")
local function note(text) print(text); if out then out:write(text, "\n"); out:flush() end end
local events, used = { { 0, 0 } }, 0
local function hold(joy, frames)
  events[#events + 1] = { emu.frames("ref"), joy }
  local target = emu.frames("ref") + frames
  while emu.frames("ref") < target do
    assert(emu.run("ref")); emu.joy("ref", joy); emu.vsync("ref")
  end
  used = used + frames
end
hold(0, 60)
local function copy(l) local c = {} for i, v in ipairs(l) do c[i] = v end return c end
local seen, queue = {}, {}
local function key() return string.format("%d/%x", room(), items()) end
local function push()
  local k = key()
  if seen[k] then return false end
  seen[k] = true
  queue[#queue + 1] = { state = emu.save("ref"), events = copy(events), k = k }
  note(string.format("state room %d carrying %x (%d frames)", room(), items(), used))
  return true
end
-- leave the intro to the right first
hold(R, 100)
push()
local head = 1
while head <= #queue and used < BUDGET do
  local node = queue[head]; head = head + 1
  for _, plan in ipairs(plans) do
    emu.restore("ref", node.state)
    events = copy(node.events)
    for _, seg in ipairs(plan) do hold(seg[1], seg[2]) end
    local before = room()
    if push() and room() == GOAL and items() == 0xFFFF then end
    if room() == GOAL and items() == 13 then
      note("GOAL")
      local parts = {}
      for _, e in ipairs(events) do parts[#parts + 1] = string.format("{%d,%d}", e[1], e[2]) end
      local line = "events = {" .. table.concat(parts, ",") .. "}"
      note(line)
      quit(0)
    end
  end
end
print("no goal in budget; states:", #queue)
quit(1)
