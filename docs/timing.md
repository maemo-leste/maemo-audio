# Timing: what has been measured, and what was wrong

Everything in this file was measured on the VM (USB Audio Device, 48 kHz,
quantum 4096 unless stated). It exists because several plausible-looking
timing numbers were derived, believed, and then found to be wrong -- and
the wrong ones were wrong in ways that no amount of re-reading the code
would have caught. Both the numbers and the failures are recorded, because
the failures are the part that is easy to repeat.

See also `DESIGN.md`: the model. This file is only about timing.

---

## 1. The rule that came out of all of it

```
codec write target position = current graph position + clock.delay
```

`clock.delay` read from the node every cycle. Nothing else enters the
arithmetic: no device constant, no summed latency, no quantum derived
from configuration.

The software half of a software/hardware pair needs no separate constant
either. A software gain change written in the cycle at position `P`
becomes audible at `P + clock.delay` -- the same distance, because it
rides the same samples. So a software step and a hardware step that are
*decided* together do not need to be *issued* together; the hardware write
goes out about `clock.delay/rate` later (154 ms at quantum 4096).

---

## 2. Measured and believed

### The mix-handle instances -- a correctness bug, found by accident

The core instantiates the mixer factory **once per link** for the mix-handle
role, so a process holds `1 + active links` instances of it. They are
distinguishable by what they are created with:

| | the sink the module made | a per-link mix handle |
|---|---|---|
| `node.name` | `maemo_mixer` | absent |
| `media.class` | `Audio/Sink` | absent |
| `maemo.sink.channels` | `2` | absent |
| `library.name` | absent | `audiomixer/libspa-audiomixer` |

Measured on a stereo stream: **five instances applied the node gain** -- four
handles and the sink. Any gain other than unity was therefore applied
`1 + links` times, so the volume depended on how many apps were playing.
Invisible forever at unity, which is why it survived this long. The step to
0.30 landed five times: 0.3^5, about -52 dB instead of -10.5 dB.

Two guards, both keyed on the marker the module passes:

- `node_gain(this, chan)` returns 1.0 when `chan == SPA_ID_INVALID`, which is
  the mix-handle role.
- the software step is only armed when `maemo.sink.channels` is present, so
  exactly one instance can request a codec write. (Gating `node_gain` alone
  is not enough: the step overrides the gain, and each handle had its own
  copy of the step state.)

After both: one instance applies the step, one codec write armed, one route
write. Before: five and four.

### The data -> control crossing, implemented and measured

```
mixer (data loop, the cycle carrying position P)
    apply the software gain to this cycle's samples
    deadline = now + clock.delay/rate      both from this node's own clock
    spa_loop_invoke(loop, cb, {deadline, value})    -- copied by value

module (control loop, whenever the invoke is picked up)
    arm a timer at that ABSOLUTE deadline -- never re-derived from "now"

module (timer)
    pw_impl_device_set_param(SPA_PARAM_Route, channelVolumes = value)
```

The deadline must be computed on the data loop and passed across, not
computed in the callback: `spa_loop_invoke` promises nothing about when the
control loop gets to it, and a deadline measured from the arrival would
inherit every millisecond of whatever else the control loop was doing.

**Why the mixer and not the module, stated plainly because it was violated
once.** The distance that matters is from the frame the software gain was
applied to, and only the cycle that applied it knows which frame that was.
A module that computes the same formula on the control loop is arithmetically
identical and physically wrong: it measures from when the module *heard* about
the stream, and the gap between hearing and applying is audio that plays at MAX
without the duck. The module decides *whether* a change is due, because it is
the only party guaranteed to observe the stream's state; the mixer decides
*when*, because it is the only party holding a graph position. Neither may
compute the other's fact.

This requires the mixer to be certain of a cycle in which to notice, which is
what `node.always-process` is for -- see *hold the graph up* in the commit for
`9d85cc4`. Detection granularity is then one quantum, 85 ms at quantum 4096,
and that costs nothing: the gain lands in the same cycle the deadline is taken
in, so a late detection shifts both halves together.

