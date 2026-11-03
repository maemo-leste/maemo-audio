#!/usr/bin/env python3
"""Timeline check that needs no speaker.

The artifact a mis-timed codec produces is an interval during which the
codec and the software gain disagree, and both edges are already in the
log -- the gain edge with the instant the mixer says it becomes audible,
the codec write with the instant the module actually wrote. So the two
durations can be subtracted instead of recorded.

The software gain and the codec multiply, so the two edges bracket the
same interval and disagreeing about their order is what is heard:

  attack   (soft 1 -> duck, codec low -> top)
           codec first  -> (top, top)    too loud
           soft first   -> (low, low)    too quiet
  release  (soft duck -> 1, codec top -> low)
           soft first   -> (1, 1)        too loud
           codec first  -> (low, low)    too quiet

So the benign direction is the same on both edges -- arrive late and the
error is a dip, arrive early and it is a jump -- and the numbers below
are how far each codec move is late, which is what the design accepts:

  attack   margin = t(codec lift)    - audible(gain duck)
  release  margin = t(codec release) - audible(gain restore)

Positive is late, which is the safe direction. The gain line's t= is the
moment the edge was SEEN, not when it becomes audible; the audible
instant is that plus the printed offset, and mixing the two up makes a
one-period skew look like a 122 ms one.

Usage: edge-check.py [logfile] [--since HH:MM:SS]
"""
import re
import sys

LOG = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("--") \
    else "/var/log/maemo-audio/pipewire.log"
since = None
if "--since" in sys.argv:
    since = sys.argv[sys.argv.index("--since") + 1]

gain_re = re.compile(
    r"\[(\d\d:\d\d:\d\d\.\d+)\].*GAIN edge: port (\d+) ([\d.]+) -> ([\d.]+)"
    r"( \((\w+)\))? at off=(\d+) fr.*audible in ([+-]\d+) ms.*\(t=(\d+) us")
wrote_re = re.compile(
    r"\[(\d\d:\d\d:\d\d\.\d+)\].*WROTE route idx=\d+ amp=([\d.]+) t=(\d+)")


def hms(s):
    h, m, rest = s.split(":")
    return int(h) * 3600 + int(m) * 60 + float(rest)


events = []
for line in open(LOG, errors="ignore"):
    if since and not re.search(r"\[\d\d:\d\d:\d\d", line):
        continue
    m = gain_re.search(line)
    if m:
        if since and hms(m.group(1)) < hms(since):
            continue
        old, new = float(m.group(3)), float(m.group(4))
        kind = "adopt" if m.group(6) else ("duck" if new < old else
                                           "restore" if new > old else "none")
        # t= is the instant the edge was seen; the offset is how far
        # ahead of the DAC that sample sits on the graph clock.
        seen = int(m.group(9))
        audible = seen + int(m.group(8)) * 1000
        events.append((audible, "gain", kind, old, new, m.group(1)))
        continue
    m = wrote_re.search(line)
    if m:
        if since and hms(m.group(1)) < hms(since):
            continue
        t_us = int(m.group(3))
        kind = "lift" if float(m.group(2)) > 0.999 else "release"
        events.append((t_us, "codec", kind, float(m.group(2)), None,
                       m.group(1)))
        # The module's t= is already the instant it wrote, in us.

events.sort(key=lambda e: e[0])

print(f"{'t(wall)':<16}{'event':<9}{'kind':<8}{'detail'}")
for t, what, kind, a, b, wall in events:
    if what == "gain":
        d = f"{a:.4f} -> {b:.4f}" + (f" [{kind}]" if kind == "adopt" else "")
    else:
        d = f"amp={a:.4f}"
    print(f"{wall:<16}{what:<9}{kind:<8}{d}")

# Pair each codec move with the last gain edge of the kind it must follow.
print("\nhow late each codec move is, against the gain edge it must follow:")
bad = 0
for i, (t, what, kind, a, b, wall) in enumerate(events):
    if what != "codec":
        continue
    want, label = ("duck", "attack") if kind == "lift" \
        else ("restore", "release")
    prev = [e for e in events[:i] if e[1] == "gain" and e[2] == want]
    if not prev:
        print(f"  {wall} {label:<8} no prior {want} edge to pair with")
        continue
    margin = (t - prev[-1][0]) / 1000.0
    flag = "" if margin >= 0 else "   <== EARLY: this window is a jump"
    if margin < 0:
        bad += 1
    print(f"  {wall} {label:<8} {margin:+8.1f} ms late{flag}")

print(f"\n{bad} artifact window(s)")
sys.exit(1 if bad else 0)
