# Nokia queue vs ours

Out-of-tree harness. Feeds an identical event script to

* `nokia_queue.c` — a literal transcription of `pa_sink_volume_change_push()`
  / `_apply()` / `_flush()` from Nokia's patch 0081, and
* our own `maemo_vol_engine` / `maemo_hw_queue`,

on a shared virtual clock, and compares the sequence of hardware writes
each one issues. No audio, no mixer, no PipeWire: this is the scheduling
law on its own.

    make && ./equiv        # or: make run

Substitutions in the Nokia side, and nothing else: `pa_cvolume` becomes an
int ladder step (Nokia compares `pa_cvolume_avg()`, a single monotone
number, so a ladder index carries the same ordering), `pa_rtclock` becomes
a `now` argument, and `pa_usec_t` becomes nanoseconds. Rewind is absent
because PipeWire 1.6.9 has no rewind protocol, so there is no behaviour of
ours to compare against it.

Both sides get the same latency, extra delay and margin, so a difference is
the algorithm and not the tuning. Two modes run: **grid off** (no measured
period grid, so our `snap_to_grid` degenerates to a plain `+margin` on
lifts — the mode directly comparable to Nokia) and **grid on** (a synthetic
512-frame period at 48 kHz, i.e. what the module publishes on the VM).

## What it shows

**The level sequence is identical in every scenario.** Zero STEP
disagreements: whenever either implementation moves the codec, it moves it
to the same rung. Everything that differs is *when*, or *how many* writes
survive.

Three genuine divergences remain, in order of how much they matter:

**1. A release is left where it was named; Nokia shifts it early by the
margin.** This is deliberate and is the whole subject of the comment on
`snap_to_grid()`: biasing a release earlier separates the codec's fall from
the instant the restored software gain reaches the DAC, and the result is a
6-12 dB dip at the end of every notification. Nokia could get away with it
because PulseAudio's software volume and its hardware write were in the same
process at the same clock; here the two edges are produced by different
parties. Shows up as `TIME`, ~9-14 ms in these scenarios.

**2. In a fast burst, Nokia never lifts the codec at all — and neither do we
without the grid.** Four clicks 8 ms apart:

    grid off   ours   [t+33 step 2] [t+65 step 2]              <- no lift
               nokia  [t+24 step 2] [t+40 step 2] [t+56 step 2] [t+72 step 2]
    grid on    ours   [t+32 step 4] [t+33 step 2] [t+43 step 4] ... (8 writes)
               nokia  [t+24 step 2] [t+40 step 2] [t+56 step 2] [t+72 step 2]

A release that arrives before its own lift's deadline is computed to land
*earlier* than that lift, so it is inserted in front of it and Nokia's
"drop everything behind the new entry" deletes the lift. The click then
plays at slider level. Our queue does the same thing when the grid is off,
because the `>=` supersede rule has the same effect once the release is
shifted ahead of the lift. With the period grid in force the lift snaps to a
period boundary ahead of the release and survives, which is why the grid is
load-bearing for fast clicks rather than a refinement: **fast clicks being
improper is inherited from Nokia's law, not a regression we introduced.**

**3. Nokia collapses a slider staircase to its last level; we do not.**
Three raises and a drop at 15 ms spacing produce one write in Nokia and two
in ours. Nokia's insert position comes from the tail walk, so a new entry
can be placed ahead of older ones and delete them; ours only drops entries
whose deadline is at or after the new one, which keeps the levels that
samples already in flight were mixed against. Arguably ours is the intended
behaviour — that is the stated reason the schedule is a queue at all — but
it is a difference, and it means ours writes the codec more often during a
slider drag.

## Reading the verdicts

`MATCH` identical writes at the same instants. `TIME` same levels, instants
differ by more than a tick. `STEP` different levels — none currently.
`COUNT` different number of writes, i.e. a different amount of coalescing.
