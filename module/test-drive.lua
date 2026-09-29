-- test-drive.lua: drives module-maemo-volume from WirePlumber
--
-- Proves the whole delivery path without touching the real policy:
--
--   WP Lua  md:set()  ->  daemon metadata impl  ->  module listener
--         ->  data-loop timer  ->  pw_impl_device_set_param(Route)
--
-- It resolves the sink's device and active route the same way the real
-- policy does (device.id + card.profile.device, then the Route param on
-- the device whose .device matches that index), so the payload the module
-- replays is identical in shape to what apply_route() would have written.

local cutils = require ("common-utils")

local log = Log.open_topic ("s-policy")

local NS          = "x-maemo-volume"
local PAYLOAD_KEY = "x-maemo.route-volume"

-- Change between runs to exercise the direction-aware bias.
local LEVEL = tonumber (os.getenv ("MAEMO_TEST_LEVEL") or "0.35")

log:info ("TEST: driver loaded")

local source = nil
local md     = nil
local sends  = 0
local ticking = false
local levels = { 0.18, 0.42, 0.25, 0.55 }

local function object_manager (kind)
  if source == nil then return nil end
  local ok, om = pcall (function () return source:call ("get-object-manager", kind) end)
  if ok then return om end
  return nil
end

local function ensure_metadata ()
  if md ~= nil then return md end
  local mom = object_manager ("metadata")
  if mom == nil then return nil end
  local ok, found = pcall (function ()
    return mom:lookup { Constraint { "metadata.name", "=", NS } }
  end)
  if not ok or found == nil then return nil end
  md = found
  log:info ("TEST: found our namespace '" .. NS .. "'")
  return md
end

local function try_send ()
  if md == nil then return end

  -- first Audio/Sink that has a resolved device
  local sink = nil
  local nodes = object_manager ("node")
  if nodes == nil then return end
  for n in nodes:iterate () do
    if (n.properties["media.class"] or "") == "Audio/Sink" then
      local d = tonumber (n.properties["device.id"])
      local i = tonumber (n.properties["card.profile.device"])
      if d ~= nil and i ~= nil then
        sink = n
        break
      end
    end
  end
  if sink == nil then return end

  local dev_id  = tonumber (sink.properties["device.id"])
  local dev_idx = tonumber (sink.properties["card.profile.device"])

  local dom = object_manager ("device")
  if dom == nil then return end
  local device = nil
  local ok, found = pcall (function ()
    return dom:lookup { Constraint { "bound-id", "=", dev_id, type = "gobject" } }
  end)
  if ok then device = found end
  if device == nil then
    log:info ("TEST: device " .. tostring (dev_id) .. " not found")
    return
  end

  local route_index = nil
  for p in device:iterate_params ("Route") do
    local r = cutils.parseParam (p, "Route")
    if r ~= nil and r.device == dev_idx then route_index = r.index end
  end
  if route_index == nil then
    log:info ("TEST: no route for device index " .. tostring (dev_idx))
    return
  end

  local lvl = levels[(sends % #levels) + 1]
  local payload = string.format ("index=%d devidx=%d device=%d vol=%s,%s save=1",
      route_index, dev_idx, dev_id,
      string.format ("%.6f", lvl), string.format ("%.6f", lvl))

  local ok2, err = pcall (function ()
    md:set (dev_id, PAYLOAD_KEY, "String", payload)
  end)
  if not ok2 then
    log:info ("TEST: md:set failed: " .. tostring (err))
    return
  end

  log:info ("TEST sent: " .. payload)
  sends = sends + 1
end

-- Re-send on a timer so the payload can be delivered while playback is
-- stable, rather than only during a restart (where the sink suspends and
-- the ring is genuinely empty).
local function tick ()
  try_send ()
  Core.timeout_add (3000, tick)
end

SimpleEventHook {
  name = "test-drive@send",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "device-params-changed" },
    },
  },
  execute = function (event)
    if event:get_source () ~= nil then
      source = event:get_source ()
    end
    ensure_metadata ()
    if md ~= nil and not ticking then
      ticking = true
      Core.timeout_add (3000, tick)
    end
  end,
}:register()