Measured landing error of the timer against the absolute deadline:
**0.097, 0.135, 0.204 ms** -- far better than the 4-9 ms main-loop tail,
because arming a timer well in advance is a different problem from hitting a
deadline from a timer armed now.

The route is the only channel that survives: an external `amixer` write is
re-asserted away within seconds by the same mechanism that makes the route
authoritative, so a test that sets the codec from outside silently tests
nothing. `MAEMO_TEST_ROUTE` sets the starting level through the route.

### `clock.delay` is frame-accurate, but only with htimestamp on

| | published `clock.delay` | driver `snd_pcm_avail_delay()` | error |
|---|---|---|---|
| htimestamp off | 6640 | 7392 | **-752 frames = -15.7 ms** |
| htimestamp on | 7359 | 7360 | **0 at the median** |

Upstream published a number systematically short by 15.7 ms. The
correction that closes the gap (`hw_extra`) sits inside
`if (state->htimestamp)` in `alsa-pcm.c`, and the config that enables
htimestamp had been deleted, so the correction was inert. Restored in
`cc8fd0a` as `66-alsa-htimestamp.conf`.

This is a *same-instant comparison of two reads from one driver*, so it is
immune to every perturbation listed in section 5.

### `clock.delay` is scheduler-load-bearing, not just information

In `alsa-pcm.c get_status()` one quantity does three jobs: flow control
(`delay > target + max_error` -> `-EAGAIN`), wakeup scheduling
(`update_time()` -> `next_time`), and our publication. Inflating it makes
the node believe it is ahead of itself, return `-EAGAIN` forever and push
its own wakeup further out every cycle: permanent silence. **`clock.delay`
cannot be repurposed as a channel for anything else.**

### The node's quantum is not the graph's quantum

Asked for graph quantum 8192, the playback ALSA node ran at `duration`
4096, clamped by its own `threshold`. The capture node ran at 8192. Any
constant computed from `default.clock.quantum` would have been wrong by
85 ms while looking entirely reasonable. **Read the quantum from the
node.**

### Path jitter is under 1 ms, with zero drift

Injected bursts at known graph positions, heard through a tap that shares
the clock: identical detected onset across 11 bursts, spread 0 as
measured. The detector's resolution is 48 frames (1 ms), so the honest
statement is **jitter below 1 ms, zero drift over 10 s** -- not
sample-exact.

Two ALSA drivers (playback sink, capture source) are separate graph
drivers with independent position counters, and they showed **zero
relative drift**: locked to the same master clock.

### `t_codec` is about 3.5 ms, and is therefore not used

The codec's apply latency -- register write to audible effect:

| step | run | `t_codec` |
|---|---|---|
| value 12 -> 3 (-40 dB) | 1 | +4.08 ms |
| value 12 -> 3 | 2 | +3.55 ms |
| value 30 -> 21 (-13.5 dB) | 1 | +2.00 ms |
| value 30 -> 21 | 2 | +3.91 ms |

Independent of step size, so a fixed write-and-latch delay, not a ramp.

**It is deliberately not compensated for.** The main loop cannot hit an
instant more precisely than about 4-7 ms (below), so subtracting a 3.5 ms
constant buys nothing. This is how the "no hardcoded device values"
requirement is satisfied: not by storing `t_codec` per device, but by it
being below the precision we can exercise.

### The codec write itself costs about 1 ms

`snd_ctl_elem_write` measured 0.76-1.77 ms, sd ~250 us, over a dozen
runs. Also the 1 kHz spike that accompanies it is a real detector hazard.
**This is the hard argument against issuing the write from the RT data
loop**, however attractive the timing precision there.

### Main-loop timer landing error

`/home/user/maemo-testrig/loop-latency.c`. Median is excellent; the tail
is not, and **RT priority does not fix it**:

| class | period | median | 99th | max |
|---|---|---|---|---|
| SCHED_OTHER | 20 ms | +0.23 ms | +1.50 ms | +3.54 ms |
| SCHED_FIFO 88 | 20 ms | +0.15 ms | +1.94 ms | **+8.79 ms** |
| SCHED_FIFO 88 | 1 ms | +0.09 ms | +2.48 ms | +4.04 ms |
| SCHED_FIFO 88, loaded | 20 ms | +0.06 ms | +3.82 ms | +4.03 ms |

