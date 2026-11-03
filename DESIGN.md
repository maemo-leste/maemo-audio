# Maemo volume: the model

Read this before changing anything in `mixer/` or `module/module-maemo-mix.c`.

For anything involving latency, timing or "when does a change take effect", also
read [`docs/timing.md`](docs/timing.md). It records what has actually been
measured, which numbers were derived and later found wrong, and the measurement
hygiene that was learned by getting it wrong. Do not re-derive a timing constant
without checking it.

## The one rule

**The mixer mixes. The hardware sets the level.**

Everything else follows from that. The mixer sums its inputs and applies one
software gain to the result, and that gain is 1.0. The loudness the user sets
is the loudness the amplifier produces, with nothing multiplied in software on
the way.

## The chain

```
applet writes x-maemo.volume.target   0..65536, cubic in amplitude
      -> amplitude                     amp = (step / 65536)^3
      -> rung of the device's ladder   nearest rung, not a continuum
      -> the hardware step that carries that rung
      -> that step's amplitude, written to the ALSA route
```

The cube is not decoration. The applet's scale is the PulseAudio perceptual
step, which is the cube root of amplitude; dividing by 65536 alone reads the
step scale as amplitude and collapses the ladder — the -4000 cB rung arrives
as 0.2154 instead of 0.01, and the bottom 26.7 dB of the slider disappears.

## The software gain stays, at unity

The mixer's gain path is not dead code and is not to be removed. It is where the
software duck for absolute-volume playback lives (see *Absolute volume* below),
and it was kept for exactly that reason: leaving it in place made that feature a
repointing rather than a rebuild. It is set to 1.0 explicitly, not left implicit
in a zeroed struct, because unity is a decision here and not an absence.

The param is advertised **read** and deliberately *not* read-write.
WirePlumber sets a volume on every sink it manages; if this were writable from
outside, its policy and the device's own hardware level would both be driving
loudness and would multiply — a slider at full with the mixer quietly at 0.37.
The module that owns the gain writes it in-process, which does not consult the
advertisement.

## The sum is unbounded, on purpose

Nothing clamps. The kernels accumulate into float and store; the ALSA sink's
own volume is at unity; so two streams each near full scale sum past 1.0 and
saturate at the converter.

That is deliberate. Stock `audiomixer` sums the same way, and so did Nokia's
flat-volume sink -- the per-stream residual was bounded, the sum was not.
Adding a limiter or dipping headroom when a second stream appears would change
loudness semantics away from Fremantle to protect a case that is rare in
practice: two songs playing at once is not what a handset volume model was
drawn for.

Worth knowing rather than rediscovering: with the software gain at unity there
is no software stage left that could absorb an overload, so where the clip
happens is a consequence of the design, not an oversight in it.

## Per-device ladders

Each output has its own ladder, its own persisted slider position, and its own
restoring behaviour when it becomes active. The slider re-derives from the
device's own restored hardware level on a switch; it does not carry over.

The ladder is what makes the slider a ladder rather than a fader. With it, the
level the applet shows, the level the codec is asked for and the level any
later reader reports are all the same rung, and nothing has to be reconciled
afterwards. Without it they are three numbers that happen to agree.

## The three publications

Each of these looks incidental and each fails as a silent stack rather than an
error message.

* **The `x-maemo-mix` metadata namespace.** `maemo-routing.lua`'s `refresh()`
  returns early when it is absent. Without it: no relay link, no route
  announcement, no audio — every process up, nothing logged.
