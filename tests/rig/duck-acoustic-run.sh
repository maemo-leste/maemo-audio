#!/usr/bin/env bash
# Does the music hold level while a notification plays?
#
# The invariant is duck x MAX == slider level: the codec rises to MAX and the
# software duck removes exactly that much, so the music must be *unchanged*
# for the duration of the click and only the click itself should stand out.
# Everything measured so far was the module's own bookkeeping, which DESIGN.md
# already warns is not proof. This is the proof: one microphone, one capture,
# the music bed and the click heard together, so the capture path's own delay
# is common mode and cancels.
#
# Captured through the mixer's tap (MAEMO_CAP_DUMP), not pw-record: the tap
# shares the graph clock and writes (wall_ns, position, sample_index) beside
# the samples, which is what makes any timing claim from it meaningful.
#
# THE TAP NEVER STOPS BY ITSELF. It is enabled by an environment variable on
# the pipewire process, so the stack this script leaves running keeps writing
# capture frames -- about 2.3 MB a minute, indefinitely. It filled a
# filesystem to 98% while nobody was looking. The teardown at the end of this
# script is not ceremony: if it is skipped (a failed run, a killed shell),
# restart the stack with maemo-audio-run before walking away.
set -u
export XDG_RUNTIME_DIR=/run/user/$(id -u)
cd /home/user/maemo-testrig

pkill -x pw-play; pkill -x pw-record; pkill -x mis-click
pkill -x pipewire-pulse; pkill -x wireplumber; pkill -x pipewire
sleep 2
rm -f /tmp/cap.raw /tmp/cap.raw.idx

setsid env MAEMO_CAP_DUMP=/tmp/cap.raw PIPEWIRE_DEBUG=3 \
	pipewire >/var/log/maemo-audio/pipewire.log 2>&1 </dev/null &
sleep 5
setsid wireplumber >/var/log/maemo-audio/wireplumber.log 2>&1 </dev/null &
setsid pipewire-pulse >/var/log/maemo-audio/pipewire-pulse.log 2>&1 </dev/null &
sleep 8

# The bed sits 21 dB down at rung 9, so the default capture gain leaves it
# near the noise floor. Gain is raised for this measurement and the peak is
# checked afterwards rather than assumed.
MAEMO_MIC_GAIN=${MAEMO_MIC_GAIN:-18} ./calibrate-mic.sh
./slider -s 9 >/dev/null 2>&1
sleep 1

# The tap lives in the capture node; nothing runs it unless something reads a
# source, so a capture client has to be alive for the whole measurement. It
# writes nowhere -- the tap is what is being recorded.
(pw-record --rate 48000 --channels 1 /dev/null >/dev/null 2>&1 &)
sleep 1

# music bed first, so there is a level to compare against on both sides
(pw-play --target maemo_mixer /tmp/tone1k.wav >/dev/null 2>&1 &)
sleep 2

# The stack was just restarted, so any earlier mis-click has lost its
# connection; it is started here, after the server is up.
setsid ./mis-click -f /usr/share/sounds/ui-key_press.wav >/tmp/mis.log 2>&1 </dev/null &
echo $! > /tmp/mis.pid
sleep 2
MIS=$(cat /tmp/mis.pid)
if [ -n "$MIS" ] && kill -0 "$MIS" 2>/dev/null; then
	echo "# firing the notification at $(date +%H:%M:%S.%N)"
	kill -USR1 "$MIS"
else
	echo "# mis-click is not running; start it first:"
	echo "#   setsid /home/user/maemo-testrig/mis-click -f /usr/share/sounds/ui-key_press.wav &"
fi

sleep 3
pkill -x pw-play
sleep 1
python3 /home/user/maemo-testrig/duck-envelope.py

# Put the stack back without the tap. See the warning at the top: leaving
# this out is how the dump ran overnight.
pkill -x pipewire-pulse; pkill -x wireplumber; pkill -x pipewire
sleep 2
setsid maemo-audio-run >/dev/null 2>&1 </dev/null &
