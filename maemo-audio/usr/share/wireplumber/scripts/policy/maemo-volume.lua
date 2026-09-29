-- Maemo volume policy
--
-- The volume applet no longer sets the sink.  It publishes what the user
-- asked for and this script turns that into a route volume.
--
--   applet  --metadata-->  x-maemo.volume.target   (subject = sink node id)
--   policy  --Route---->   channelVolumes on the card's active port
--
-- The target lives in the default metadata namespace next to
-- default.audio.sink, which is the same kind of thing: policy relevant,
-- written by clients, watched by us.  Nothing new has to be created for
-- it and the applet keeps binding the one object it already binds.
--
-- Writing the route rather than the node is the whole point.  A sink's
-- level lives on the card route: pipewire-pulse writes it with
-- pw_device_set_param(Route, save=true) and reads it back out of that
-- same route, so a bare node write leaves every PulseAudio client quoting
-- a stale number and skips the per-port persistence that state-routes.lua
-- performs for us.  Asking for save=true is what makes headphones and
-- speakers keep their own levels across a restart.
--
-- Absolute streams.  A stream that carries
--
--   x-maemo.volume.absolute = 1      (a node property, not metadata)
--
-- is saying that its own volume is the level it wants to be heard at,
-- wherever the slider sits.  That is Fremantle's volume_is_absolute: the
-- flag changes how the value is read, it does not carry a second value.
-- The output goes to the loudest such claim and every stream is pulled
-- back to its own target from there:
--
--   S       = max(target, all absolute targets)
--   output  = S
--   normal  master         = target - S      (channelVolumes untouched)
--   absolute channelVols   = own - S       (master pinned at unity)
--
-- Absolute streams get their channel volumes rewritten because the target
-- and the applied gain are the same number; leaving the application's own
-- gain in place would apply it twice.  What we write is a compensation,
-- not something the application asked to have remembered, so the loader
-- conf carries a stream rule that sets state.restore-props=false on every
-- stream bearing the flag.  Ordinary streams are not matched there, which
-- is what keeps a per-application level such as VLC's surviving.
--
-- The flag arrives as a node property because that is the only channel a
-- client has before its stream runs.  libcanberra forwards every property
-- it is given (convert_proplist has no whitelist), pipewire-pulse
-- deserialises TAG_PROPLIST straight into pw_properties, and read_props
-- stores unknown keys verbatim -- so a property set on the play call is
-- on the node before a single sample is queued.  Metadata cannot do that:
-- it is written after the fact, and a click is over in fifty milliseconds.
--
-- Ordinary streams are compensated through the master volume, a scalar no
-- PulseAudio client writes.  PulseAudio's pa_cvolume has no master at all,
-- so the compatibility layer has nowhere to put one: pipewire-pulse parses
-- SPA_PROP_volume into volume_info.level and then never reads that field
-- anywhere in the module.  That is why the master is free to own.  It is a
-- fact about the current code rather than a documented contract, so if a
-- future pipewire-pulse ever starts writing the master this policy would
-- have to move.
--
-- The slider is never inferred from the output.  That is what keeps a
-- lift for an alarm from rebasing the user's ladder, and it is why the
-- applet reads the target rather than the sink.

Script.async_activation = true

local cutils = require ("common-utils")

local TARGET_KEY = "x-maemo.volume.target"
local ABS_KEY = "x-maemo.volume.absolute"
-- The delayed route writer (module-maemo-volume) owns its own metadata
-- namespace rather than the daemon's default one, because a listener
-- attached to the default metadata from inside the daemon crashes on the
-- first permissions update.  Writing ROUTE_KEY into that namespace hands
-- the route write to the module, which waits out the live ring fill and
-- applies it in the graph driver thread.  Absent namespace means no
-- module, and routes are written directly and undelayed.
local ROUTE_NS  = "x-maemo-volume"
local ROUTE_KEY = "x-maemo.route-volume"
-- The applet's step scale: 65536 is 0 dB and the mapping to amplitude
-- is the perceptual cube.  Centibels here are true centibels, so -60 dB
-- is -6000 and amplitude 0.001 is -6000 as well.
local STEP_NORM = 65536

local MIN_CB = -6000
local MAX_CB = 1200

local log = Log.open_topic ("s-policy")

----------------------------------------------------------------------------
-- units
----------------------------------------------------------------------------

