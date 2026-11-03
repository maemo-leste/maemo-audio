#!/usr/bin/env bash
# Does the software gain step and the codec write land on the same sample?
#
# Both halves now go through the machinery that will really be used:
#
#   software  the mixer switches its applied gain on the cycle whose
#             graph position is MAEMO_SWSTEP_AT -- exact to the sample,
#             no scheduling involved
#   hardware  on that same cycle the mixer computes
#             now + clock.delay/rate from its own clock, hands the value
#             and that absolute deadline to the module across the loop
#             boundary, and the module arms a timer for it and writes the
#             route
#
# Nothing here pokes ALSA from outside: the route is the only channel that
# survives, and an external write gets re-asserted away within seconds.
#
# The product is what the speaker emits: total = software * hardware.
# Start at hardware 0.30 and software 1.00; end at hardware 1.00 and
# software 0.30. If the two land together the product never moves. If the
# hardware arrives early the product rises (a bump); late, it falls (a
# dip). At a 0.30 ratio the excursion is +10.5 dB or -10.5 dB.
set -u
export XDG_RUNTIME_DIR=/run/user/$(id -u)
cd /home/user/maemo-testrig

STEP_AT=${STEP_AT:-192000}	# 4 s: the mixer counts from its own start
HW_FROM=${HW_FROM:-0.30}	# route volume at the start
SW_TO=${SW_TO:-0.30}		# software gain from the step on
HW_TO=${HW_TO:-1.00}		# route volume armed at the step

pkill -x pw-play; pkill -x pw-record
pkill -x pipewire-pulse; pkill -x wireplumber; pkill -x pipewire
sleep 2
rm -f /tmp/swstep.csv /tmp/alsaprobe.csv /tmp/step.wav

export MAEMO_SWSTEP_AT=$STEP_AT MAEMO_SWSTEP_TO=$SW_TO MAEMO_HWSTEP_TO=$HW_TO
export MAEMO_SWSTEP_CSV=/tmp/swstep.csv
export MAEMO_TEST_ROUTE=$HW_FROM
export MAEMO_ALSA_PROBE=/tmp/alsaprobe.csv
MAEMO_RT=1 maemo-audio-run >/tmp/flat-start.log 2>&1
sleep 2
grep -E "software gain step armed|test: route forced|sink write" /var/log/maemo-audio/pipewire.log | tail -3

# the mic at its default of 28 is +30 dB and pins the recording at full
# scale, which makes every level difference a lie; 8 is 0 dB
./codec-step hw:0 8 29 >/dev/null 2>&1
sleep 1
echo "# codec at start: $(amixer -c 0 cget numid=6 | grep -o 'values=[0-9]*' | tail -1) of 30 (want 23)"

(pw-play --target maemo_mixer /tmp/tone20.wav >/dev/null 2>&1 &)
sleep 1
(pw-record --rate 48000 --channels 1 /tmp/step.wav >/dev/null 2>&1 &)

# the mixer arms the codec write itself; the script only has to be out of
# the way and then look at what arrived
for i in $(seq 1 200); do
	[ -s /tmp/swstep.csv ] && [ "$(wc -l < /tmp/swstep.csv)" -ge 2 ] && break
	sleep 0.05
done
if [ ! -s /tmp/swstep.csv ]; then
	echo "the software step never fired"; pkill -x pw-record; pkill -x pw-play; exit 1
fi
cat /tmp/swstep.csv
sleep 2
echo "# codec after the step: $(amixer -c 0 cget numid=6 | grep -o 'values=[0-9]*' | tail -1) of 30 (want 30)"
grep -E "codec write armed|codec: route|late" /var/log/maemo-audio/pipewire.log | tail -4

sleep 2
pkill -x pw-record; pkill -x pw-play
sleep 1
python3 /home/user/maemo-testrig/step-analyze.py /tmp/step.wav
