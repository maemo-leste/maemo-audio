-- Maemo routing
--
-- What this script is for, and what it is explicitly not for.
--
-- WirePlumber owns one thing in the volume design: where the audio goes.
-- Which device, which route index, which card profile device, and which
-- sink is the default. That is genuinely its domain and nothing else in
-- the stack can derive it -- several card routes can be available at
-- once, and only the policy knows which one was selected.
--
-- It owns nothing about loudness. The level lives in the daemon, in
-- module-maemo-mix and the mixer it drives, because the level, the route
-- it is written to and the node the applet reads it back from all have to
-- agree at one moment, and a policy script sees a proxy of each, updated
-- on its own schedule.
--
--   maemo-routing.lua          where the audio goes        (this file)
--   module-maemo-mix           how loud                    (in-process)
--
-- Nothing here computes a level. What is left is one announcement per
-- routing change, and the events that make it necessary to re-send it.

Script.async_activation = true

local cutils = require ("common-utils")
local lutils = require ("linking-utils")

local log = Log.open_topic ("s-policy")

-- The module's own metadata namespace. Not the daemon's default one:
-- the applet owns that, and this is us talking to our own module.
local ROUTE_NS = "x-maemo-mix"
local ROUTE_KEY = "x-maemo.route"

-- The default sink announcement lives in the daemon's default namespace,
-- alongside the applet's target. Dotted key; the hyphenated spelling
-- does not exist and silently never matched.
local DEFAULT_NS = "default"
local DEFAULT_SINK_KEY = "default.audio.sink"
-- The user's chosen device, as distinct from the resolved default.
-- WP keeps these separate and we must read the right one. See
-- default_sink_name().
local CONFIGURED_SINK_KEY = "default.configured.audio.sink"

-- Our own virtual sink. Streams are aimed at this rather than at a
-- device sink, so that every one of them passes through the daemon's
-- mixer and is mixed there. The mixer's output is then
-- relayed to whichever real sink is current, which is what this script
-- has to keep true.
local MIXER_NAME = "maemo_mixer"

-- One mixer per audio card, each named `<MIXER_NAME>.<card>`, the card token
-- derived by the module from the card's own nick. Nothing here matches a
-- literal node name any more: which card is current changes, the names are
-- descriptive, and a name that has to be edited whenever hardware appears is
-- a second truth. "One of ours" is a prefix test, and "the one for this
-- device" is answered by the device.id the mixer carries, the same property
-- the device sinks carry.
local MIXER_PREFIX = MIXER_NAME .. "."