Priority improving the median but not the tail points at timer *delivery*
in a VM (hypervisor-mediated), not preemption. Consequence: **the VM
cannot answer "can the main loop hit an instant to +/-1 ms"** -- neither
presence nor absence of the capability is demonstrable here. Re-run the
probe on the device with RTKit working before relying on any such
precision.

### The route's amplitude -> dB mapping is correct, so the ladder needs no correction

Writing `channelVolumes = 0.0794` (intended -22.0 dB) through the route puts
the control at `values=16` on a `min=0,max=30` / `-45..0 dB` scale, which is
-45 + 45*(16/30) = **-21.0 dB**. One step of quantisation, no scale error.

This matters because the alternative would have invalidated the whole flat
model: if ALSA had mapped the amplitude linearly in dB, the codec at rung 9
would have sat near -41 dB, the jump to MAX would have been ~41 dB against a
22 dB duck, and the music would have risen by the difference during every
notification. It does not: the mapping is what the ladder assumes, so no
per-device correction factor is wanted here.

### The codec write lands several ms after we ask, and the tail is another process

`tools/pollctl` (rig) reads the codec control back and says when it
actually changed, so the gap between the module's `write_route_volume`
line and the hardware holding the new value became measurable. Twenty
clicks, USB development card:

```
poll 200 us   raise  9.05 .. 10.80 ms   release  9.07 .. 25.85 ms   9 of 21 above 10 ms
poll 1 ms     raise  9.01 .. 11.53 ms   release  9.28 .. 27.91 ms   9 of 21 above 10 ms
```

Identical at both poll rates, so the observer is not the cause. The tail
is on the release only; raises are consistent to 2 ms.

**The first explanation given here -- that this was `snd-usb-audio`
serialising mixer access on a full-speed bus -- was wrong, and the
emulated AC97 disproved it in one run.** Same measurement, no USB
anywhere, no packet bus to contend with:

```
AC97 emulated   raise  4.36 ..  8.63 ms   release  4.44 .. 15.72 ms   4 of 16 above 10 ms
```

The floor halved, which is the USB part going away. The tail did not.
Silence also shrinks it -- with no music playing the same card gives
`raise 5.43 .. 6.55`, `release 5.53 .. 13.30`, one of ten above 10 ms --
so whatever loads the path is loaded by the audio, not by the control.

The cause came out of segmenting the path with three timestamps, in the
core, in ACP and at the ALSA call:

```
our timer -> our write call                 0.041 ms median
our set_param call (returns immediately)    0.013 ms
core -> ACP applies it                      0.735 ms median, 6.377 ms worst
ACP -> the snd_mixer call itself            0.038 ms
snd_mixer -> control visibly changed        ~2.5 ms (the codec's own write)
```

**The ALSA device does not live in pipewire.** It is created by the
monitor in the wireplumber process, so `pw_impl_device_set_param` is a
proxy call, and the ACP handler that touches the mixer element runs on
wireplumber's main loop. That hop is the whole deferral, and it explains
the asymmetry nothing else could: the release is armed at the same
instant the absolute stream goes idle, which is when wireplumber is
busiest with the teardown and state propagation for that very stream.
Raises land on a quiet loop and releases land on a loaded one.

Consequences, stated as constraints rather than tuning:

A margin cannot cover a delay whose duration depends on another
process's workload. Five milliseconds happens to sit near the median on
this rig, which is a coincidence and not a design.

The only levers that address it are an actuator that stays in pipewire
-- the sink has no mixer handle today, and the element identity ACP
knows is not published anywhere, so either would be a change to code we
do not own -- or accepting the hop and keeping the release biased toward
a dip, which is what the margins do now.

**Repeat the segmentation on the target before fitting anything.** The
hop is framework, not hardware, so it will be there; what may differ is
how busy wireplumber is when the release comes due, and on a slower CPU
it may be busier. `pollctl` and the three timestamps are standalone and
work against any codec control.

