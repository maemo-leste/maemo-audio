#!/usr/bin/env python3
"""Analysis for t-codec2-run.sh: beep and codec step emitted together.

The capture shows the codec step first (it acts on the converter now) and
the beep later (it was queued behind the whole playback buffer). The gap
between them is clock.delay - t_codec, so

    t_codec = clock.delay - spacing

and if the codec applies its gain instantly the spacing is exactly
clock.delay.
"""
import array, csv, math, statistics as st, re, sys

Q = int(sys.argv[1]) if len(sys.argv) > 1 else 4096
TONE, BEEP = 2400.0, 1000.0

idx = list(csv.DictReader(open('/tmp/cap.raw.idx')))
a = array.array('h')
a.frombytes(open('/tmp/cap.raw', 'rb').read())
N = len(a)
if N == 0:
    sys.exit("empty capture")


def wall_of(frame):
    for r in idx:
        si, nf = int(r['sample_index']), int(r['frames'])
        if si <= frame < si + nf:
            return int(r['wall_ns']) + (frame - si) * 10**9 // 48000
    return None


def band(freq, win=192, stride=48):
    cs = [math.cos(2 * math.pi * freq * n / 48000) for n in range(win)]
    sn = [math.sin(2 * math.pi * freq * n / 48000) for n in range(win)]
    return [math.hypot(sum(a[n + k] * cs[k] for k in range(win)),
                       sum(a[n + k] * sn[k] for k in range(win)))
            for n in range(0, N - win, stride)], stride


# --- codec step: largest sustained drop in the tone energy ---
e_t, s_t = band(TONE)
k = 25
# the edges of the capture are not usable: the first block is where the
# recorder primes and the last is a partial, and an earlier version of
# this detector reported a "codec step" at frame 0 three runs in a row
lo, hi = 20 * k, len(e_t) - 20 * k
best = (0.0, None)
for i in range(max(k, lo), min(len(e_t) - k, hi)):
    d = st.mean(e_t[i - k:i]) - st.mean(e_t[i:i + k])
    if d > best[0]:
        best = (d, i)
if best[1] is None:
    sys.exit("no codec step found")
drop_i = best[1]
drop_frame = drop_i * s_t
print("codec step: tone %.0f -> %.0f (%.1f dB) at frame %d"
      % (st.mean(e_t[drop_i - k:drop_i]), st.mean(e_t[drop_i:drop_i + k]),
         20 * math.log10(max(st.mean(e_t[drop_i:drop_i + k]), 1)
                          / max(st.mean(e_t[drop_i - k:drop_i]), 1)), drop_frame))

# --- beep: spikes in the burst energy, with a real margin over the floor ---
e_b, s_b = band(BEEP)
floor = st.median(e_b)
thr = floor * 6.0
# NOTE: store frames and compare in frames. Comparing a block index to a
# frame count silently suppressed every beep after the first, which is
# what made the reference look absent for several runs.
beeps = []
for i in range(1, len(e_b) - 1):
    f = i * s_b
    if e_b[i] > thr and e_b[i - 1] <= thr and (not beeps or f - beeps[-1] > 4800):
        beeps.append(f)
print("beep floor %.0f max %.0f -> %d detected at frames %s"
      % (floor, max(e_b), len(beeps), beeps[:6]))
if not beeps:
    sys.exit("no beep detected -- the injection did not reach the speaker")

# a 1 kHz spike within 100 ms of the step edge is the step's own
# transient, not the reference beep; the real beep is expected about
# clock.delay later, so anything nearer than that is not it
after = [b for b in beeps if b > drop_frame + 4800]
if not after:
    sys.exit("no beep after the codec step; cannot measure the gap")
beep_frame = after[0]

Wc = int(re.search(r't_before_ns=(\d+)', open('/tmp/step.txt').read()).group(1))
inj = sorted(int(r['wall_ns']) for r in csv.DictReader(open('/tmp/inject.csv')))
Wb = min((w for w in inj if w >= Wc - 50 * 10**6), default=None)
if Wb is None:
    sys.exit("no injection logged within 50 ms of the codec write")

emit = (Wb - Wc) / 1e6
capt = (wall_of(beep_frame) - wall_of(drop_frame)) / 1e6
rows = [r for r in csv.DictReader(open('/tmp/alsaprobe.csv'))
        if r['stream'] == '0' and int(r['clock_delay']) > 0]
cd = st.median(int(r['clock_delay']) for r in rows) if rows else None
if cd is None:
    sys.exit("no playback clock.delay samples")

print("\nemitted apart:  %+.3f ms  (beep relative to codec write)" % emit)
print("heard apart:    %+.3f ms  (beep after the codec step)" % capt)
print("clock.delay:    %.3f ms  (%d frames)" % (cd / 48.0, cd))
# spacing_heard = spacing_emit + clock.delay - t_codec, so the emission
# offset has to come back out; an earlier version left it in and reported
# a large negative latency purely because the write landed a few tens of
# milliseconds before the beep it was aimed at
t = emit + cd / 48.0 - capt
# validation: with t_codec near zero the heard spacing must come out at
# about emit_offset + clock.delay. If it does not, the pairing is wrong
# and printing a number would be worse than printing nothing
predict = emit + cd / 48.0
if abs(capt - predict) > 40.0:
    sys.exit("heard spacing %.1f ms is %.1f ms from the predicted %.1f ms;"
             " the beep and the step are not paired -- refusing to report"
             % (capt, capt - predict, predict))
print("\nt_codec = emit_offset + clock.delay - heard_spacing")
print("        = %+.3f + %.3f - %.3f = %+.3f ms  (%+.0f frames)"
      % (emit, cd / 48.0, capt, t, t * 48))