* **The ladder, on the output sink.** The applet follows `default.audio.sink`,
  which is a node *name*, and reads that node's properties. The default stays
  the physical device on purpose (see *Who owns what*), so the curve belongs on
  the device sink and not on the mixer: the ladder is that amplifier's response
  and that codec's steps, and our mixer node is device independent, outlives
  every device, and is never the default. A curve on it would be a curve
  belonging to no hardware. A sink with no curve has nothing to map a slider
  position to, and the applet disables the slider rather than invent one.

  The module publishes the fallback onto any Audio/Sink that appears without
  one, which is also what catches a sink node being *recreated* -- a profile
  change or port switch produces a fresh node carrying only what the monitor
  rules gave it, and the announcement that named the previous incarnation does
  not follow it. The lookup is by node name for the same reason: an id or a
  pointer goes stale exactly when the value matters.
* **The level, back onto the same node.** The applet *reads* the level from node
  properties and *writes* the user's choice to metadata. On a fresh daemon the
  metadata key is absent while the hardware level is not, restored per device
  by the route store — so the side that can read the codec has to publish to
  the side that has to display it. Otherwise the slider opens wherever it was
  last left while the codec sits somewhere else.

## Thread ownership

Everything the module does belongs to the main loop: resolving the ladder,
writing the ALSA route, publishing the level back to the node.

It does not touch the data loop. An earlier design hopped there because a
slider move also had to arm a hardware write against the audio clock. Left in
after that requirement went away, it was fatal: `pw_impl_node_update_properties`
off the main loop has the protocol marshal a node-info change to clients while
the node is being changed under it. It does not fail where it goes wrong — it
fails some time later as a `memcpy` to a small integer, with a stack naming the
victim and not the culprit.

## Silence

Silence is amplitude `0`, never a cB value. A cB entry is `10^(cb/2000)`, which
is strictly positive for any finite value, so -4000 cB is still -40 dBFS and
audible through anything better than a tin speaker. The slider's bottom rung is
a literal zero prepended to the ladder; the ladder keeps its own lowest step,
so the bottom of the slider is a rung and not a hole in it.

## Calls

A call owns its route and its ladder and is not ducked by, and does not duck,
media volume. Nothing in the mix path may touch a call's hardware level.

## Who owns what

* the **applet** writes `x-maemo.volume.target` (perceptual 0..65536) on the
  metadata object it binds, and reads the level back from node properties
* **WirePlumber** (`maemo-routing.lua`) owns where audio goes: device, route
  device, route index, and the relay link from the mixer to the real sink
* the **module** owns the ladder, the quantisation, the route write and the
  level published back
* the **mixer** owns the sum and the unity software gain

The module deliberately does **not** own `default.audio.sink`. That property
has to stay a truthful statement of which physical output WirePlumber resolved
— the relay reads it to find where the hardware is. Streams reach the mixer by
their own `target.node` instead, which does not require winning the election.

## A derived value is never stored in the slot it describes

The mixer's software gain and the codec's level are two different things, and
collapsing them into one writable field means every writer of one silently
overwrites the other. That is why the gain is not writable from outside and why
the level the applet displays is published from the codec rather than cached
from the last slider write.

## Absolute volume

A stream may declare itself absolute: `x-maemo.volume.absolute = "1"`, set by
the caller (see `mis-sound.c`) and carried on the stream's node properties.
Such a stream plays at a calibrated level *wherever the slider sits* -- a
notification click is meant to be heard at the same loudness whether the music
is at rung 3 or rung 20.

That cannot be done in software alone, because the absolute stream's own samples
are already at full scale: attenuating everything else in software and leaving
the codec at the slider rung would leave the click below its calibrated level.
So at one instant two things move together:

- the normal streams are **ducked in software**, per input, to the level the
  codec was providing
- the codec is raised to **MAX**, so the absolute stream reaches the converter
  unattenuated

The absolute stream itself is never ducked; it sits at unity throughout.

### The invariant, and why it is not a measurement

`duck x MAX == the slider level`. The duck is therefore *the same number* the
module wrote to the route as the current rung's amplitude -- one value, two
consumers. The product cannot drift, because there is no second copy of it to
drift.

A consequence worth stating: while an absolute stream plays, the slider moves
the **duck**, not the codec. The codec is pinned at MAX, so a slider change
during a notification is a software-only change -- immediate, no hardware
access, no queue entry -- and the codec lands on the new rung when the last
absolute stream ends.