local function clamp_cb (cb)
  if cb < MIN_CB then return MIN_CB end
  if cb > MAX_CB then return MAX_CB end
  return cb
end

local function step_to_cb (step)
  if step == nil or step <= 0 then return MIN_CB end
  return math.floor (6000 * math.log (step / STEP_NORM, 10) + 0.5)
end

local function cb_to_amp (cb)
  return 10 ^ (cb / 2000)
end

local function amp_to_cb (amp)
  if amp == nil or amp <= 0 then return MIN_CB end
  return math.floor (2000 * math.log (amp, 10) + 0.5)
end

local function cb_to_step (cb)
  local step = math.floor (STEP_NORM * (10 ^ (cb / 6000)) + 0.5)
  if step < 0 then step = 0 end
  if step > STEP_NORM then step = STEP_NORM end
  return step
end

local function close_enough (cur, want)
  return cur ~= nil and math.abs (cur - want) <= math.max (want, 1e-9) * 1e-4
end

----------------------------------------------------------------------------
-- state
----------------------------------------------------------------------------

-- What the user asked for, per sink bound id, in centibels.
local target = {}
-- The level each absolute stream wants to be heard at, per stream bound
-- id, in centibels.
local abs_target = {}
-- The amplitude we last wrote to each absolute stream's channel volumes,
-- so that our own write coming back around is not mistaken for the client
-- changing its mind.
local abs_written = {}
-- The amplitude we last pushed at each sink, so a change that is not ours
-- can be recognised as somebody else moving the output.
local pushed = {}
-- link id -> { stream = <node bound id>, sink = <node bound id> }
local link_ends = {}
-- The node-state-changed state string per stream bound id.  A sample that
-- has finished but is being kept warm for reuse still sits here with the
-- level of the playback that ended, so the level alone cannot tell a
-- request that is sounding from one that is merely left behind.
local stream_state = {}
-- Stream bound ids whose claim has been released.  A claim is armed by
-- the level becoming known and is released only once the stream has
-- actually sounded and then gone quiet, so a stream that has never run
-- is not in this table and still counts.
local claim_released = {}
-- Set MAEMO_VOL_DEBUG=1 in the environment the policy is loaded under to
-- get per-stream detail on every recompute.  The ordinary log says what
-- the output did; this says why, stream by stream -- which streams were
-- counted as claiming, which were left alone and on what grounds, and
-- what gain each was handed.  That is what is needed to chase a level
-- that landed somewhere other than where it was asked for.  Off by
-- default: it is a line per stream per event.
local debug = (os.getenv ("MAEMO_VOL_DEBUG") ~= nil)
-- The event source, which is the only way to reach the object managers.
local source = nil
-- The default metadata, bound once.
local md = nil
-- The module's metadata namespace, resolved on first use and cached once
-- found.  Not cached when absent: the module is loaded at daemon startup
-- and may legitimately not be registered yet when this script first runs.
local route_md = nil
-- Sinks that changed before we could act on them.
local pending = {}

local function object_manager (kind)
  if source == nil then return nil end
  local ok, om = pcall (function () return source:call ("get-object-manager", kind) end)
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

local function read_target (sink_id)
  if md == nil or target[sink_id] ~= nil then return end
  local ok, value = pcall (function () return md:find (sink_id, TARGET_KEY) end)
  if ok and value ~= nil then
    local n = tonumber (value)
    if n ~= nil then target[sink_id] = step_to_cb (n) end
  end
end

----------------------------------------------------------------------------
-- reading levels
----------------------------------------------------------------------------