### The direction margins: 5 ms each, and the sweep that dismissed them was invalid

**Current value: `raise 5000, lower 5000` microseconds.**

An earlier section of this document said the margins were unjustified and
reset them to 0, on the strength of a four-setting sweep (`0/0`, `0/15`,
`15/0`, `10/10`) in which the artefact did not move. **That sweep was
measuring a different system.** At the time the deadline was being computed
on the control loop from the module's own `clock_gettime`, which put the
codec write about 175 ms away from the frame the software gain landed on.
Shifting one edge by 10-15 ms against an error of 175 ms is not a test of
the margin; it is noise against a much larger mistake, and the sweep's
negative result was really a measurement of how insensitive the artefact was
to a small perturbation of a badly aimed write.

Once the mixer was restored as the deadline authority -- the instant taken
from the cycle whose samples the gain lands on, see *The data -> control
crossing* above -- the margins began to have audible effects, which is what
a correctly aimed write should do. With both at 0 a bump is audible on one
edge or the other depending on the click; at `5/5` single clicks during music
are clean. The fine-grained capture (5 ms windows, which matters: a 5 ms
effect is invisible in a 25 ms window, and the analysis was coarser than the
thing being tuned for most of this) shows why the sign is what Nokia's rule
says it should be: at `5/5` the release transient is a **dip**, not a bump,
so the residual sits on the quiet side where the rule intends it.

What is *not* established, and should not be over-read: **both edges were
moved together**, so neither 5 is individually attributed. `5/0` and `0/5`
were tried by ear and each left a bump on one edge, which is the evidence
for needing both, but the numbers themselves are a pair fitted on a VM whose
main-loop tail is 4-9 ms and hypervisor-mediated. On the device, where RTKit
works and the codec is a different part, the same pair may be wrong in the
other direction. **Re-measure there before defending these values.**

The general rule survives, and is the reason this section exists rather than
a bare constant: establish which edge a bump is on, change one knob, record
the measurement. What went wrong the first time was not the method but the
system under test, and a negative result is only as good as the state of the
thing being perturbed.

### Route volume restore is wanted, and was being fought

`device.restore-routes` is the mechanism that gives the codec its
per-port level, which is what the module *adopts* at startup to derive the
slider (`reread_target()`, the `app_target == NULL` path). The shipped
`50-maemo-profile-policy.conf` says explicitly it is left at its default
of true. A VM-only `64-no-route-restore.conf` was overriding that and has
been removed. **Do not disable route restore.**

### Bluetooth has no hardware write to time

BlueZ declares its distance on the port `SPA_PARAM_Latency` and sets
`clock->delay = 0`; a volume change to a BT sink is a software change
with ordinary `clock.delay` semantics. So the uniform abstraction is not
"schedule a hardware write on every backend" but **"each backend declares
how far the DAC is; aim at that distance."** There is nothing for an ALSA
patch to unify.

---

## 3. Retracted -- do not re-derive these

| claim | why it is wrong |
|---|---|
| "absolute loop latency is 184 ms, verified" | subtracted a playback position from a capture position. The two ALSA nodes are **separate graph drivers** with independent position counters, so the arithmetic is meaningless. |
| "summing two nodes' `clock.delay` overcounts by exactly one quantum" | four data points and a coincidence. At quantum 1024 the "measured loop" came out *smaller than the playback path alone*, which is impossible, and proved the anchor bad. |
| "wall-clock stamp error: median 0.40 ms, max 4.27 ms" | that max is our own mixer probe dumping 1 MB of CSV from the data loop (measured cost 4.2 ms). It measured the observer. |
| "target 30419 is near the top" | the applet target scale is `PA_VOLUME_NORM` = 65536. 30419 is **46 %**. The module divides by 65536, correctly. |
| "the sink volume 0.46 mirrors the slider" | unproven. The applet out of a call writes only the target; it has no path to the sink volume. |
| "the tone is 440 Hz" | `tone20.wav` is **2400 Hz**. Several detector runs were looking at noise. |
| "no links are active -- found the blocker" | the graph was dumped after the player was killed. Paused links with no stream are correct. |