### What the mixer knows, and what the module knows

The split follows knowledge and lifetime, not convenience.

| fact | who can see it | who acts on it |
|---|---|---|
| which streams are absolute | module (node properties) | module publishes it |
| which are running | module (node state) | module folds it into one bit |
| the current rung's amplitude | module (ladder, vol-map) | module publishes it as the duck |
| *when* a change must become audible | mixer (graph position, `clock.delay`) | mixer arms it |
| writing the codec | module (device handle) | module writes at the armed instant |

The mixer never learns what a rung is and the module never learns where the
stream is. The mixer is the timing authority; the module is the only party that
touches hardware, and it **never arms a codec change on its own initiative**.

### The shared word

One 64-bit atomic, published by the module and read by the mixer once per cycle:

```
[63:32] duck (IEEE float bits)   [31] abs_active   [30:0] generation
```

`abs_active` is the only thing whose edge arms a codec change. `duck` may change
freely while `abs_active` is steady -- that is a slider move during absolute
playback, and it must not arm anything. Both change together when an absolute
stream starts, and they must be seen together, which is why they are one word
rather than two: a torn read would arm against a duck that had not landed.

64-bit atomics are lock-free on ARMv7 (`LDREXD`/`STREXD`) and on arm64, but a
toolchain without them falls back silently to `libatomic` and possibly a
spinlock. The build therefore asserts `__GCC_ATOMIC_LLONG_LOCK_FREE == 2`
rather than discovering it on a data loop, and the field is explicitly
8-byte aligned because armhf does not guarantee it for a buried member.

Per-port state is a single bit -- `absolute` -- in a **refcounted** per-port
object allocated by the mixer and referenced by the module. The reference *is*
the identity: while the module holds it the memory cannot be freed, so it may
write at leisure, and a recycled global id cannot make it write into a
different stream's port. This is what the earlier registry tried to achieve with
a generation counter and a "never cache the pointer" warning; refcounting makes
the warning unnecessary.

### Detection, and why one cycle of lag is free

The mixer compares `abs_active` against last cycle's value. Detection may be up
to one quantum late and nothing is harmed: during that window the codec is at
MAX and the normal streams are ducked, which is the correct *level* and only
worse SNR. We accept a quantum of bad SNR deliberately; it is why detection can
be a per-cycle comparison instead of an interrupt.

The deadline is then computed **from the cycle where the duck actually landed**,
not from when the module decided it. That is what closes the loud window: the
param path from the main loop to the data loop costs milliseconds (measured
main-loop tail 4-9 ms), and if the codec were timed from the module's
intention those milliseconds would be milliseconds of un-ducked audio at MAX --
the one direction that must never happen. Timing from the arrival instead makes
the delivery latency irrelevant without needing a margin sized to it.

Both halves become audible at the same instant for a reason that needs no
scheduling: samples written this cycle reach the DAC one pipeline distance
later, which is exactly how far the armed hardware write has to wait.

### The queue is not optional

