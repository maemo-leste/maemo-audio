#!/usr/bin/env python3
"""Look for a bump or a dip where a software gain step and a hardware
codec step were supposed to land on the same sample.

The product is what the speaker emits: total = software * hardware. If
the two transitions coincide the product never moves. If the hardware
arrives early the product rises for a while (a bump); if it arrives late
it falls (a dip). The depth says how far out of agreement they were and
the width says for how long.

Everything here is a difference within one stream, which is the class of
measurement that survives a stall in the reader: whatever the recorder
does between the two edges affects both edges the same way. The absolute
start time is deliberately not reported, because pw-record's priming is
opaque and anchoring on it has already been wrong once.
"""
import array, math, statistics as st, sys, wave

path = sys.argv[1] if len(sys.argv) > 1 else '/tmp/step.wav'
BLOCK = 48			# 1 ms

w = wave.open(path)
rate = w.getframerate()
a = array.array('h')
a.frombytes(w.readframes(w.getnframes()))
if w.getnchannels() > 1:
    a = a[0::w.getnchannels()]
if len(a) < 2 * rate:
    sys.exit("capture too short (%.2f s)" % (len(a) / rate))

sum2 = [sum(x * x for x in a[i:i + BLOCK]) for i in range(0, len(a) - BLOCK, BLOCK)]
env = [20 * math.log10(max(math.sqrt(v / BLOCK), 1.0)) for v in sum2]

# ignore the first and last tenth: the recorder primes and drops, and an
# earlier detector reported one of those edges as the event under test
lo, hi = len(env) // 10, len(env) - len(env) // 10
core = env[lo:hi]
flat = st.median(core)
print("capture %.2f s, flat level %.1f dBFS, %d ms blocks" %
      (len(a) / rate, flat, BLOCK * 1000 // rate))

# an excursion is a run of blocks away from the flat level by more than
# the tolerance; the flat level itself is the median, so a step that
# landed correctly simply does not appear
TOL = 1.5
runs, cur = [], None
for i in range(lo, hi):
    if abs(env[i] - flat) > TOL:
        if cur is None:
            cur = [i, i, max(env[lo:i], default=flat), min(env[lo:i], default=flat)]
        cur[1] = i
        cur[2] = max(cur[2], env[i])
        cur[3] = min(cur[3], env[i])
    else:
        if cur is not None and cur[1] - cur[0] >= 2:	# >= 2 ms, not a blip
            runs.append(tuple(cur))
        cur = None
if cur is not None and cur[1] - cur[0] >= 2:
    runs.append(tuple(cur))

if not runs:
    print("\nno excursion larger than %.1f dB: the two steps held the product flat"
          % TOL)
    sys.exit(0)

print("\n%d excursion(s):" % len(runs))
for s, e, mx, mn in runs:
    dur = e - s + 1
    bump, dip = mx - flat, flat - mn
    kind = "BUMP (hardware early)" if bump > dip else "DIP (hardware late)"
    depth = max(bump, dip)
    signed = bump if bump > dip else -dip
    print("  at %6.0f ms, %4d ms wide, %+5.1f dB  -> %s" %
          (s * BLOCK * 1000 // rate, dur, signed, kind))
    print("      peak %.1f dBFS, trough %.1f dBFS (flat %.1f)" % (mx, mn, flat))