**The pattern:** every number that fell over was a *maximum* or an
*absolute timestamp*. Every number that survived is a *difference within
one clock domain* or a *bound*. A stall or an offset is common mode to a
difference and invisible to it; it contaminates a maximum completely.

---

## 4. Instrumentation (env-gated; the rig is committed under `tests/rig/`)

`/home/user/maemo-testrig` is where the tools are built and run from and is
not under version control; `tests/rig/` in this repository is a copy so the
instruments a recorded measurement was made with survive the machine. See
`tests/rig/README.md` for building them and the two traps that cost time.

The graph clock probe and the burst injector lived in
`mixer/maemo-probe.{c,h}` with hooks in `mixer-process.c`, `mixer-impl.h` and
`mixer-factory.c`. They are removed, the numbers they produced are recorded
below and stand on their own, and re-introducing them is a small job if a
measurement is ever needed again. What remains in the mixer is the software
step, `mixer/maemo-swstep.{c,h}`, which is a production path and not
instrumentation.

| env | what it does |
|---|---|
| `MAEMO_PROBE=1`, `MAEMO_PROBE_DUMP=<csv>` | removed, was per-cycle `spa_io_clock` fields. **Banned for timing work** -- dumps in bulk, 4.2 ms per dump. |
| `MAEMO_INJECT=<frames>`, `MAEMO_INJECT_CSV=<csv>`, `MAEMO_INJECT_LEN=<ms>`, `MAEMO_INJECT_AMP=<0..1>` | removed, was injects a 1 kHz burst at a known graph position and logs `(wall_ns, position)`. Default 2 ms, amp 0.3. |
| `MAEMO_SWSTEP_AT=<pos>`, `MAEMO_SWSTEP_TO=<gain>`, `MAEMO_SWSTEP_CSV=<csv>` | software gain step anchored to a graph position, so the software half has no scheduling jitter. |

In the built ALSA tree `~/tmp/pipewire/spa/plugins/alsa/alsa-pcm.c`
(rebuild: `ninja -C build spa/plugins/alsa/libspa-alsa.so`):

| env | what it does |
|---|---|
| `MAEMO_ALSA_PROBE=<csv>` | per-period playback and capture clock/delay/avail rows. Cost ~1 us/period. |
| `MAEMO_CAP_DUMP=<raw>` | dumps capture frames with `(wall_ns, position, sample_index)` -- the tap that turned an inference into a measurement. Cost ~7 us/period. **Worth keeping.** |

Standalone tools in `/home/user/maemo-testrig/`:

- `codec-step.c` -- write an ALSA control at a logged instant. **Takes a
  percent, not a raw value**: value = max * percent / 100. `hw:0` not `0`.
- `loop-latency.c` -- main-loop timer landing error, with a load generator.
- `probe-cost.c` -- what the instrumentation costs the loop it measures.
- `t-codec2-run.sh` + `t-codec3.py` -- the `t_codec` measurement, one command.
- `flat-step-run.sh` -- the flat-volume bump/dip test (written, **not yet run**).
- `mis-click.c` -- resident MIS stand-in: primes one pooled sample and fires it
  on `SIGUSR1`, so a notification can be replayed without a desktop. Build it
  with `-lcanberra` explicitly; the makefile rule does not carry it.

`CA_PROP_CANBERRA_VOLUME` is a **bare dB number with no suffix**. libcanberra's
pulse driver parses it with `strtod` and rejects anything with trailing text:

```c
dvol = strtod(vol, &e);
if (errno != 0 || !e || *e) ret = CA_ERROR_INVALID;
```

So `"-5.0"` works and `"-5.0 dB"` or `"70%"` return `EINVAL` -- and they do it
inside libcanberra, before any Pulse request is sent, which is why the failure
looked like a server or sample-cache problem and produced nothing in the pulse
log at all. Bisecting it needed a 10-line client, because the error names
neither the property nor the reason.

