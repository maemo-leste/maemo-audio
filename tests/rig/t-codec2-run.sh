#!/usr/bin/env bash
# Emit a beep and a codec step at the same instant, then measure how far
# apart they arrive.
#
# Both are heard through the same microphone and the same capture ring, so
# whatever delay that path adds is common to both and cancels -- which is
# the whole point of firing them together. Runs several seconds apart do
# not cancel, because the ring fill differs between the two events, and
# that is what made three earlier attempts come out with impossible
# negative latencies.
#
# Emitted together at wall time W:
#   the beep reaches the DAC at W + clock.delay  (it is queued behind the
#     whole playback buffer)
#   the codec change acts at W + t_codec        (it affects whatever the
#     converter is turning into sound right now)
# so the capture shows the codec step first and the beep clock.delay -
# t_codec later, and t_codec = clock.delay - spacing. If the codec applies
# its gain instantly, the spacing equals clock.delay exactly.
set -u
export XDG_RUNTIME_DIR=/run/user/$(id -u)
cd /home/user/maemo-testrig

PERIOD=96000			# 2 s between beeps: short wait, easy aiming
Q=4096

pkill -x pw-play; pkill -x pw-record
pkill -x pipewire-pulse; pkill -x wireplumber; pkill -x pipewire
sleep 2
rm -f /tmp/cap.raw /tmp/cap.raw.idx /tmp/alsaprobe.csv /tmp/inject.csv

setsid env MAEMO_INJECT=$PERIOD MAEMO_INJECT_LEN=20 MAEMO_INJECT_AMP=0.8 \
	MAEMO_INJECT_CSV=/tmp/inject.csv \
	MAEMO_ALSA_PROBE=/tmp/alsaprobe.csv \
	MAEMO_CAP_DUMP=/tmp/cap.raw \
	PIPEWIRE_DEBUG=3 pipewire >/var/log/maemo-audio/pipewire.log 2>&1 </dev/null &
sleep 5
setsid wireplumber >/var/log/maemo-audio/wireplumber.log 2>&1 </dev/null &
setsid pipewire-pulse >/var/log/maemo-audio/pipewire-pulse.log 2>&1 </dev/null &
sleep 8

./codec-step hw:0 6 100 >/dev/null 2>&1	# full up: an ordinary audible step, not a mute
wpctl set-volume @DEFAULT_AUDIO_SINK@ 1.0 >/dev/null 2>&1

# the tone is what the codec step is seen on; the beep is the clock
(pw-play --target maemo_mixer /tmp/tone20.wav >/dev/null 2>&1 &)
sleep 3
(pw-record --rate 48000 --channels 1 /dev/null >/dev/null 2>&1 &)
sleep 2

# aim the codec write at the next beep: the injector logs the wall time of
# every beep it emits, and the period is fixed, so the next one is due at
# the last one plus the period
for i in $(seq 1 40); do
	[ "$(wc -l < /tmp/inject.csv 2>/dev/null || echo 1)" -ge 2 ] && break
	sleep 0.25
done
DELAY=$(python3 - "$PERIOD" <<'PY'
import csv, sys, time
period = int(sys.argv[1])
rows = list(csv.DictReader(open('/tmp/inject.csv')))
last = int(rows[-1]['wall_ns'])
now = time.clock_gettime(time.CLOCK_MONOTONIC) * 1e9
due = last + period * 10**9 // 48000
ms = int((due - now) / 1e6)
while ms < 300:			# never fire in the past; wait a whole period
	ms += period * 1000 // 48000
print(ms)
PY
)
echo "# codec write aimed at the next beep, in ${DELAY} ms"
./codec-step hw:0 6 70 "$DELAY" | tee /tmp/step.txt

sleep 8
pkill -x pw-record; pkill -x pw-play
sleep 1
python3 /home/user/maemo-testrig/t-codec3.py "$Q"
