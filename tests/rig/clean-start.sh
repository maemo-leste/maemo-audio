#!/bin/bash
# Bring the audio stack up in a known state before a measurement.
#
# The WirePlumber state cache is the part that has to go. It holds
# per-stream volumes and per-route volumes, and both are values this
# stack computes rather than values anyone configures:
#
#   stream-properties  a remembered stream volume is an attenuation
#                      applied upstream of the mixer, so a measurement
#                      reflects whatever the previous run left behind
#                      instead of what was just asked for. Absolute
#                      streams opt out of it per-node, but ordinary
#                      streams do not, and a test tool that passes
#                      --volume has its value overwritten.
#
#   default-routes     a remembered route volume is a codec level from
#                      an earlier session. The residual is slider
#                      divided by the codec level actually in force, so
#                      a restored value silently invalidates it.
#
# Restarting is not enough on its own: the cache is written on shutdown
# and read back on startup, so a plain restart reproduces the stale
# state perfectly.
set -u

WP_STATE="${XDG_STATE_HOME:-$HOME/.local/state}/wireplumber"
HARNESS="${MAEMO_HARNESS:-/usr/local/bin/maemo-audio-run}"
LOG=/var/log/maemo-audio/pipewire.log
SINK_NAME="${MAEMO_SINK_NAME:-maemo_mixer}"

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

pkill -x wireplumber 2>/dev/null
pkill -x pipewire 2>/dev/null
sleep 1

rm -f "$WP_STATE/stream-properties" "$WP_STATE/default-routes"

setsid nohup "$HARNESS" >/tmp/harness.out 2>&1 </dev/null &

# Readiness is the mixer node existing with the right name, which the
# module logs when it binds it. Read the log rather than asking the
# daemon: pw-dump and pw-cli block while the daemon is still coming up,
# which turns a slow start into a hung script.
: > "$LOG" 2>/dev/null || true
for _ in $(seq 1 60); do
	sleep 1
	if grep -aq "virtual sink created: $SINK_NAME" "$LOG" 2>/dev/null; then
		echo "clean: cache removed, '$SINK_NAME' up"
		exit 0
	fi
done

echo "WARNING: sink '$SINK_NAME' did not appear; check $LOG" >&2
exit 1