`maemo-audio-run` now accepts `MAEMO_RT=1` (and `MAEMO_RT_PRIO`, default
88) to start the stack under `SCHED_FIFO`, because the VM skips
`libpipewire-module-rt` (`19-no-rtkit-vm.conf`) and would otherwise run
everything `SCHED_OTHER`, which is pessimistic versus hardware. It needs
`sudo -n env XDG_RUNTIME_DIR=...` because sudo strips the runtime dir.

---

## 5. Measurement hygiene, learned the hard way

0. **Check the recording level before trusting any level.** The mic's
   default capture volume is +30 dB and pins the file at full scale. A
   clipped capture makes a bump invisible and turns a modest gain reduction
   into a 34 dB cliff, because what is actually being watched is the
   recorder unclipping. Confirm the peak is well below full scale first.

1. **Never delete a file a running process has open.** The tap and injector
   hold their handles; `rm` sends the writes to a deleted inode and the
   probe silently produces nothing. Restart after deleting, or use new
   names.
2. **The tap's `wall_ns` is when the capture node *read* the block, not
   when sound arrived.** The capture ring holds ~6600 frames (138 ms) of
   unread data and that fill varies. Absolute acoustic times derived from
   it are off by an unmeasured amount -- this produced several impossible
   negatives. The fix is to make two events common mode: emit them
   together so the unknown cancels.
3. **`pw-record` cannot be a clock anchor.** Its priming and dropping are
   opaque; an attempt to anchor on it was off by 1152 frames.
4. **Validate pairing before believing a number.** Refuse to print when the
   observation is more than a tolerance from the prediction. Three runs
   produced impossible values before this discipline was added; after it,
   failures announced themselves instead of looking plausible.
5. **Detect the frequencies you actually have.** Measure the spectrum
   before writing a detector.
6. **A step detector that scans for the biggest drop will find the capture's
   own start edge.** Predict where the event must be and search a narrow
   window around it.
7. **The factory is instantiated more than once.** Anything opened eagerly
   by a factory instance that never processes will truncate a file opened
   by the one that does. Open lazily on first use.
8. **`clock.cycle` increments every cycle**, contradicting its header
   comment. Not a graph-restart detector.