local function channels_of (props)
  local src = props.channelVolumes
  if src == nil then return nil end
  local out = {}
  for i = 0, 64 do
    local v = src[i]
    if type (v) == "number" then out[#out + 1] = v end
  end
  if #out == 0 then return nil end
  return out
end

-- parseParam hands back a flat table for Props: channelVolumes sits on the
-- table itself.  Only the Route param nests its values under .properties.
local function node_channels (node)
  for p in node:iterate_params ("Props") do
    local props = cutils.parseParam (p, "Props")
    if props ~= nil then
      local chans = channels_of (props)
      if chans ~= nil then return chans end
    end
  end
  return nil
end

local function loudest_of (chans)
  if chans == nil then return nil end
  local top = chans[1]
  for _, v in ipairs (chans) do
    if v > top then top = v end
  end
  return top
end

local function node_amp (node)
  return loudest_of (node_channels (node))
end

local function node_master (node)
  for p in node:iterate_params ("Props") do
    local props = cutils.parseParam (p, "Props")
    if props ~= nil and props.volume ~= nil then return props.volume end
  end
  return nil
end

local function is_playback (node)
  return (node.properties["media.class"] or "") == "Stream/Output/Audio"
end

-- The flag is a client-set node property.  Accept the two spellings a
-- proplist is likely to carry rather than trusting one.
local function is_absolute (node)
  local v = node.properties[ABS_KEY]
  if v == nil then return false end
  v = tostring (v)
  return v == "1" or v == "true"
end

-- The amplitude a stream holds before it has said anything, which is the
-- default channel volume.  It is the only value that can be told apart
-- from a level the client chose, and under the flag every other value is
-- a level the client chose.
local UNSET_AMP = 1.0

-- A flagged stream's own volume is its heard level, so that volume has to
-- be read before anything is written to it.
--
-- Unity means "not set yet".  A node is announced before the client has
-- spoken -- some clients leave the field at the default until they set it,
-- others never set it at all -- and taking unity for a request would lift
-- the whole output to full scale on every click.  Anything other than
-- unity is the request.  A stream that never leaves unity simply never
-- makes a claim, which degrades to ordinary playback rather than to
-- full volume.
--
-- This is what the ordering actually looks like.  A client that supplies
-- its volume with pa_stream_connect_playback, which is what libcanberra
-- does, arrives as: no channel volumes, no channel volumes, then the
-- level.  A client that sets it afterwards arrives as: unity, then the
-- level.  Both are handled by ignoring unity.
local function observe_absolute (node)
  local id = node.bound_id
  local cur = node_amp (node)
  if cur == nil then return end

  -- Our own write coming back around.
  if abs_written[id] ~= nil and close_enough (cur, abs_written[id]) then return end

  -- Still the default: the client has said nothing.
  if abs_target[id] == nil and close_enough (cur, UNSET_AMP) then return end

  abs_target[id] = amp_to_cb (cur)
  log:info (string.format ("stream %d is absolute at %d cB", id, abs_target[id]))
end

-- Whether this stream's claim on the output is live.
--
-- Arming is deliberately early and release is deliberately late.
--
-- Arming on "running" is what made a click land at the wrong level.  The
-- level the click wants is known well before the stream starts -- the
-- transport sets it while the stream is still paused -- and waiting for
-- the running transition left the output sitting at the slider for the
-- first ten-odd milliseconds of a sound that is only thirty milliseconds
-- long.  The click was already playing by the time the lift landed.
--
-- Releasing only after the stream has really sounded is what stops a
-- pooled stream from pinning the output.  A cached stream that has
-- drained still carries the level of the last thing it played, and that
-- is a memory rather than a request, so it stops counting once the
-- stream has gone quiet.  A stream that has never run keeps counting,
-- because the only thing that will ever run it is the playback that
-- asked for this level.
local function claim_is_live (node)
  return claim_released[node.bound_id] ~= true
end

----------------------------------------------------------------------------
-- writing
----------------------------------------------------------------------------

-- The active route for the device this sink was opened on.  A node names
-- both the device and the device index within it; the route index is the
-- port the device currently has active for that index.
local function output_route (sink)
  local dom = object_manager ("device")
  if dom == nil then return nil end

  local dev_id = tonumber (sink.properties["device.id"])
  local dev_idx = tonumber (sink.properties["card.profile.device"])
  if dev_id == nil or dev_idx == nil then return nil end

  local device = node_by_id (dom, dev_id)
  if device == nil then return nil end

  for p in device:iterate_params ("Route") do
    local r = cutils.parseParam (p, "Route")
    if r ~= nil and r.device == dev_idx then
      return device, r.index, dev_idx, r.props
    end
  end
  return nil
end

-- Scale every channel by the same factor so an existing left/right
-- balance survives the change.
local function scaled_channels (current, amp)
  if current == nil or #current == 0 then return { amp } end
  local top = loudest_of (current)
  local out = {}
  for _, v in ipairs (current) do
    out[#out + 1] = (top > 0) and (v * (amp / top)) or amp
  end
  return out
end

local function pod_of_floats (amps)
  local vols = { "Spa:Float" }
  for _, v in ipairs (amps) do vols[#vols + 1] = v end
  return Pod.Array (vols)
end

-- Resolve the delayed route writer's namespace, or nil if it is not
-- loaded.  Deliberately re-checked until found so that a policy which
-- activated before the module registered still picks it up.
local function ensure_route_md ()
  if route_md ~= nil then return route_md end
  local mom = object_manager ("metadata")
  if mom == nil then return nil end
  for o in mom:iterate { Constraint { "metadata.name", "=", ROUTE_NS } } do
    route_md = o
    log:info ("maemo-volume: delayed route writer present, route writes go through it")
    return route_md
  end
  return nil
end

-- The payload the module expects.  Volumes are plain decimals; the module
-- parses them back into floats and writes them as the route's channel
-- volumes at the moment it has chosen.
local function route_payload (index, dev_idx, dev_id, vols, muted)
  local parts = {}
  for _, v in ipairs (vols) do parts[#parts + 1] = string.format ("%.6f", v) end
  return string.format ("index=%d devidx=%d device=%d vol=%s mute=%s save=1",
                       index, dev_idx, dev_id, table.concat (parts, ","),
                       muted and "1" or "0")
end

local function apply_route (sink, amp)
  local device, index, dev_idx, props = output_route (sink)
  if device == nil then return false end

  local current, muted = nil, false
  if props ~= nil and props.properties ~= nil then
    current = channels_of (props.properties)
    muted = props.properties.mute or false
  end

  local vols = scaled_channels (current, amp)
  local dev_id = tonumber (sink.properties ["device.id"])

  -- Hand the write to the delayed route writer when it is loaded.  It
  -- waits out the live ring fill and applies the level from the graph
  -- driver thread, so the change lands as the samples recorded without
  -- it drain out rather than on top of them.
  --
  -- This does not loop: the write comes back through device-params-changed
  -- and the pushed[] guard in the caller recognises it as ours, exactly as
  -- it recognises a direct write.
  if dev_id ~= nil then
    local rmd = ensure_route_md ()
    if rmd ~= nil then
      local ok, err = pcall (function ()
        rmd:set (dev_id, ROUTE_KEY, "String",
                 route_payload (index, dev_idx, dev_id, vols, muted))
      end)
      if ok then return true end
      log:warn (string.format ("maemo-volume: handoff failed (%s), writing directly",
                              tostring (err)))
    end
  end

  device:set_param ("Route", Pod.Object {
    "Spa:Pod:Object:Param:Route", "Route",
    index = index,
    device = dev_idx,
    props = Pod.Object {
      "Spa:Pod:Object:Param:Props", "Route",
      mute = muted,
      channelVolumes = pod_of_floats (scaled_channels (current, amp)),
    },
    -- save=true is what makes state-routes.lua store these props against
    -- this port, which is where per-output persistence comes from.
    save = true,
  })
  return true
end

-- Writing a stream's Props makes it emit node-params-changed/Props, which
-- is exactly what the @stream interest watches.  Without a guard the write
-- re-triggers the hook, which recomputes, which writes again, and the
-- loop runs at about a thousand iterations a second for as long as an
-- absolute stream is live.  Compare against what the node already has
-- instead of remembering our own last value, so a level set by somebody
-- else still gets picked up.

-- Ordinary playback: the master only.  channelVolumes stays the
-- application's own, so session restore keeps remembering what the user
-- set.  Setting the master to plain unity when nothing is claimed is also
-- what clears a stale value restored from an earlier session.
local function apply_master (node, amp)
  if close_enough (node_master (node), amp) then return false end
  node:set_param ("Props", Pod.Object {
    "Spa:Pod:Object:Param:Props", "Props",
    volume = amp,
  })
  return true
end

-- An absolute stream has its channel volumes rewritten, because the
-- target and the applied gain are the same number and leaving the
-- application's own gain in place would apply it twice.  The master is
-- pinned at unity so a stale restored value cannot add to the result.
local function apply_absolute (node, amp)
  if close_enough (node_amp (node), amp) then return false end
  node:set_param ("Props", Pod.Object {
    "Spa:Pod:Object:Param:Props", "Props",
    volume = 1.0,
    channelVolumes = pod_of_floats (scaled_channels (node_channels (node), amp)),
  })
  abs_written [node.bound_id] = amp
  return true
end

----------------------------------------------------------------------------
-- the policy
----------------------------------------------------------------------------

local function recompute (sink_id)
  local nodes = object_manager ("node")
  local sink = node_by_id (nodes, sink_id)
  if sink == nil then return false end

  local streams = {}
  for _, ends in pairs (link_ends) do
    if ends.sink == sink_id then
      local node = node_by_id (nodes, ends.stream)
      if node ~= nil and is_playback (node) then
        streams[#streams + 1] = { node = node, abs = is_absolute (node) }
      end
    end
  end

  -- Only streams whose claim is live contribute.  See claim_is_live().
  local claim = nil
  for _, s in ipairs (streams) do
    if s.abs and claim_is_live (s.node) then
      local cb = abs_target[s.node.bound_id]
      if cb ~= nil and (claim == nil or cb > claim) then claim = cb end
    end
  end

  local want = target[sink_id]
  if want == nil then want = MIN_CB end
  local level = clamp_cb (math.max (want, claim or want))
  local amp = cb_to_amp (level)

  -- The route write and the stream writes are not one transaction, and
  -- they do not land together.  The route write goes to the ALSA device
  -- and was measured on device taking about 170 ms to take effect, while
  -- the stream writes land almost at once.  So the order has to follow
  -- the direction of the change rather than stay fixed.
  --
  -- Raising the output: pull the streams down first, so nothing is left
  -- sitting at unity against an already-raised sink.
  --
  -- Lowering the output: drop the sink first, so nothing is restored to
  -- unity against a sink that has not come down yet.  Getting this half
  -- wrong is what made background music play loud for a sixth of a
  -- second after a click ended -- the stream master went back to unity
  -- at the moment of release while the hardware volume held the lifted
  -- level for another 170 ms, and the music rode that difference.
  local lifting = (pushed [sink_id] == nil) or (amp >= pushed [sink_id])

  local function write_route ()
    if pushed[sink_id] == nil or math.abs (pushed[sink_id] - amp) > 0.0005 then
      if not apply_route (sink, amp) then return false end
      pushed[sink_id] = amp
    end
    return true
  end

  if not lifting and not write_route () then return false end

  local dbg = {}

  -- Every gain written here is <= 0 dB: the output is the max of the
  -- slider and the loudest live claim, so a stream's own target minus
  -- the output can only be a reduction.  Nothing is pushed above unity,
  -- so a claim is exact rather than merely a floor -- turning the volume
  -- up does not make a notification louder.
  --
  -- Ordinary streams are compensated whether or not they are sounding.
  -- Their gain is want - level and level is never below want, so it is
  -- never a boost and there is nothing to guard against.  Leaving an idle
  -- one alone means the output lifts for a click while that stream still
  -- sits at unity, and background music comes through at the click's
  -- level until some later event happens to correct it.
  --
  -- A released absolute stream is left alone.  Its own level is out of
  -- the max, so compensating it could ask for gain above unity, and it
  -- has no sound to correct anyway.  Re-arming it triggers a recompute
  -- that writes the right value before it can be heard.
  for _, s in ipairs (streams) do
    local t = abs_target[s.node.bound_id]
    if s.abs and t ~= nil and claim_is_live (s.node) then
      local g = clamp_cb (t - level)
      local before = node_amp (s.node)
      local wrote = apply_absolute (s.node, cb_to_amp (g))
      if debug then
        dbg[#dbg + 1] = string.format (
            "    s%d abs, claim live, wants %d cB -> %d cB (%s, read back %.4f -> %.4f)",
            s.node.bound_id, t, g,
            (wrote and "written" or "skipped: guard thought it was already there"),
            before or -1, node_amp (s.node) or -1)
      end
    elseif not s.abs then
      local cb = 0
      if claim ~= nil then cb = want - level end
      local before = node_master (s.node)
      local wrote = apply_master (s.node, cb_to_amp (cb))
      if debug then
        dbg[#dbg + 1] = string.format (
            "    s%d ordinary -> master %d cB (%s, read back %.4f -> %.4f)",
            s.node.bound_id, cb,
            (wrote and "written" or "skipped: guard thought it was already there"),
            before or -1, node_master (s.node) or -1)
      end
    else
      if debug then
        dbg[#dbg + 1] = string.format (
            "    s%d abs, %s, level %s -> left alone",
            s.node.bound_id,
            (claim_is_live (s.node) and "level unknown" or "claim released"),
            tostring (t))
      end
    end
  end

  if lifting and not write_route () then return false end

  local msg = string.format (
      "sink %d -> %d cB (target %d cB, claim %s), %d stream(s)",
      sink_id, level, want, tostring (claim), #streams)
  if debug and #dbg > 0 then
    msg = msg .. "\n" .. table.concat (dbg, "\n")
  end
  log:info (msg)
  return true
end

local function flush_pending ()
  if source == nil then return end
  for sink_id in pairs (pending) do
    if recompute (sink_id) then pending[sink_id] = nil end
  end
end

-- Publish the output's level as the user's target.  Used when something
-- other than us moved the output -- a port switch restoring the level
-- stored for the new port -- so the slider follows without the applet
-- ever having to read the sink.
local function adopt (sink_id, amp)
  if md == nil then return end
  local cb = amp_to_cb (amp)
  local step = cb_to_step (cb)
  md:set (sink_id, TARGET_KEY, "Spa:Int", tostring (step))
  log:info (string.format ("adopted output level %d cB as target %d on sink %d",
      cb, step, sink_id))
end

-- Bind the default metadata once.  It is where the applet writes, and
-- the only namespace whose changes we are told about.
local function ensure_metadata ()
  if md ~= nil then return md end
  local mom = object_manager ("metadata")
  if mom == nil then return nil end

  local ok, found = pcall (function ()
    return mom:lookup { Constraint { "metadata.name", "=", "default" } }
  end)
  if not ok or found == nil then return nil end

  found:connect ("changed", function (_, subject, key, _, value)
    if key ~= TARGET_KEY then return end

    local n = tonumber (value)
    if n == nil then
      log:warn (string.format ("maemo-volume: %s on %s is not a number",
          key, tostring (subject)))
      return
    end

    target[subject] = step_to_cb (n)
    log:info (string.format ("target for sink %s is %d cB (step %d)",
        tostring (subject), target[subject], n))
    pending[subject] = true
    flush_pending ()
  end)

  md = found
  log:info ("maemo-volume: watching the default metadata namespace")
  return md
end

----------------------------------------------------------------------------
-- hooks
----------------------------------------------------------------------------

local function set_source (event)
  if event:get_source () == nil then return end
  source = event:get_source ()
  ensure_metadata ()
end

-- sink bound id -> { device = <device bound id>, dev_idx = <int> }
sink_map = {}

local function update_sink_map ()
  local nom = object_manager ("node")
  if nom == nil then return end
  for node in nom:iterate () do
    if (node.properties["media.class"] or "") == "Audio/Sink" then
      local dev = tonumber (node.properties["device.id"])
      local idx = tonumber (node.properties["card.profile.device"])
      if dev ~= nil and idx ~= nil then
        sink_map[node.bound_id] = { device = dev, dev_idx = idx }
      end
    end
  end
end

local function route_amp (props)
  if props == nil or props.properties == nil then return nil end
  return loudest_of (channels_of (props.properties))
end

-- The route is the store, so it is what a change has to be read from.
-- The sink node mirrors it only while the sink is running; suspended, it
-- keeps whatever it last had, which is why watching node Props cannot be
-- used to notice that the output moved.
SimpleEventHook {
  name = "policy/maemo-volume@route",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "device-params-changed" },
      Constraint { "event.subject.param-id", "=", "Route" },
    },
  },
  execute = function (event)
    set_source (event)
    update_sink_map ()

    local device = event:get_subject ()
    for p in device:iterate_params ("Route") do
      local r = cutils.parseParam (p, "Route")
      if r ~= nil then
        for sink_id, m in pairs (sink_map) do
          if m.device == device.bound_id and m.dev_idx == r.device then
            local amp = route_amp (r.props)
            if amp ~= nil
                and (pushed[sink_id] == nil
                     or math.abs (pushed[sink_id] - amp) > 0.0005) then
              read_target (sink_id)
              if target[sink_id] ~= nil
                  and amp_to_cb (amp) == target[sink_id] then
                pushed[sink_id] = amp
              else
                -- Not ours: the initial seed, or a port switch restoring
                -- the level stored for the port that is active now.
                adopt (sink_id, amp)
              end
            end
          end
        end
      end
    end
    flush_pending ()
  end,
}:register()

-- Keep the sink map current as outputs appear and disappear.
SimpleEventHook {
  name = "policy/maemo-volume@sinks",
  interests = {
    EventInterest {
      Constraint { "event.type", "c", "node-added", "node-removed" },
      Constraint { "media.class", "matches", "Audio/Sink" },
    },
  },
  execute = function (event)
    set_source (event)
    update_sink_map ()
    flush_pending ()
  end,
}:register()

-- The stream's Props changes are where its own volume arrives, and for an
-- absolute stream a change from the arrival value is the target.  Our own
-- writes land here too; observe_absolute recognises them, so the two
-- cannot be confused.
SimpleEventHook {
  name = "policy/maemo-volume@stream",
  interests = {
    EventInterest {
      Constraint { "event.type", "c", "node-added", "node-params-changed" },
      Constraint { "event.subject.param-id", "=", "Props" },
      Constraint { "media.class", "matches", "Stream/Output/Audio" },
    },
  },
  execute = function (event)
    set_source (event)
    local node = event:get_subject ()
    if is_absolute (node) then observe_absolute (node) end
    for _, ends in pairs (link_ends) do
      if ends.stream == node.bound_id then
        pending[ends.sink] = true
      end
    end
    flush_pending ()
  end,
}:register()

-- Tracks who is actually making a sound.  The state arrives as a string
-- on the transition, so it has to be remembered; there is no cheap way
-- to ask a node for it later.
SimpleEventHook {
  name = "policy/maemo-volume@stream-state",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "node-state-changed" },
      Constraint { "media.class", "matches", "Stream/Output/Audio" },
    },
  },
  execute = function (event)
    set_source (event)
    local node = event:get_subject ()
    local id = node.bound_id
    local new_state = event:get_properties ()["event.subject.new-state"]

    local prev = stream_state[id]
    if prev == new_state then return end
    local was_running = (stream_state[id] == "running")
    stream_state[id] = new_state
    if debug then
      log:info (string.format (
          "s%d %s -> %s (was_running %s, claim %s)",
          id, tostring (prev), tostring (new_state),
          tostring (was_running),
          tostring (claim_released[id] ~= true)))
    end

    -- Running arms; going quiet releases, but only after the stream has
    -- actually sounded.  See claim_is_live().
    if new_state == "running" then
      claim_released[id] = nil
    elseif was_running then
      claim_released[id] = true
    end

    for _, ends in pairs (link_ends) do
      if ends.stream == id then pending[ends.sink] = true end
    end
    flush_pending ()
  end,
}:register()