It lives entirely on the main loop, in Nokia's shape, holding `{ value,
deadline }`. Three independent reasons a single slot cannot work:

- **overlapping absolute streams** -- a click arriving while a notification
  plays has its own release armed; neither may clobber the other
- **the slider staircase** -- key repeat produces a transition every ~100 ms;
  the equivalence harness measured 8 codec writes against Nokia's 4, the
  difference being his pruning of superseded entries
- **ordering** -- a pending raise overwritten by a lower, or a lower processed
  after a newer raise, leaves the codec matching no current state

Nokia's rule -- *we can ignore volume events that came earlier but should
happen later than this* -- is what makes it coalesce rather than merely
buffer. His direction margins come with it: **raise late, lower early**, so
every residual error lands on the quiet side. They are not insurance against
late delivery (a timer cannot fire early) but against a published delay that
underestimates the real pipeline.

### The queue is a transcription, not an approximation

An earlier version of the push was a plausible rewrite of Nokia's and got four
things wrong, each of which changes behaviour under load rather than breaking
it obviously. It is now transcribed, and the four are worth stating because
none of them would have been found by reading the code:

1. **The no-change guard.** If nothing is in flight and the requested level
   equals the level the hardware already holds, the request is dropped. This
   needs the queue to track the hardware's resting level -- Nokia's
   `current_hw`, here `hw_written` -- and without it every notification writes
   the codec twice for no change.
2. **The margin is conditional, not a fixed offset on the direction.** Nokia
   walks back from the newest entry and shifts the new instant past the first
   entry it may overtake, applying the margin only if the shifted instant
   still lands beyond it. A fixed +/-margin per direction lets a burst of
   close-together requests each push the next one further out without bound.
3. **Insertion is in time order**, after the entry the walk stopped at, and
   **everything behind the new entry is dropped**. Appending and then pruning
   the tail is not the same thing.
4. **The drain pops the head and preserves order.** "The newest due entry
   wins" depends on walking the queue in deadline order; a swap-remove that
   moves the tail to the front silently unordered the queue and armed the timer
   for whichever entry happened to be last.

The one thing deliberately **not** copied is the rewind: Nokia rewrites the
sample timeline when it shifts a change, and our timing comes from the graph
clock instead, so there is nothing to rewind.

Two bugs found this way and worth remembering, because both were silent: a
`size_t` loop counter that wrapped on an empty queue and wrote the entry
outside the array while reporting a queue length of zero, and the
swap-remove in point 4 above.

### The duck is the ladder's number, not a readback of the control

Reading the codec at the moment an absolute stream starts looks more honest --
*duck by what the hardware is actually doing rather than what we asked for*.
On this rig it agreed with the ladder to within a thousandth, so it bought
nothing, and its failure mode is severe: with overlapping absolute streams the
control is already at MAX when the second stream starts, so the readback
returns MAX, and that value is simultaneously the duck and the release target.
The release then "restores" unity with nothing pending, and the codec sticks at
full volume. Measured: five pooled clicks left the codec at 1.0000.

The ladder tracks the slider, which is the only thing allowed to move the codec
while no absolute stream is playing, so the ladder is the correct source. The
readback was removed rather than guarded, because a guard would have had to
decide, at attack time, which of two disagreeing numbers to believe.

### Where the per-input gain is applied

The volume-and-accumulate kernel takes one volume per input and applies it
while summing, so per-input gain and mixing are the same instruction stream --
there is no "sum then scale per input" available at any price. The mix handle
owns exactly one input and is the only node that can tell an absolute stream
from a normal one, so **the handle applies the gain**. The sink applies unity,
which for a single input per channel falls into the existing passthrough path
and costs nothing.

This corrects an earlier guard that silenced the handles and let the sink apply
the gain. That was only ever valid while the gain was one global number, for
which it did not matter who applied it.

### Identification: settled by measurement

`port_set_mix_info` delivers the global id of the peer **port**, not of the
peer node -- observed as `peer 73` and `peer 74` for a stream whose node id
was 72. The first implementation matched node ids and matched nothing.

The join needs no private headers, because **ports are globals**: the module's
existing `global_added` listener sees every `PW_TYPE_INTERFACE_Port` as it
appears, `pw_global_get_id()` returns exactly the number the mixer will later
report as a `peer_id`, and `pw_impl_port_get_node()` names the owner. So each
half records what it knows -- the stream records its port ids, the mixer's
publication records its port object -- and `stream_apply()` joins them.
Neither half has to know the other's schedule, and the usual order (ports
registered long before anything links) is not an assumption the code depends
on.

A stereo stream has two ports and the absolute bit must reach both; leaving one
at the duck gives a channel that does not match the other.

### Two facts the code had wrong

**`port_set_mix_info` runs on the main loop**, reached through port negotiation
(`do_negotiate -> pw_impl_port_set_param -> setup_mixer`), not from the data
loop. The first version marshalled to the main loop from inside it and crashed
in a re-entrant invoke onto the loop it was already running on.

**The module must take its own reference** on a port object the mixer hands
over. Storing the pointer the mixer owns and unrefing it later is a double
free that malloc does not notice until an unrelated teardown -- which is
exactly how it presented, and it is why a release path looked broken when the
state underneath it was corrupt.

### Duplicate arming: settled

Every mixer instance -- the sink and each mix handle -- observes the published
word, so one transition armed five codec requests. The fix is a shared
`observed` word in the state block and a compare-exchange: whoever consumes the
edge reports it, the others see none. Measured 5 requests before, 1 after.
Exclusivity comes from the state rather than from a role test, so nothing has
to know which node is the sink.

### Verified end to end

With the slider on rung 8 (codec 0.0631, -24.0 dB):

- absolute stream starts -> `absolute volume ON (duck 0.0631)`, one codec
  request armed, codec written to 1.0000 at the mixer's deadline
- absolute stream ends -> one release request armed, codec written back to
  0.0631 at its own deadline
- timer landing 0.06-0.16 ms late; the queue collapsed five requests into two
  writes on a burst

The duck is the rung amplitude, so `duck x MAX == slider level` holds by
construction, and the release restores the rung the slider is on *now* rather
than the one it was on when the request was queued.

### Two things the factory's multiplicity breaks

The core instantiates the mixer factory more than once, and that has two
consequences neither of which is visible from either side on its own.

**One peer id, several port objects.** More than one instance can be told
about the same peer, each with its own port and therefore its own port data.
Deduplicating the module's peer table by id keeps the first object and marks
that one, while a different instance is the one that actually mixes -- so the
absolute flag is set on an object nobody reads, and the click plays at the
rung even though the codec went to MAX. Every object for a peer id must be
marked, and the release must drop every one.

**The factory is instantiated on the output side too.** One mix handle is
created per input port of the *device* sink, and its peer is a device port,
not a stream. Those handles write the already-mixed sum, so ducking one ducks
everything -- including the absolute stream, which then follows the slider,
which is the symptom of getting this wrong. Ducking is therefore **opt-in**: a
port is only ducked if the module has marked it `MAEMO_PORT_STREAM`, which
happens for ports belonging to a tracked playback stream. An unrecognised peer
is left alone, so the failure mode for anything unexpected is "no duck" rather
than "wrong attenuation".

### Not settled

- **A stream destroyed while running leaves the absolute count non-zero.** The
  only release event is a node's state leaving `RUNNING`; a stream torn down
  without that transition never decrements the count, so the codec stays at
  MAX until some later stream's release corrects it. Identified, not yet
  fixed: `stream_destroy` must recompute the absolute state after removing the
  stream, the same way the state-change listener does.
- **The residual -1.9 dB during absolute playback is a ladder mismatch, and
  is accepted rather than fixed.** See *The duck is the ladder's number*
  below: the mechanism is right, the table is a fallback that does not
  describe this rig's codec. On hardware whose ladder describes the part the
  offset should vanish; if it does not, the ladder is what to look at.
- **`t_codec` and the main-loop tail must be re-measured on the device,** and
  so must the direction margins, which were fitted as a pair on a VM whose
  main-loop tail is hypervisor-mediated.

### Settled since this was first written

- **The music no longer rises during absolute playback.** It was three
  separate faults -- a stale published duck in the mix handles, a deadline
  computed on the control loop instead of in the cycle the gain landed on, and
  untuned direction margins -- each hidden behind the previous. The bed now
  holds to -1.9 dB for the whole notification and the release transient is a
  dip rather than a bump, which is the quiet side the margin rule aims for.
  `docs/timing.md` section 7 has the sequence.
- **The acoustic verification is done,** with the rig under `tests/rig/`. The
  lesson recorded there is that the instrument has to be finer than the thing
  measured: a 5 ms effect is invisible in 25 ms windows, and the analysis was
  coarser than the tuning for most of the investigation.

## The transient write

A route volume write costs the session manager far more than it looks like it
should. Measured on an idle stack, forty writes, wireplumber's own CPU from
`/proc/<pid>/schedstat`:

```
normal                                    16.3 ms of cpu per route write
param change not announced at the source   3.0 ms
```

So about 13 ms of the 16 is downstream of the fact that a volume change is
modelled as a device param change. It is not one thing. `on_volume_changed`
bumps the Route param serial, wireplumber re-enumerates the param over the
protocol and caches it, and *that* is what emits `device-params-changed`, which
then runs `device/store-or-restore-routes`, `default-nodes/rescan-trigger`,
`linking/rescan-trigger` and our own routing hook, followed by the whole
`rescan-for-linking` cascade. Removing the three upstream Lua hooks recovers
only 3.3 ms of the 13.3; the rest is the re-enumeration, the re-serialisation
and broadcast to every client holding a device proxy, and the audio adapter
recomputing hardware and software volumes. That is upstream behaviour, not
ours: every tick of a slider pays it, and on the target it will cost more.

The consequence for us is not the CPU, it is the queue. The release write
arrives at a session manager still busy with the cascade the raise started,
which is where the multi-millisecond tail on the hardware half comes from.

### Why the listener cannot fix it

A listener could tell that a Route change was volume-only -- wireplumber holds
the old pod and the new one when it refreshes its cache, and comparing index,
selected, available and name is cheap. What it cannot tell is the thing that
matters. A transient write and a real one are identical in content: the codec
holds 0.35 either way, the same route is selected, the same availability. The
only difference is that one will be undone in a hundred milliseconds and the
other is what the user wants remembered. That is a fact about the writer's
intent and it appears nowhere in the data, so a content-based rule either
ignores all volume changes -- which stops the store persisting the slider, and
the store is the per-device persistence we adopt from -- or ignores none.

The cost argument is secondary but points the same way: filtering in Lua
recovers a quarter of the cascade, because by the time a listener can classify
the change the expensive part has already happened. A listener can decline to
react to an announcement. It cannot unring it.

### The flag

So the statement comes from the sender, and `set_param` already has a `flags`
argument that is marshalled to the process owning the device and is unused for
Route and Profile. `SPA_DEVICE_SET_PARAM_FLAG_TRANSIENT` means: apply this, but
it is not news -- do not bump the param serial, do not announce a change.

It is a contract, not a hint. Nothing downstream learns of the value, including
caches: verified on the rig, a transient write of 0.10 moved the control from
63 to 50 while the device still enumerated 0.899987, the last announced value.
So the caller asserts that the change will be undone, which is exactly what an
absolute pair is. An implementation that does not know the flag announces as
before, which is always correct and merely pays.

Two statements stay separate. `SPA_PARAM_ROUTE_save` is upstream and means *do
not remember this*; the flag means *do not announce this*. In this module the
only writer that answers no to the first is the absolute pair, and both halves
of it are transients, so one answer serves both at that call site -- but they
are different questions and the protocol keeps them apart.

### What is preserved

The slider keeps going through the route with `save=true` and no flag, so the
store keeps seeing only genuine levels and per-device restore keeps working.
After a pair the control holds byte-identical values to before it, which is
also what wireplumber still believes it holds, so no divergence is left behind.
The mixer remains the timing authority: the write happens from the module's
main loop at the deadline the mixer handed, and bypassing the announcement does
not change who decides when.

The race is unchanged and small: a wireplumber re-apply inside the window
overwrites the top rung mid-notification, and the route-change invalidation
that already drops `hw_written` covers it.

### The ladder is not a tuning table

Two different things got conflated on the way here, and the distinction is
worth keeping straight. The ladder is a policy statement: the positions the
volume is allowed to take, which is why droid4 has thirteen rungs for media
and ten for a call, and why the top rung is a chosen acoustic level rather
than the codec's ceiling. Nokia's `mixer_tuning` is a device statement: sparse
`value:mB` pairs for one control, interpolated at load, hardware taking the
nearest rung and software absorbing the residual.

We do not need the second, because we never bypass ACP's mapping: the slider
and both halves of the absolute pair go through the route, and ACP turns the
ladder's centi-dB into a control value as it always has. For the record, the
Bluetooth parameter tables in Nokia's tree are not ALSA values either --
`module-bluetooth-device.c` maps the volume onto the headset's own 16-step
gain and sends it over D-Bus for HSP, and does nothing at all for A2DP -- and
only the ALSA sink in that series sets `PA_SINK_SYNC_VOLUME`, which is what
"synchronisable" meant there: a hardware element that can be aimed at a buffer
position. A device with no control to name is not a missing case, it is a
correct statement about that device.

The same flag decides what the deadline is aimed at. An output that advertises
`x-maemo.volume.sync` has a hardware stage whose distance is on the graph
clock, an ALSA codec register, so its level change is queued and aimed at the
samples that need it, with the margins protecting the duck on one side and the
release on the other. An output that does not advertise it, a Bluetooth sink
whose level is an AVRCP command, keeps the queue and the margins but takes the
deadline as now, because the transaction latency is a D-Bus call, an
over-the-air round trip and whatever the headset decides, and no observable
says when it was applied. What serves such an output in practice is the other
path, the software ladder applied to the node gain against the graph clock,
which is sample accurate and needs no queue at all; the codec path only runs
for an output that has a control to write.

## What the node declares, and which nodes exist

The node answers every param it declares. The factory declares `Format`
readable and writable, so WirePlumber enumerates it for the param cache as
part of registration, and a refusal there is not a warning in a log, it ends
the registration transition with an error: no announcement to the rest of the
policy, no session item, no link to the relay, and the dummy driver left as
the mixer's only neighbour with every stream aimed at it playing into that.
Whether the cache pass lands before the node is registered is a matter of
timing, which is the whole of why this looked intermittent.

`Format` is answered with the concrete geometry once a sink rate is known and
with an empty enumeration before that, never with `-ENOTSUP` and never with a
guess. `EnumFormat` keeps the rate choice range so the peer has something to
pick. The sample format goes in as an `Id`, not an `Int`, because that is what
a raw format pod carries and what `spa_format_audio_raw_parse` reads; an `Int`
parses as 0 for the format field and the node then reports `UNKNOWN` in
`pw-top`. The port answers its own planar float from the start rather than
refusing until the node has negotiated.

There is one output per audio card, and a card gets a mixer node when the
routing policy announces it. The node comes into being as soon as the card
grows a sink node rather than a fraction of a second later, so on a handset
the built-in mixer exists during startup. A mixer node is an `Audio/Sink`, so
a node per card is a candidate per card in the default-sink election, and a
card with nothing playable behind it, HDMI with no cable, gets no node at all.
The node carries the `device.id` of its card so the policy matches a mixer to
a sink with the same test it uses for sinks, and the shared volume state is
registered under the node name before the node is built, because the mixer
resolves its state while the node is being created and falls back to the
module's own if nothing is registered yet.

The codec request is armed per output and delivered to the output it was armed
for. The control written and the deadline met were already per output; an arm
that is not per output lets one card's mixer raise another card's control.

The routing script moves links it finds in the wrong place. A stream that
asked for a target while the relay was being built is wired straight into the
hardware and never asks again, so the metadata redirect alone leaves it there.
When the relay reports itself active the script takes those links down, clears
the peer flags so the linking policy considers the pair free, and asks for a
rescan.
