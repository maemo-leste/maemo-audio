#!/usr/bin/env python3
"""t_codec at an ordinary, audible volume, with the search anchored on a
prediction instead of an argmax.

Every earlier failure of this measurement was the same mistake: scan the
whole capture for the biggest drop and hope it is the codec step. It found
the capture's own start edge, a beep's transient, and once a transition
two seconds from the write. So instead: the beep is located first, the
step is predicted to sit a known distance before it, and the search is
confined to a narrow window around that prediction. If nothing is there,
that is a failure to report, not a number.

Emitted together at wall time W:
    beep  audible at W + clock.delay      (queued behind the buffer)
    codec audible at W + t_codec          (acts on the converter now)
so the step precedes its beep by about clock.delay - t_codec, and

    t_codec = emit_offset + clock.delay - heard_spacing
"""
import array, csv, math, statistics as st, re, sys

Q = int(sys.argv[1]) if len(sys.argv) > 1 else 4096
TONE, BEEP = 2400.0, 1000.0
GUESS_MS = 4.0			# t_codec from the earlier low-volume runs

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


rows = [r for r in csv.DictReader(open('/tmp/alsaprobe.csv'))
        if r['stream'] == '0' and int(r['clock_delay']) > 0]
if not rows:
    sys.exit("no playback clock.delay samples")
cd = st.median(int(r['clock_delay']) for r in rows)
cd_ms = cd / 48.0

Wc = int(re.search(r't_before_ns=(\d+)', open('/tmp/step.txt').read()).group(1))
inj = sorted(int(r['wall_ns']) for r in csv.DictReader(open('/tmp/inject.csv')))
Wb = min((w for w in inj if w >= Wc - 50 * 10**6), default=None)
if Wb is None:
    sys.exit("no injection logged within 50 ms of the codec write")
emit = (Wb - Wc) / 1e6

# --- beeps ---
e_b, s_b = band(BEEP)
floor = st.median(e_b)
thr = floor * 6.0
beeps = []
for i in range(1, len(e_b) - 1):
    f = i * s_b
    if e_b[i] > thr and e_b[i - 1] <= thr and (not beeps or f - beeps[-1] > 4800):
        beeps.append(f)
after = [b for b in beeps if b > 4800]
if not after:
    sys.exit("no beeps detected")

# --- predict where the step must be, relative to each candidate beep ---
# step precedes its beep by (emit + clock.delay - t_codec)
lead = emit + cd_ms - GUESS_MS
e_t, s_t = band(TONE)
k = 10

best = None
for b in after:
    want = b - int(lead * 48.0)
    lo = max(k, int((want - 40 * 48) / s_t))
    hi = min(len(e_t) - k, int((want + 40 * 48) / s_t))
    for i in range(lo, hi):
        d = st.mean(e_t[i - k:i]) - st.mean(e_t[i:i + k])
        if best is None or d > best[0]:
            best = (d, i * s_t, b)
if best is None:
    sys.exit("no candidate window available")
drop_frame, beep_frame = best[1], best[2]
capt = (wall_of(beep_frame) - wall_of(drop_frame)) / 1e6
t = emit + cd_ms - capt

print("clock.delay      %.3f ms (%d frames)" % (cd_ms, cd))
print("emitted apart    %+.3f ms" % emit)
print("predicted lead   %+.3f ms (step before its beep)" % lead)
print("heard apart      %+.3f ms" % capt)
print("beeps seen       %d" % len(beeps))

if abs(capt - lead) > 40.0:
    sys.exit("heard spacing %.1f ms is %.1f ms from the predicted %.1f ms"
             " -- refusing to report" % (capt, capt - lead, lead))
print("\nt_codec = %+.3f ms  (%+.0f frames)" % (t, t * 48))
