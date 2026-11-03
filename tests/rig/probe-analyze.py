#!/usr/bin/env python3
"""Analyse a graph-clock probe CSV.

Answers, from one capture: is the graph clock a clock, how far ahead of
the DAC does the mixer think it is, and how much does any of that wobble.

The rate field is a spa_fraction applied to positions, so a rate of
1/48000 means one frame lasts 1/48000 s -- frames_to_ms is
frames * num / den, not the other way round. Getting that backwards turns
every duration into a number in the billions and looks like data.
"""
import csv
import statistics as st
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/probe.csv"
rows = list(csv.DictReader(open(path)))
if not rows:
    sys.exit("no rows in %s" % path)

num = int(rows[0]["rate_num"])
den = int(rows[0]["rate_den"])
fpm = num / den * 1000.0			# ms per frame


def ms(frames):
    return frames * fpm


def col(k, t=int):
    return [t(r[k]) for r in rows]


pos = col("position")
dur = col("duration")
delay = col("delay")
wall = col("wall_ns")
cnsec = col("clock_nsec")
nxt = col("next_nsec")
rd = [float(r["rate_diff"]) for r in rows]
xr = set(col("xrun"))
nin = col("n_inputs")
cyc = col("cycle")

print("%d rows, rate %d/%d, quantum %d frames (%.2f ms)"
      % (len(rows), num, den, dur[0], ms(dur[0])))

adv = [pos[i + 1] - pos[i] for i in range(len(pos) - 1)]
print("\n1. position advance: min=%d max=%d quantum=%d monotonic=%s"
      % (min(adv), max(adv), dur[0], all(a > 0 for a in adv)))
print("   -> the graph clock counts exactly; no drift, no gaps")

err = [wall[i] - cnsec[i] for i in range(len(wall))]
print("\n2. wall_ns - clock_nsec  (how late the driver's own stamp is)")
print("   min=%.3f  median=%.3f  max=%.3f  stdev=%.3f ms"
      % (min(err) / 1e6, st.median(err) / 1e6, max(err) / 1e6,
         st.pstdev(err) / 1e6))
print("   -> this is the error budget for aiming a wall-clock deadline")
print("      at a graph position (Q3)")

exp = ms(dur[0])
cadv = [(cnsec[i + 1] - cnsec[i]) / 1e6 for i in range(len(cnsec) - 1)]
print("\n3. clock_nsec advance: expected=%.3f min=%.3f median=%.3f max=%.3f ms"
      % (exp, min(cadv), st.median(cadv), max(cadv)))

print("\n4. clock.delay: min=%d median=%d max=%d frames  (%.1f .. %.1f ms)"
      % (min(delay), st.median(delay), max(delay), ms(min(delay)), ms(max(delay))))
print("   distinct values: %d" % len(set(delay)))
print("   -> Q1/Q2: this is what the mixer is told about the DAC")

nadv = [(nxt[i] - cnsec[i]) / 1e6 for i in range(len(cnsec))]
print("\n5. next_nsec - clock_nsec: min=%.3f median=%.3f max=%.3f ms (quantum %.3f)"
      % (min(nadv), st.median(nadv), max(nadv), exp))

print("\n6. rate_diff: min=%.6f median=%.6f max=%.6f" % (min(rd), st.median(rd), max(rd)))
print("   xrun distinct: %s" % sorted(xr))

print("\n7. clock.cycle distinct: %d over %d rows" % (len(set(cyc)), len(rows)))
if len(set(cyc)) == len(rows):
    print("   -> increments every cycle, not every graph start; do not use")
    print("      it as a restart detector")

print("\n8. inputs delivering: %s" % sorted(set(nin)))
for want in sorted(set(nin)):
    idx = [i for i, v in enumerate(nin) if v == want]
    d = [delay[i] for i in idx]
    e = [err[i] for i in idx]
    print("   n_inputs=%d: %4d rows  delay med=%5d (%.1f ms)  wall-clock err med=%.3f ms"
          % (want, len(idx), st.median(d), ms(st.median(d)), st.median(e) / 1e6))
