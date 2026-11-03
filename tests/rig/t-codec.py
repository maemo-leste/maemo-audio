#!/usr/bin/env python3
"""Measure the codec's apply latency: how long after the register write
returns until the change is audible.

Why it is hard, and what that means for the method. The playback sink and
the capture source are separate graph drivers, each with its own position
counter starting at its own start, so a playback position and a capture
position cannot be subtracted -- an earlier attempt did exactly that and
produced a loop latency that was smaller than the playback path alone.
The fix is to keep every subtraction inside one domain and let the
capture path's unknown constant cancel:

    emit a burst at wall time Wb, step the codec at wall time Wc
    observe them in the capture at wall times Sb and Sc
    the capture path delay D is common to both, so

        (Sc - Sb) = (Wc - Wb) + t_codec
        t_codec   = (Sc - Sb) - (Wc - Wb)

The reference is the injected burst, whose emission instant the mixer
logs. The step is a codec write, whose instant codec-step prints. Both
are converted to capture-domain wall time through the tap's own index,
which stamps every block it writes with the wall clock.

Frequencies are measured from the capture, not assumed: an earlier run
assumed a 440 Hz tone and the tone was 2400 Hz, and every detector in
that run was looking at noise.
"""
import array, csv, math, statistics as st, re, sys

TONE_HZ = float(sys.argv[1]) if len(sys.argv) > 1 else 2400.0
BURST_HZ = float(sys.argv[2]) if len(sys.argv) > 2 else 1000.0

idx = list(csv.DictReader(open('/tmp/cap.raw.idx')))
a = array.array('h')
a.frombytes(open('/tmp/cap.raw', 'rb').read())
N = len(a)
if N == 0:
    sys.exit("empty capture")


def wall_of(frame):
    """Capture-domain wall time of a sample index, from the tap index."""
    for r in idx:
        si, nf = int(r['sample_index']), int(r['frames'])
        if si <= frame < si + nf:
            return int(r['wall_ns']) + (frame - si) * 10**9 // 48000
    return None


def band_energy(freq, win=192, stride=96):
    cs = [math.cos(2 * math.pi * freq * n / 48000) for n in range(win)]
    sn = [math.sin(2 * math.pi * freq * n / 48000) for n in range(win)]
    out = []
    for n in range(0, N - win, stride):
        seg = a[n:n + win]
        out.append(math.hypot(sum(seg[k] * cs[k] for k in range(win)),
                              sum(seg[k] * sn[k] for k in range(win))))
    return out, stride


# --- the codec step, from the tone's energy: largest sustained drop ---
e_tone, stride = band_energy(TONE_HZ)
k = 25
best = (0.0, None)
for i in range(k, len(e_tone) - k):
    d = st.mean(e_tone[i - k:i]) - st.mean(e_tone[i:i + k])
    if d > best[0]:
        best = (d, i)
if best[1] is None:
    sys.exit("no step found in the tone energy")
step_i = best[1]
step_frame = step_i * stride
before = st.mean(e_tone[step_i - k:step_i])
after = st.mean(e_tone[step_i:step_i + k])
print("tone %.0f Hz energy %.0f -> %.0f (%.1f dB) at frame %d = %.2f s"
      % (TONE_HZ, before, after, 20 * math.log10(max(after, 1) / max(before, 1)),
         step_frame, step_frame / 48000))

# --- the bursts, from the burst frequency: spikes above its own floor ---
e_burst, bstride = band_energy(BURST_HZ)
med = st.median(e_burst)
thr = med * 4
bursts = []
for i in range(1, len(e_burst) - 1):
    if e_burst[i] > thr and e_burst[i - 1] <= thr:
        if not bursts or i - bursts[-1] > 150:
            bursts.append(i * bstride)
print("burst %.0f Hz: floor %.0f, threshold %.0f -> %d detected"
      % (BURST_HZ, med, thr, len(bursts)))
sp = [bursts[i + 1] - bursts[i] for i in range(len(bursts) - 1)]
if sp:
    print("burst spacing in capture:", sp[:8])

pre = [b for b in bursts if b < step_frame]
if not pre:
    sys.exit("no burst detected before the step")

Wc = int(re.search(r't_before_ns=(\d+)', open('/tmp/step.txt').read()).group(1))
inj = sorted(int(r['wall_ns']) for r in csv.DictReader(open('/tmp/inject.csv')))
# the burst nearest before the step in the capture pairs with the injection
# nearest before the write; both sequences are strictly ordered and share
# one period, so rank matching is enough
Wb = max(w for w in inj if w < Wc)
Sb, Sc = wall_of(pre[-1]), wall_of(step_frame)
if Sb is None or Sc is None:
    sys.exit("event outside the indexed capture window")

emit = (Wc - Wb) / 1e6
capt = (Sc - Sb) / 1e6
print("\nlast burst before step: emitted %d, heard %d" % (Wb, Sb))
print("codec write:              written %d, heard %d" % (Wc, Sc))
print("\nspacing at emission: %.3f ms" % emit)
print("spacing at capture:    %.3f ms" % capt)
print("\nt_codec = %.3f ms  (%.0f frames)" % (capt - emit, (capt - emit) * 48))
