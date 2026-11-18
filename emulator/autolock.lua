-- Autoplayer in lockstep: the port and the reference both run the same joystick inputs while the player walks out of
-- every room in every direction (branching from savestates of both sides) and all of memory is compared at every
-- sync point. The rooms reached come from the reference.
--
-- usage: garfield-dev emulator/autolock.lua [budget_frames] [log_file]
local BUDGET = tonumber(args[1]) or 20000
local LOG = args[2] ~= "" and args[2] or nil
local RANDOM_PLANS = tonumber(args[3]) or 0
local TARGET_ROOM, TARGET_PLAN = tonumber(args[4]), tonumber(args[5])   -- once found, only this plan of this room runs

local SYNC_PC = { [0x1089A] = 0, [0x172F4] = 1, [0x103B4] = 2 }
local MASKED = { { 0x44DA6, 0x450A6 } }   -- the program's stack: the port has none
local A6 = 0x440F4
local function room() return emu.rd16("ref", A6 + 0xA0) end
local function player() return emu.rd16("ref", 0x10100 + 0x4F4A), emu.rd16("ref", 0x10100 + 0x4F4C) end

local RIGHT, LEFT, UP, DOWN, FIRE = 0x08, 0x04, 0x01, 0x02, 0x80
local plans = {
  { "right", { { RIGHT, 200 } } },
  { "left", { { LEFT, 200 } } },
  { "right, jumping", { { RIGHT | UP, 60 }, { RIGHT, 40 }, { RIGHT | UP, 60 }, { RIGHT, 100 } } },
  { "left, jumping", { { LEFT | UP, 60 }, { LEFT, 40 }, { LEFT | UP, 60 }, { LEFT, 100 } } },
  { "up", { { UP, 120 } } },
  { "down", { { DOWN, 120 } } },
  { "fire, right", { { FIRE, 10 }, { RIGHT, 100 } } },
  { "fire, left", { { FIRE, 10 }, { LEFT, 100 } } },
  { "fire, up", { { FIRE | UP, 10 }, { UP, 60 } } },
  { "walk and fire", { { RIGHT, 40 }, { FIRE, 10 }, { RIGHT, 80 }, { FIRE, 10 } } },
}

