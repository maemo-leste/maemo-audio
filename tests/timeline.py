#!/usr/bin/env python3
"""Merge the absolute-volume instrumentation into one timeline.

Four separate observers write to the log, on two threads, and the defect
this is for only shows up when they are read together:

  stream_info_changed  the module learns a stream started or stopped
  input_gain           the mixer applied a software gain, in the cycle
                       whose samples it is mixing right now
  DIAG arm / codec_request
                       the instant the codec change was aimed at
  write_route_volume   the instant the codec actually moved

The two derived windows are the whole point. A sample mixed at wall time
T is heard at T + clock.delay, because that is how much audio is already
in front of the write pointer. So

  unducked at the speakers   [duck lifted + delay, duck applied + delay]
  codec at MAX               [write -> 1.0, write -> below 1.0]

Their overlap is the bump: music at full software level while the codec
is at maximum. A negative overlap on the release side is the duck held
past the codec's drop, which is a dip -- the far less audible mistake,
and the reason the margins are allowed to be a few milliseconds wide.
"""
import re
import sys
from datetime import datetime

RATE = 48000.0

ev, delay_ms = [], None

for line in open(sys.argv[1], errors="replace"):
    m = re.search(r"\[(\d\d:\d\d:\d\d\.\d+)\]", line)
    if not m:
        continue
    ms = datetime.strptime(m.group(1), "%H:%M:%S.%f").timestamp() * 1000.0

    if "DIAG arm:" in line:
        m2 = re.search(r"DIAG arm: (\w+) clock=(\d+)", line)
        if m2:
            delay_ms = int(m2.group(2)) / RATE * 1000.0
            ev.append((ms, "arm", f"{m2.group(1)} aimed +{delay_ms:.1f} ms "
                                  f"ahead (clock.delay {m2.group(2)} frames)"))
    elif "armed for" in line:
        m2 = re.search(r"codec (\w+) -> ([\d.]+) armed for \+([\d.]+) ms \(queue (\d+)\)", line)
        if m2:
            ev.append((ms, "queue", f"{m2.group(1)} -> {float(m2.group(2)):.4f} "
                                    f"due +{m2.group(3)} ms, queue {m2.group(4)}"))
    elif "codec: route" in line:
        m2 = re.search(r"codec: route \d+ dev \d+ -> ([\d.]+) \(([-\d.]+) dB\)", line)
        if m2:
            ev.append((ms, "hw", f"codec -> {float(m2.group(1)):.4f} ({m2.group(2)} dB)"))
    elif "gain:" in line and "input_gain" in line:
        m2 = re.search(r"gain: port (\d+).*-> ([\d.]+) \(word abs=(\d) duck=([\d.]+)\)", line)
        if m2:
            ev.append((ms, "sw", f"port {m2.group(1)} gain -> {float(m2.group(2)):.4f} "
                                 f"(word abs={m2.group(3)} duck={float(m2.group(4)):.4f})"))
    elif "stream_info_changed" in line:
        m2 = re.search(r"stream (\S+) (\w+)", line)
        if m2 and m2.group(2) in ("running", "idle", "paused"):
            ev.append((ms, "node", f"{m2.group(1)} {m2.group(2)}"))

if not ev:
    sys.exit("nothing recognised in that log")
ev.sort()
t0 = ev[0][0]

print(f"{'t(ms)':>9}  {'':5}  event")
for ms, kind, text in ev:
    print(f"{ms - t0:9.1f}  {kind:5}  {text}")

if delay_ms is None:
    sys.exit("\nno clock.delay seen; cannot project samples onto the speakers")

# Collapse the per-port lines: every port of every instance flips in the
# same cycle, and one transition is one event.
def edges(pairs, want):
    # Seeded at the state a capture starts in: no absolute stream running,
    # so the codec is at the slider and nothing is ducked. Capture from
    # before the first click for that to hold.
    out, last = [], False
    for ms, v in pairs:
        if last is not None and v == want and v != last:
            out.append(ms)
        last = v
    return out

# The word's abs flag, not the gain value: the absolute stream's own port
# is always at unity and is interleaved with the bed's ducked lines, so a
# gain-derived state flickers on every port that is not being ducked.
sw = [(ms - t0, "abs=1" in t)
      for ms, k, t in ev if k == "sw"]
hw = [(ms - t0, float(t.split("-> ")[1].split(" ")[0]) >= 0.999)
      for ms, k, t in ev if k == "hw"]

lift = edges(sw, False)          # abs dropped: the bed is no longer ducked
hold = edges(sw, True)           # abs rose: the bed is ducked from this cycle
up   = edges(hw, True)           # codec reached maximum
down = edges(hw, False)          # codec left maximum

def dedupe(times, within=5.0):
    out = []
    for t in times:
        if not out or t - out[-1] > within:
            out.append(t)
    return out

# The codec's maximum intervals, from the writes that opened and closed
# them. Pairing them beats scanning for "the last time it went up", which
# with several clicks in flight picks an interval that has already closed.
maxes = []
for u in up:
    d = next((x for x in down if x > u), None)
    maxes.append((u, d if d is not None else float("inf")))

lift, hold = dedupe(lift), dedupe(hold)

print(f"\nsamples reach the speakers clock.delay = {delay_ms:.1f} ms after they are mixed\n")
print(f"{'unducked heard':>15} {'until':>8} | {'MAX from':>9} {'MAX to':>9} | verdict")
for lf in lift:
    hs = lf + delay_ms
    he = next((h for h in hold if h > lf), float("inf")) + delay_ms
    win = next(((u, d) for u, d in maxes if u <= hs < d), None)
    if win is None:
        print(f"{hs:15.1f} {he:8.1f} | {'-':>9} {'-':>9} | codec not at MAX "
              f"when this audio is heard")
        continue
    over_b = min(he, win[1])
    over_a = max(hs, win[0])
    if win[1] == float("inf"):
        print(f"{hs:15.1f} {he:8.1f} | {win[0]:9.1f} {'-':>9} | MAX still open "
              f"when the capture ended; nothing to compare")
        continue
    if over_b > over_a:
        verdict = f"BUMP: {over_b - over_a:.1f} ms of unducked audio at MAX"
    else:
        verdict = f"clean: codec drops {win[1] - hs:.1f} ms before the unducked audio arrives"
    print(f"{hs:15.1f} {he:8.1f} | {win[0]:9.1f} {win[1]:9.1f} | {verdict}")