9. **`pkill -f <pattern>` can kill your own shell** (the pattern matches the
   shell's command line). Use `pkill -x`.
10. **`sudo -n cp ... && ...` silently skips on sudo failure** while
    `;`-separated restarts still run. Verify installs with
    `strings <so> | grep -c <symbol>`.

---

## 6. Known-good ALSA control map (USB device, card 0)

| numid | control | range | notes |
|---|---|---|---|
| 6 | Speaker Playback Volume | 0..30 = -45..0 dB | the "codec". value v -> dB = -45 + 1.5v |
| 8 | Mic Capture Volume | -12..+30 dB | at 28 by default; **clips the mic**, lower it for acoustic work |
| 3 | Mic Playback Switch | bool | sidetone; must be off or the loop is not acoustic |
| 9 | Auto Gain Control | bool | must be off |

Amplitude 0.30 corresponds to value 23 (percent 77).

The `pipewire` alsa-lib plugin exposes a virtual `Master Playback Volume`
on a 0..65536 scale; it is the **same volume as the sink node**, seen
through the plugin, not a second stage.

---

## 7. Open

- **Resolved: the music rose instead of holding level during absolute
  playback.** Three separate causes, each found by measurement after the
  previous one was removed, and none of them visible from the module's own
  bookkeeping, which was correct throughout: (a) mix handles applied the
  published duck to a stale copy of it, so the music jumped a rung louder --
  fixed by loading the published word unconditionally rather than only on the
  edge-winner path; (b) the deadline was computed on the control loop from
  `clock_gettime` instead of in the cycle the gain landed on, which left a
  175 ms misalignment and an 18.4 dB dip after the click -- fixed by restoring
  the mixer as the deadline authority, which in turn required the
  always-process hold so the release always gets a cycle; (c) the direction
  margins, now 5/5. Acoustically the bed now holds to **-1.9 dB for the whole
  notification**, and the release transient is a dip rather than a bump.

- **The residual -1.9 dB is a ladder mismatch and is accepted.** It is flat
  for the entire notification, so no margin can touch it. The ladder in use
  on this rig is the N900 internal-speaker fallback; this VM's codec is a USB
  device with 31 steps over -45..0 dB, so the ladder asks for -22.0 dB, the
  ALSA node rounds to control value 16, and value 16 is -21.0 dB. The codec
  is ~1 dB louder than the ladder claims and the duck over-removes by about
  that much. The alternative -- reading the control's dB range and step count
  from ALSA at startup to build the ladder from the hardware -- was considered
  and declined: the fallback is documented as "a sane fallback, not a claim
  about the hardware in hand", and on the N900 the ladder describes the part,
  so this should disappear by itself. **If it does not disappear on the
  device, that is a finding about the N900 ladder, not about this mechanism.**

- **The bump/dip test: first run invalidated by its own instrument.**
  `flat-step-run.sh` gave a clean, reproducible envelope in which the codec
  read back 23 immediately after being set and **30 by the time of the
  software step**. Something re-asserts the route volume within seconds, so
  the hardware half silently did nothing and the observed step was the
  software step alone. **Raw ALSA control writes do not survive** -- the
  ALSA node owns that control and re-applies it. That is a direct
  validation of the module's choice to write `SPA_PARAM_Route` instead of
  touching ALSA, and it means the only honest version of this test drives
  the module rather than an external `amixer`.
- **A `pw-cli set-param <dev> Route` write with `channelVolumes` also does
  not move the codec** (device 54, route index 1 `analog-output-speaker`,
  which carries no `channelVolumes` in `pw-dump`). Work out what the module
  does differently before building another test on this channel.
- **Software step measured -13.6 dB where -10.5 dB was predicted** (0.30
  ratio). Everything else in that envelope agreed to 0.1 dB, so this is
  either codec/mic non-linearity or the analyser's flat-level median.
- **Sink software volume lands at ~0.2 on restart** -- an unexplained
  second volume stage that multiplies with the codec. In the flat model the
  sink volume must be pinned at unity or the slider describes a level that
  is not the level. Unexplained: the applet has no path to it.
- **`spa_handle_clear` teardown crash** -- garbage `destroy` pointer during
  plugin handle teardown, new signature, possibly the triple factory
  instantiation. Teardown-only. The older `spa_node_port_set_io` teardown
  crash is pre-existing and not runtime-reachable.
- **`t_codec` and the main-loop tail must be re-measured on the device**,
  where RTKit works and the codec is a different part (expected smaller).
- **Analog/converter group delay is unobtainable on this device** -- the ALSA
  audio timestamp report is entirely empty (`valid=0, type=0, accuracy=0`).
  It measured as unresolvable inside the loop, so it costs nothing here, but
  it is not something to reason from.

---

## 8. State when this was written

Branch `pure-mixer`. The queue is Nokia's push semantics transcribed
(`3ff83fe`), the graph hold is back (`9d85cc4`), and the mixer arms the codec
from the cycle the duck lands on (`b3dfc8b`). Codec margins are **5/5 ms** in
`/etc/pipewire/pipewire.conf.d/60-maemo-mix.conf` -- still deployed config
only, so a rig rebuilt from the repo alone will not match a rig tuned in
place, which is now a known gap rather than a surprise.

The absolute-volume cycle works end to end and is now verified acoustically
rather than by the module's own log lines: raise to MAX, release back to the
rung, the bed holding to -1.9 dB for the duration, and a burst of pooled
clicks leaving the codec on the rung rather than stuck at MAX.

The measurement scaffolding that was left behind during the absolute-volume
work is gone: the `PROBE instance` walk of the mix list, the `PORTPROP` and
`mix info prop` property dumps, the `FLAG set` stamp in the process path, and
the dead `timing_logged` field. What remains of that instrumentation is at
debug level rather than deleted, because it is what makes a field log
readable once the level is raised: `gain: port`, `join: peer`,
`mixer published peer`, and the per-step codec arm line.

Still open as behaviour, and untested since the per-output rewrite: a stream
destroyed while it is running must take its absolute claim down with it, or
the codec sits at MAX until some other stream's release corrects it. The
port release path hands the module a NULL association, which is the right
shape for it, but the case has not been exercised end to end.