local function is_mixer (name)
  if name == nil then return false end
  if name == MIXER_NAME then return true end
  return string.sub (name, 1, #MIXER_PREFIX) == MIXER_PREFIX
end

----------------------------------------------------------------------------
-- state
----------------------------------------------------------------------------

-- sink bound id -> { device = <device bound id>, dev_idx = <int> }
--
-- The sink node carries both, so this is a lookup table rather than
-- anything that needs maintaining beyond a rebuild.
local sink_map = {}

-- The script's own source object, from which object managers are
-- obtained. Not available until the first hook fires.
local source = nil

-- Our namespace, and the default namespace we read the default sink from.
local route_md = nil
local default_md = nil

-- The relay link from our mixer to the current real sink, and the name
-- of the sink it points at. Kept so a device change can take the old
-- link down instead of leaving it behind and feeding two sinks.
local relay_link = nil
local relay_target = nil

----------------------------------------------------------------------------
-- helpers
----------------------------------------------------------------------------

local function object_manager (kind)
  if source == nil then return nil end
  local ok, om = pcall (function ()
      return source:call ("get-object-manager", kind)
    end)
  if ok then return om end
  return nil
end

local function node_by_id (om, id)
  if om == nil or id == nil then return nil end
  local ok, node = pcall (function ()
      return om:lookup { Constraint { "bound-id", "=", id, type = "gobject" } }
    end)
  if ok then return node end
  return nil
end

-- The route on `device` that belongs to this sink's card profile device.
-- Matching on the device index rather than taking the first route is what
-- keeps this correct on a card whose profile has several outputs.
local function route_index_for (device, dev_idx)
  for p in device:iterate_params ("Route") do
    local r = cutils.parseParam (p, "Route")
    if r ~= nil and r.device == dev_idx then
      return r.index
    end
  end
  return nil
end

local function update_sink_map ()
  local nom = object_manager ("node")
  if nom == nil then return end
  sink_map = {}
  for node in nom:iterate () do
    if (node.properties ["media.class"] or "") == "Audio/Sink" then
      local dev = tonumber (node.properties ["device.id"])
      local idx = tonumber (node.properties ["card.profile.device"])
      if dev ~= nil and idx ~= nil then
        sink_map [node.bound_id] = { device = dev, dev_idx = idx }
      end
    end
  end
end

local function name_from_blob (raw)
  if raw == nil then return nil end
  if type (raw) == "table" and raw.name ~= nil then
    return tostring (raw.name)
  end
  return tostring (raw):match ('"name"%s*:%s*"([^"]+)"')
end

-- Which real device the user actually wants.
--
-- WP keeps two keys and the difference is the whole answer to "which
-- sink do we connect to?":
--
--   default.configured.audio.sink  the user's chosen device. Written by
--       `wpctl set-default` and pavucontrol, persisted across restarts,
--       and updated by WP's own priority logic when a Bluetooth set
--       connects or drops. This is the source of truth.
--
--   default.audio.sink  the resolved, active default. apply-default-node
--       writes it from whatever the resolution chain picked, and we
--       overwrite it with our mixer so streams route through us.
--
-- Reading the resolved key is what made us guess. We clobber it with
-- `maemo_mixer`, so looking up the real sink there can only ever fail,
-- and the code fell through to "newest sink wins". That ignored an
-- explicit `wpctl set-default`, trusted iterate() order as a clock, and
-- let any third device -- HDMI, a USB DAC -- hijack routing.
--
-- Reading the configured key instead makes device selection exactly
-- stock WP behaviour with no guessing: the user picks the built-in while
-- BT is up, configured says built-in, and that is where the relay goes.
-- Our claim on the resolved key does not disturb it -- setting the
-- default to the ALSA sink leaves configured pointing at ALSA and the
-- active default still pointing at us.
--
-- The name, not the bound id: a bound id does not survive a daemon
-- restart, a node name is the stable handle.
local function default_sink_name ()
  if default_md == nil then return nil end

  local function find_key (key)
    local raw = nil
    pcall (function () raw = default_md:find (0, key) end)
    return name_from_blob (raw)
  end

  -- Configured first: that is the explicit user choice. Then the
  -- resolved default. Both now name a real device, because we stopped
  -- overwriting the resolved key with our mixer.
  return find_key (CONFIGURED_SINK_KEY) or find_key (DEFAULT_SINK_KEY)
end

local function sink_by_name (nom, name)
  if name == nil then return nil end
  for node in nom:iterate () do
    if (node.properties ["media.class"] or "") == "Audio/Sink"
        and node.properties ["node.name"] == name then
      return node
    end
  end
  return nil
end

-- The real output sink: an Audio/Sink that is not our mixer and that
-- belongs to a device. Our mixer has no device of its own, so the
-- presence of device.id is what tells the two apart.
--
-- Preference follows the framework's default while that default is a
-- real sink. Once we have aimed the default at our mixer it no longer
-- points at a device, so fall back to the newest real sink with a
-- device: on a card change the new sink appears before the old one is
-- withdrawn, so taking the last one tracks a Bluetooth connect, and
-- when the headset goes away the iteration lands back on the built-in.
local function real_sink_node ()
  local nom = object_manager ("node")
  if nom == nil then return nil end

  local def_name = default_sink_name ()
  if def_name ~= nil and not is_mixer (def_name) then
    local n = sink_by_name (nom, def_name)
    if n ~= nil and n.properties ["device.id"] ~= nil then
      return n
    end
  end

  local newest = nil
  for node in nom:iterate () do
    if (node.properties ["media.class"] or "") == "Audio/Sink"
        and not is_mixer (node.properties ["node.name"])
        and node.properties ["device.id"] ~= nil then
      newest = node
    end
  end
  return newest
end

-- The sink whose route the module must write. Deliberately not the
-- default sink any more: with the mixer in the path the default is
-- ours, and the codec lives on the real sink behind it.
local function announce_sink_id ()
  local n = real_sink_node ()
  if n == nil then return nil end
  return n.bound_id
end

----------------------------------------------------------------------------
-- the relay
----------------------------------------------------------------------------


-- Point a playback stream at our mixer.
--
-- stream.rules cannot do this. Its update-props feeds the restorable
-- property set -- which is exactly why state.restore-props works
-- there -- and not the routing properties find-defined-target reads.
-- The supported channel is the default metadata's per-subject
-- target.node, which find-defined-target consults before falling back
-- to the default sink, and whose value it treats as node.id namespaced.
--
-- Doing it per stream rather than by forcing the default also keeps the
-- relay honest: streams always enter the mixer, and this script decides
-- where the mixer's output goes, so a device change moves one link
-- rather than re-targeting every stream.
local function redirect_stream (md, node_id, want)
  local cur = nil
  pcall (function () cur = md:find (node_id, "target.node") end)
  if cur ~= nil and tonumber (cur) == want then return false end
  pcall (function ()
      md:set (node_id, "target.node", "Spa:Id", tostring (want))
    end)
  return true
end

local function redirect_all_streams (want)
  local nom = object_manager ("node")
  if nom == nil or default_md == nil or want == nil then
    return
  end
  local n = 0
  for node in nom:iterate () do
    if (node.properties ["media.class"] or "") == "Stream/Output/Audio" then
      -- The subject of a metadata entry is the pipewire node id, because
      -- that is what find-defined-target looks the entry up by
      -- (si_props["node.id"]).  node.id in this API is WirePlumber's own
      -- object id, a different number space -- writing the redirect
      -- under it produces an entry nobody asks for, and the stream
      -- silently keeps its old target.
      if redirect_stream (default_md, node["bound-id"], want) then
        n = n + 1
      end
    end
  end
  if n > 0 then
    log:info (string.format (
        "routing: redirected %d stream(s) to node %d", n, want))
  end
end

-- Our mixer's node id.  Streams are aimed at it by number, so this has
-- to be looked up rather than assumed; it is not stable across restarts.
local function mixer_node_id ()
  local nom = object_manager ("node")
  if nom == nil then return nil end
  for node in nom:iterate () do
    if (node.properties ["media.class"] or "") == "Audio/Sink"
        and is_mixer (node.properties ["node.name"]) then
      -- bound-id is the pipewire node id; node.id is WirePlumber's own
      -- and does not match what target.node is expected to contain.
      return node ["bound-id"]
    end
  end
  return nil
end

-- Forward declaration: the session-item register callback below runs
-- before refresh() is textually defined, and a plain local would have
-- captured nil at that point.
local refresh

-- Session items are created asynchronously: at the first node-added
-- the session-item object manager is empty, so a relay attempted then
-- finds nothing to link. Watch the manager itself and retry as items
-- land. refresh() is guarded against duplicate links, so re-entering
-- it is cheap.
local si_om_hooked = false
local function hook_session_items ()
  if si_om_hooked then return end
  local som = object_manager ("session-item")
  if som == nil then return end
  som:connect ("objects-changed", function (_, plugin)
    refresh ()
  end)
  si_om_hooked = true
end

-- The mixer for a given card, matched on device.id rather than on the name.
-- Falls back to any mixer, so a setup with one card still finds its one and a
-- mixer that has not appeared yet does not stall the relay.
local function si_by_device (dev_id)
  local som = object_manager ("session-item")
  if som == nil then return nil end
  local fallback = nil
  for si in som:iterate { type = "SiLinkable" } do
    local n = si:get_associated_proxy ("node")
    if n ~= nil and is_mixer (n.properties ["node.name"]) then
      if fallback == nil then fallback = si end
      if dev_id ~= nil
          and tostring (n.properties ["device.id"]) == tostring (dev_id) then
        return si
      end
    end
  end
  return fallback
end

local function si_by_node_name (name)
  local som = object_manager ("session-item")
  if som == nil or name == nil then return nil end
  -- Match through the associated node proxy rather than a property
  -- constraint: node.name lives on the proxy, and a SI does not
  -- necessarily copy it into its own property set, so a Constraint on
  -- node.name can succeed and still find nothing.
  local ok, found = pcall (function ()
      for si in som:iterate { type = "SiLinkable" } do
        local n = si:get_associated_proxy ("node")
        if n ~= nil and n.properties ["node.name"] == name then
          return si
        end
      end
      return nil
    end)
  if ok and found ~= nil then return found end
  return nil
end

-- Keep a link from our mixer's output to the current real sink's input.
--
-- This is what makes the mixer transparent to the device: the real sink
-- sits at unity because all level control happened upstream, so nothing
-- downstream needs coordinating with device restore or AVRCP absolute
-- volume.
--
-- The link is torn down and rebuilt rather than moved, because WP
-- 0.5.12 has no Policy object to declare the relationship
-- declaratively, and a hand-built si-standard-link is the closest.
-- Take the relay down properly.
--
-- SessionItem exposes get_associated_proxy, reset, configure, register,
-- remove and get_property -- there is no destroy, and wrapping the call
-- in pcall hides that rather than handling it.
--
-- reset() is the one that actually tears the PipeWire objects down:
-- si_standard_link_reset() deactivates ACTIVE and EXPORTED first, and
-- disable_active() is what calls clear_node_links(), which
-- deactivates the WpLink and destroys the PW link.  remove() then
-- drops the item from the core's object list so it cannot be found
-- again.  Both are needed: remove() alone leaves the PW link alive.
local function drop_relay ()
  if relay_link == nil then return end
  local link = relay_link
  relay_link = nil
  relay_target = nil
  pcall (function () link:reset () end)
  pcall (function () link:remove () end)
end

-- Streams that linked before the relay existed are wired straight into the
-- hardware, and the metadata redirect cannot move a stream that has already
-- been placed: the target entry is read when a stream asks for a target, and
-- a linked stream never asks again. Take the link away instead, clear the
-- peer flags so the linking policy considers the pair unlinked, and let it
-- put the link back where the metadata now says it belongs.
local function move_misplaced_links (mixer_si, sink_si)
  if mixer_si == nil or sink_si == nil then return 0 end
  local som = object_manager ("session-item")
  if som == nil then return 0 end

  local stream = {}
  for si in som:iterate { type = "SiLinkable" } do
    local n = si:get_associated_proxy ("node")
    if n ~= nil and (n.properties ["media.class"] or "")
        == "Stream/Output/Audio" then
      stream[si.id] = true
    end
  end

  local moved = 0
  for silink in som:iterate { type = "SiLink" } do
    local out_id = tonumber (silink.properties ["out.item.id"])
    local in_id = tonumber (silink.properties ["in.item.id"])
    if in_id == sink_si.id and out_id ~= mixer_si.id and stream[out_id] then
      local out_flags = lutils:get_flags (out_id)
      local in_flags = lutils:get_flags (in_id)
      if out_flags.peer_id == in_id then out_flags.peer_id = nil end
      if in_flags.peer_id == out_id then in_flags.peer_id = nil end
      silink:remove ()
      moved = moved + 1
    end
  end

  if moved > 0 then
    log:info (string.format ("routing: moved %d misplaced stream link(s)", moved))
    lutils:rescan ()
  end
  return moved
end

local function ensure_relay ()
  local real = real_sink_node ()
  local mixer_si = si_by_device (real and real.properties ["device.id"])


  if mixer_si == nil or real == nil then
    drop_relay ()
    return
  end

  local target_name = real.properties ["node.name"]
  if relay_target == target_name and relay_link ~= nil then
    return
  end

  -- The device changed underneath us. Take the old link down first so
  -- the mixer is not feeding two sinks during the swap.
  drop_relay ()

  local real_si = si_by_node_name (target_name)
  if real_si == nil then return end

  local link = SessionItem ("si-standard-link")
  if link == nil then return end

  local ok = link:configure {
    ["out.item"] = mixer_si,
    ["in.item"] = real_si,
    ["out.item.port.context"] = "output",
    ["in.item.port.context"] = "input",
    ["passthrough"] = false,
    ["exclusive"] = false,
    ["target.media.class"] = real.properties ["media.class"],
    ["main.item.id"] = mixer_si.id,
    ["target.item.id"] = real_si.id,
  }
  if not ok then
    log:warning (string.format ("routing: could not configure the relay into %s",
        tostring (target_name)))
    return
  end

  link:register ()
  link:activate (Feature.SessionItem.ACTIVE, function (l, err)
    if err == nil then
      -- Streams that asked for a target while this relay was still being
      -- built are linked into a path that went nowhere, and nothing would
      -- have moved them otherwise: this callback is the moment the path
      -- becomes real, so it is also the moment to re-aim them.
      redirect_all_streams (mixer_node_id ())
      move_misplaced_links (mixer_si, real_si)
    end
    if err ~= nil then
      log:warning (string.format ("routing: relay into %s failed: %s",
          tostring (target_name), tostring (err)))
      if relay_link == l then
        relay_link = nil
        relay_target = nil
      end
    end
  end)

  relay_link = link
  relay_target = target_name
  log:info (string.format ("routing: relay %s -> %s",
      MIXER_NAME, tostring (target_name)))
end

----------------------------------------------------------------------------
-- the announcement
----------------------------------------------------------------------------

-- What we tell the module. Deliberately nothing about level: the module
-- reads the user's target straight off the applet's metadata, computes
-- the residual itself, and decides what may be persisted.
--
--   sink    the default sink's bound id, so the module knows which
--           subject the applet keyed its target under
--   device  the device global to write
--   devidx  the card profile device within it
--   index   the route index within that device
local last_announcement = nil

local function announce ()
  if route_md == nil then return end

  local sink_id = announce_sink_id ()
  if sink_id == nil then return end

  local m = sink_map [sink_id]
  if m == nil then return end

  local dom = object_manager ("device")
  if dom == nil then return end
  local device = node_by_id (dom, m.device)
  if device == nil then return end

  local index = route_index_for (device, m.dev_idx)
  if index == nil then return end

  local payload = string.format ("sink=%d device=%d devidx=%d index=%d",
      sink_id, m.device, m.dev_idx, index)

  -- Nothing changed, so say nothing.
  --
  -- Everything that can move the answer re-announces, and during a click
  -- burst the answer does not move at all, it is the same output the whole
  -- time; measured at 56 identical announcements in eight seconds of
  -- clicking. Each one was a metadata write, and the module answers a
  -- metadata write by resolving the device, binding the control and
  -- putting the level on the codec again, which is a hardware write and a
  -- device re-announce, which is more events for this script to answer.
  -- The loop is not infinite but it is not idle either.
  if payload == last_announcement then
    return
  end
  last_announcement = payload

  log:info (string.format ("routing: %s", payload))

  pcall (function ()
      route_md:set (0, ROUTE_KEY, "Spa:String", payload)
    end)
end

----------------------------------------------------------------------------
-- metadata binding
----------------------------------------------------------------------------

-- Bind the default namespace. It is where the applet writes the user's
-- target and where the framework announces the default sink, and the
-- only namespace whose changes we are told about.
local function ensure_default_metadata ()
  if default_md ~= nil then return default_md end
  local mom = object_manager ("metadata")
  if mom == nil then return nil end

  local ok, found = pcall (function ()
      return mom:lookup { Constraint { "metadata.name", "=", DEFAULT_NS } }
    end)
  if not ok or found == nil then return nil end

  found:connect ("changed", function (_, subject, key, _, value)
    -- React to both keys. `default.audio.sink` moves when the resolved
    -- default changes; `default.configured.audio.sink` moves when the
    -- user picks a device or WP's priority logic does. The second is the
    -- one that carries intent, and without watching it an explicit
    -- `wpctl set-default` would sit unread until some other event
    -- happened to wake us.
    if key == DEFAULT_SINK_KEY or key == CONFIGURED_SINK_KEY then
      -- Subject 0 is the global key space, so this is the framework
      -- announcing a new default rather than a per-node change. This
      -- is the failover path: Bluetooth connects, headphones unplug,
      -- the user picks something else, and the module must follow
      -- within the same event rather than waiting for something else
      -- to notice.
      if subject == 0 then
        if debug then
          log:info (string.format ("%s moved to %s",
              key, tostring (value)))
        end
        announce ()
      end
    end
  end)

  default_md = found
  return found
end

-- Bind our own namespace, which the module creates.
local function ensure_route_metadata ()
  if route_md ~= nil then return route_md end
  local mom = object_manager ("metadata")
  if mom == nil then
    if debug then log:info "route md: no metadata object manager" end
    return nil
  end

  local ok, found = pcall (function ()
      return mom:lookup { Constraint { "metadata.name", "=", ROUTE_NS } }
    end)
  if not ok or found == nil then
    if debug then
      log:info (string.format ("route md: '%s' not found (ok=%s err=%s)",
          ROUTE_NS, tostring (ok), tostring (found)))
    end
    return nil
  end

  route_md = found
  return found
end

-- Everything that can change the answer, re-announced.
refresh = function ()
  if source == nil then return end
  if ensure_route_metadata () == nil then
    return
  end
  ensure_default_metadata ()
  update_sink_map ()
  ensure_relay ()
  -- Aim every playback stream at the mixer.  Without this a stream that
  -- was already linked to a real sink stays there: the default only
  -- decides where a *new* stream goes, so the music player that started
  -- before the mixer existed keeps playing straight into alsa_output and
  -- never reaches the mixer at all -- and, worse, never follows a device
  -- change, because the thing that moves on a device change is the
  -- relay, and it is downstream of a stream that is not in it.
  redirect_all_streams (mixer_node_id ())
  announce ()
end

-- The script's own source object arrives on whichever event fires
-- first; there is no "script loaded" event to wait for, so every hook
-- feeds it in. Until then nothing can be looked up, and announcing
-- nothing is correct: the next event will.
local function set_source (event)
  if event:get_source () == nil then return end
  source = event:get_source ()
end

-- Outputs appearing and disappearing changes both the map and possibly
-- the default.
hook_sinks = SimpleEventHook {
  name = "policy/maemo-routing@sinks",
  interests = {
    EventInterest {
      Constraint { "event.type", "c", "node-added", "node-removed" },
      Constraint { "media.class", "matches", "Audio/Sink" },
    },
  },
  execute = function (event)
    set_source (event)
    refresh ()
  end,
}:register()

-- A new playback stream must be redirected too, otherwise it links
-- straight to the device and bypasses the mixer for its whole life.
hook_streams = SimpleEventHook {
  name = "policy/maemo-routing@streams",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "node-added" },
      Constraint { "media.class", "matches", "Stream/Output/Audio" },
    },
  },
  execute = function (event)
    set_source (event)
    refresh ()
  end,
}:register()