-- Seeded random plans: bursts of random joystick positions, with fire and up (doors) among them.
local seed = tonumber(args[6]) or 12345
local function random(n) seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF; return (seed >> 8) % n end
local moves = { 0, RIGHT, LEFT, UP, DOWN, RIGHT | UP, LEFT | UP, FIRE, FIRE | RIGHT, FIRE | LEFT, FIRE | UP, FIRE | DOWN, RIGHT, LEFT }
for n = 1, RANDOM_PLANS do
  local segments, total = {}, 0
  while total < 400 do
    local frames = 8 + random(40)
    segments[#segments + 1] = { moves[1 + random(#moves)], frames }
    total = total + frames
  end
  plans[#plans + 1] = { "random " .. n, segments }
end

-- Doors: walk a while one way, then press up where a door would be, or run into an edge and climb.
if args[7] == "doors" then
  for d = 22, 38, 2 do   -- from either edge, 22 to 38 frames of walking puts the player at a door
    plans[#plans + 1] = { "door right " .. d, { { RIGHT, d }, { FIRE | UP, 40 } } }
    plans[#plans + 1] = { "door left " .. d, { { LEFT, d }, { FIRE | UP, 40 } } }
  end
  for d = 60, 240, 30 do
    plans[#plans + 1] = { "door right " .. d, { { RIGHT, d }, { FIRE | UP, 40 } } }
    plans[#plans + 1] = { "door left " .. d, { { LEFT, d }, { FIRE | UP, 40 } } }
  end
  for d = 20, 100, 40 do
    plans[#plans + 1] = { "up edge right " .. d, { { RIGHT, d }, { UP, 80 }, { RIGHT | UP, 80 } } }
    plans[#plans + 1] = { "up edge left " .. d, { { LEFT, d }, { UP, 80 }, { LEFT | UP, 80 } } }
  end
end

-- Climbing: stand still under a place to climb, or walk to it and go up.
if args[7] == "doors" or args[7] == "climb" then
  for d = 4, 200, 12 do
    plans[#plans + 1] = { "climb right " .. d, { { RIGHT, d }, { UP, 160 } } }
    plans[#plans + 1] = { "climb left " .. d, { { LEFT, d }, { UP, 160 } } }
    plans[#plans + 1] = { "climb fire right " .. d, { { RIGHT, d }, { FIRE | UP, 160 } } }
  end
end

local GIVE = tonumber(args[9])   -- with START: the item the player carries then
if args[7] == "drop" then plans = {} end
if args[7] == "climb" or args[7] == "drop" then   -- down drops what is carried; then wait to be picked up, or go to meet whatever comes
  plans[#plans + 1] = { "drop and wait", { { DOWN, 30 }, { 0, 500 } } }
  for d = 10, 90, 20 do
    plans[#plans + 1] = { "drop, right " .. d, { { DOWN, 30 }, { RIGHT, d }, { 0, 500 } } }
    plans[#plans + 1] = { "drop, left " .. d, { { DOWN, 30 }, { LEFT, d }, { 0, 500 } } }
  end
end

local START = tonumber(args[8])   -- begin in this room (a door transition on both sides), then play from there
local log = LOG and assert(io.open(LOG, "w"))
local function say(...)
  local s = string.format(...)
  print(s)
  if log then log:write(s, "\n"); log:flush() end
end

local rooms, frontier, discovered_count = {}, {}, 0
local used, arrivals, started = 0, 0, false
local plan, plan_base, segment, seg_end, current_joy = nil, nil, 0, 0, 0
local frontier_index = 1
local current_fr, resume = nil, false
-- The joystick changes that lead to the state being run: {frame, value}. A failure prints them so replay.lua can reach it.
local events = { { 0, 0 } }
local function copy(list) local c = {} for i, v in ipairs(list) do c[i] = v end return c end
local function set_joy(value, frame)
  current_joy = value
  events[#events + 1] = { frame, value }
end
local function events_text()
  local parts = {}
  for _, e in ipairs(events) do parts[#parts + 1] = string.format("{%d,%d}", e[1], e[2]) end
  return "{" .. table.concat(parts, ",") .. "}"
end
local skipped = {}

local function add_frontier(r, via)
  if rooms[r] then return end
  local x, y = player()
  rooms[r] = true
  discovered_count = discovered_count + 1
  frontier[#frontier + 1] = { room = r, ref = emu.save("ref"), port = port.save(), next = 1, events = copy(events) }
  say("room %d reached %s (player at %d,%d), %d frames used", r, via, x, y, used)
end

local finished = false
local teleported = false
local failures = 0
-- A divergence: reported (with the events that reproduce it), the plan dropped, both sides restored to where it began.
local function fail()
  failures = failures + 1
  say("events = %s", events_text())
  if not current_fr or not plan or failures > 40 then finished = true; port.stop(); return end
  plan = nil
  emu.restore("ref", current_fr.ref)
  port.restore(current_fr.port)
  events = copy(current_fr.events)
  resume = true
  port.resume()
end

local function finish()
  finished = true
  local sorted = {}
  for r in pairs(rooms) do sorted[#sorted + 1] = r end
  table.sort(sorted)
  say("%d rooms reached in %d frames, identical at %d sync points: %s", #sorted, used, arrivals, table.concat(sorted, " "))
  for _, why in ipairs(skipped) do say("dropped: %s", why) end
  say("%d divergences reported", failures)
  port.stop()
end

-- Called at the top of the main loop: decides what the joystick does next. Returns false when the run is over.
local function control()
  local f = emu.frames("ref")
  if not started then
    if f < 60 then return true end
    if START and room() ~= START then
      if not teleported and f >= 120 and room() ~= 6 then
        teleported = true
        local here = room()
        local function both(addr, v) port.wr16(addr, v); emu.wr16("ref", addr, v) end
        both(0x10100 + 0x3404 + here * 20 + 8, START)
        both(0x10100 + 0x4E6A + 8 * 0x1C, 0xB0)
        both(0x10100 + 0x4E6A + 8 * 0x1C + 6, emu.rd16("ref", 0x10100 + 0x4E6A + 8 * 0x1C + 6) | 8)
      end
      current_joy = (not teleported and f >= 30) and RIGHT or 0
      return true
    end
    started = true
    if GIVE and START then
      port.wr16(0x440F4 + 0xA8, GIVE); emu.wr16("ref", 0x440F4 + 0xA8, GIVE)
    end
    add_frontier(room(), "at start")
    plan = nil
  end
  if plan then
    local r = room()
    if r ~= plan_base then
      add_frontier(r, string.format("from room %d going %s", plan_base, plan[1]))
      plan = nil
    elseif f >= seg_end then
      segment = segment + 1
      if segment > #plan[2] then
        plan = nil
      else
        set_joy(plan[2][segment][1], f)
        seg_end = f + plan[2][segment][2]
        used = used + plan[2][segment][2]
      end
    end
  end
  if not plan then
    if #frontier == 0 or used >= BUDGET then return false end
    if frontier_index > #frontier then frontier_index = 1 end
    local fr = frontier[frontier_index]
    if TARGET_ROOM and rooms[TARGET_ROOM] then
      for i, candidate in ipairs(frontier) do if candidate.room == TARGET_ROOM then fr = candidate; frontier_index = i end end
      if fr.room ~= TARGET_ROOM or fr.done then return false end
      fr.next = TARGET_PLAN
      fr.done = true
    end
    if fr.next > #plans then
      emu.free(fr.ref)
      table.remove(frontier, frontier_index)
      return true
    end
    current_fr = fr
    plan = plans[fr.next]
    fr.next = fr.next + 1
    plan_base = fr.room
    emu.restore("ref", fr.ref)
    port.restore(fr.port)
    segment = 1
    events = copy(fr.events)
    set_joy(plan[2][1][1], emu.frames("ref"))
    seg_end = emu.frames("ref") + plan[2][1][2]
    used = used + plan[2][1][2]
    frontier_index = frontier_index % #frontier + 1
  end
  return true
end

-- The port was sent to something that is not code (the original runs into garbage there, e.g. the door to room 80): that
-- plan is dropped and the run carries on from the state it began from.
port.on_unported(function(address)
  if not current_fr or not plan then return false end
  say("plan '%s' from room %d ends where the original runs into data ($%x) in room %d, frame %d", plan[1], plan_base, address, room(), emu.frames("ref"))
  skipped[#skipped + 1] = string.format("%s from room %d ($%x)", plan[1], plan_base, address)
  plan = nil
  emu.restore("ref", current_fr.ref)
  port.restore(current_fr.port)
  events = copy(current_fr.events)
  resume = true
  return true
end)

port.on_sync(function(id)
  arrivals = arrivals + 1
  if resume then resume = false
  elseif not emu.run("ref") then say("reference halted"); port.stop(); return end
  local pc = emu.pc("ref")
  if SYNC_PC[pc] ~= id then
    say("CONTROL FLOW: the port is at sync %d, the reference at pc=%06x (sync %s), frame %d, room %d", id, pc, tostring(SYNC_PC[pc]),
        emu.frames("ref"), room())
    fail(); return
  end
  local diffs = compare.ram("port", "ref", MASKED)
  if #diffs > 0 then
    say("MISMATCH at arrival %d (sync %d, frame %d, room %d): %d ranges", arrivals, id, emu.frames("ref"), room(), #diffs)
    for i = 1, math.min(#diffs, 12) do
      local d = diffs[i]
      say("  %06x..%06x (%s): port=%02x ref=%02x", d.lo, d.hi, d.field, d.a, d.b)
    end
    fail(); return
  end
  local wrong = compare.screen("port", "ref")
  if wrong > 0 then
    say("FRAMEBUFFER MISMATCH at arrival %d (sync %d, frame %d, room %d): %d bytes", arrivals, id, emu.frames("ref"), room(), wrong)
    fail(); return
  end
  local pixels = compare.render("ref")
  if pixels > 0 then
    say("RENDER MISMATCH at arrival %d (sync %d, frame %d, room %d): %d pixels", arrivals, id, emu.frames("ref"), room(), pixels)
    fail(); return
  end
  local colours, first = compare.palette("ref")
  if colours > 0 then
    say("PALETTE MISMATCH at arrival %d (sync %d, frame %d, room %d): %d colours, first %d", arrivals, id, emu.frames("ref"), room(), colours, first)
    fail(); return
  end
  if id == 2 and not control() then finish(); return end
  port.joy(current_joy); emu.joy("ref", current_joy)
  emu.vsync("ref")
end)
port.run("extracted")
if not finished then
  say("the port stopped: plan '%s' (segment %d), frame %d, room %d", plan and plan[1] or "-", segment, emu.frames("ref"), room())
end
quit(0)
