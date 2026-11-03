#!/bin/sh
# Listening test for the codec-change aim distance on a backend that reports
# no hardware distance (Bluetooth: clock.delay == 0).
#
#   tests/bt-offset.sh 20      -> raise issued 20 ms after the duck
#
# Restarts the stack with MAEMO_TEST_OFFSET_MS, brings the headset up, plays
# a bed and fires one click, then prints what the mixer actually armed with.
# The wpctl default written here is persistent, so it is cleaned afterwards.
set -u

MS="${1:-0}"
MAC=64:68:76:19:CE:16
BED="${BED:-/tmp/bed.wav}"
CLICK="${CLICK:-/usr/share/sounds/ui-key_press.wav}"
STORE="$HOME/.local/state/wireplumber/default-nodes"

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

MAEMO_TEST_OFFSET_MS="$MS" maemo-audio-run >/dev/null 2>&1
timeout 15 bluetoothctl connect "$MAC" >/dev/null 2>&1

BT=""
for _i in $(seq 1 30); do
	BT=$(timeout 6 wpctl status 2>/dev/null | grep EDIFIER | grep -oE '[0-9]+\.' | tr -d '.' | head -1)
	[ -n "$BT" ] && break
	sleep 1
done
if [ -z "$BT" ]; then
	echo "headset did not appear" >&2
	exit 1
fi
timeout 6 wpctl set-default "$BT" >/dev/null 2>&1
sleep 2

pw-play --target maemo_mixer "$BED" >/dev/null 2>&1 &
BEDPID=$!
sleep 1
setsid /home/user/maemo-testrig/mis-click -f "$CLICK" >/tmp/mis.log 2>&1 </dev/null &
sleep 1.2
MP=$(pgrep -x mis-click | head -1)
[ -n "$MP" ] && kill -USR1 "$MP"
sleep 1.5
pkill -x mis-click 2>/dev/null
kill "$BEDPID" 2>/dev/null

sed -i '/^default\.configured\.audio\.sink/d' "$STORE" 2>/dev/null

echo "=== asked for ${MS} ms; mixer armed with ==="
grep "DIAG arm" /var/log/maemo-audio/pipewire.log | tail -3 | sed 's/spa.maemo.mixer | //'
echo "=== codec writes ==="
grep "codec: route" /var/log/maemo-audio/pipewire.log | tail -2 | sed 's/ default      | //' | cut -c1-100