-- The route on the device can move without the default sink changing --
-- a port switch, or the card profile settling after a plug event.
hook_route = SimpleEventHook {
  name = "policy/maemo-routing@route",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "device-params-changed" },
      Constraint { "event.subject.param-id", "=", "Route" },
    },
  },
  execute = function (event)
    set_source (event)
    refresh ()
  end,
}:register()

-- The mixer's session item appears *after* its node does: WirePlumber
-- turns a node into a session item in its own time, and the relay is
-- built from session items, not nodes. Without this hook every refresh()
-- on a cold start finds the node but no session item for it, drops the
-- relay, and is never asked again -- which looks exactly like a mixer
-- that does not work, but only when the whole stack starts together.
-- Restarting wireplumber against a running daemon hides it, because by
-- then the session item already exists.
hook_session_items = SimpleEventHook {
  name = "policy/maemo-routing@session-items",
  interests = {
    EventInterest {
      Constraint { "event.type", "c", "session-item-added", "session-item-removed" },
    },
  },
  execute = function (event)
    set_source (event)
    refresh ()
  end,
}:register()

-- Our namespace appears when the module registers it; re-announce then
-- so the module is not left waiting for the next routing change.
hook_metadata = SimpleEventHook {
  name = "policy/maemo-routing@metadata",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "metadata-registered" },
    },
  },
  execute = function (event)
    set_source (event)
    refresh ()
  end,
}:register()

-- Devices appearing is also the earliest event that reliably fires on a
-- cold start, before any sink exists.
hook_devices = SimpleEventHook {
  name = "policy/maemo-routing@devices",
  interests = {
    EventInterest {
      Constraint { "event.type", "c", "device-added", "device-removed" },
    },
  },
  execute = function (event)
    set_source (event)
    refresh ()
  end,
}:register()

Script:finish_activation ()
