#!/usr/bin/env python3
"""Envelope of the capture, to answer one question: does the music bed move
when a notification plays?

Expectation from the invariant duck x MAX == slider level: the bed is flat
across the whole recording and only the click itself stands out. If the bed
rises for the duration of the click, the duck is not reaching the samples
that reach the DAC, however tidy the module's bookkeeping looks.

The bed is a 220 Hz tone and the click is broadband, so the click is found as
the loudest short excursion and the bed is measured as the median of the
windows around it -- median, not mean, so the click cannot drag it.
"""
import array, csv, math, statistics as st, sys

WIN_MS = 25
SR = 48000

try:
    idx = list(csv.DictReader(open('/tmp/cap.raw.idx')))
except FileNotFoundError:
    sys.exit("no /tmp/cap.raw.idx -- run duck-acoustic-run.sh")

a = array.array('h')
a.frombytes(open('/tmp/cap.raw', 'rb').read())
N = len(a)
if N == 0:
    sys.exit("empty capture")

win = SR * WIN_MS // 1000
rows = []
for i in range(0, N - win, win):
    seg = a[i:i + win]
    r = (sum(x * x for x in seg) / win) ** 0.5
    # Normalised: dBFS is relative to full scale, and forgetting the divisor
    # once made a bed at -43 dBFS print as +47 "dBFS" and look saturated.
    rows.append((i / SR, 20 * math.log10(r / 32768.0) if r > 1 else -120.0))

peak = max(r[1] for r in rows)
floor = st.median(r[1] for r in rows)
print("samples %d (%.2f s)  windows %d ms  peak %.1f dBFS  median %.1f dBFS"
      % (N, N / SR, WIN_MS, peak, floor))
if peak > -6:
    print("WARNING: capture near full scale -- levels are not trustworthy")

# The click is the loudest excursion; take its centre as the anchor.
ci = max(range(len(rows)), key=lambda k: rows[k][1])
ct = rows[ci][0]
print("click window at %.2f s (%.1f dBFS)" % (ct, rows[ci][1]))

def band(lo, hi):
    v = [db for t, db in rows if lo <= t < hi]
    return (st.median(v), len(v)) if v else (None, 0)

before = band(0.5, max(0.6, ct - 0.2))
during = band(ct + 0.15, ct + 1.0)
after = band(ct + 1.6, ct + 3.0)
for name, (m, n) in (("before", before), ("during", during), ("after", after)):
    print("  %-6s %s" % (name, ("%7.1f dBFS  (%d windows)" % (m, n))
                         if m is not None else "no data"))
if before[0] is not None and during[0] is not None:
    print("\nbed shift during the notification: %+.1f dB  (expected 0.0)"
          % (during[0] - before[0]))

print()
for t, db in rows:
    bar = '#' * int(max(0, (db + 60)) / 1.2)
    mark = '  <-- click' if abs(t - ct) < WIN_MS / 1000 else ''
    print("%6.2f %7.1f %s%s" % (t, db, bar, mark))