-- A claim belongs to the stream that made it.  Our copy would survive the
-- stream, and bound ids are recycled -- so a later stream could wake up
-- already holding a level its real owner asked for long ago.  The subject
-- is already dead by the time we are told, so read the identity out of
-- the event itself.
SimpleEventHook {
  name = "policy/maemo-volume@stream-gone",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "node-removed" },
      Constraint { "media.class", "matches", "Stream/Output/Audio" },
    },
  },
  execute = function (event)
    set_source (event)
    local props = event:get_properties ()
    local id = tonumber (props["object.id"])
    if id == nil then return end

    if abs_target[id] ~= nil then
      log:info (string.format ("stream %d gone, was absolute at %d cB",
          id, abs_target[id]))
    end
    abs_target[id] = nil
    abs_written[id] = nil
    stream_state[id] = nil
    claim_released[id] = nil

    for _, ends in pairs (link_ends) do
      if ends.stream == id then pending[ends.sink] = true end
    end
    flush_pending ()
  end,
}:register()

-- Playback moves between outputs.
SimpleEventHook {
  name = "policy/maemo-volume@links",
  interests = {
    EventInterest {
      Constraint { "event.type", "c", "link-added", "link-removed" },
    },
  },
  execute = function (event)
    set_source (event)

    local link = event:get_subject ()
    if event:get_properties ()["event.type"] == "link-removed" then
      local ends = link_ends[link.id]
      link_ends[link.id] = nil
      if ends ~= nil then
        pending[ends.sink] = true
        flush_pending ()
      end
      return
    end

    local stream_id = tonumber (link.properties["link.output.node"])
    local sink_id = tonumber (link.properties["link.input.node"])
    if stream_id == nil or sink_id == nil then return end
    link_ends[link.id] = { stream = stream_id, sink = sink_id }
    pending[sink_id] = true
    flush_pending ()
  end,
}:register()

Script:finish_activation ()
