#!/bin/bash
# Set the loopback capture to a known, verified state.
#
# The measurement is acoustic -- speakers to microphone -- so the capture
# gain is part of the instrument and has to be fixed rather than left at
# whatever a previous run needed. Two failure modes this prevents:
#
#   saturation   a transient peaking at 0 dBFS reads flat across its own
#                attack, which looks exactly like "no overshoot". Worse,
#                it looks the same on a quiet run and a loud one, so
#                levels measured across runs are not comparable.
#
#   noise floor  a music bed a few dB above the noise reads flat too,
#                which looks exactly like "the transient made no
#                difference".
#
# Auto gain control must be off. It is a compressor with an unknown time
# constant, and an envelope measurement through it measures the AGC.
#
# The controls are addressed by numid because the names differ per codec:
# this one has 'Mic Capture Volume' and no plain 'Mic' at all, and a
# control that does not exist fails silently in a script like this one.
# Ask for the numbers before changing cards:
#
#   amixer -c "$CARD" controls
#
# The gain spans -12..+30 dB over its raw range. The default was picked
# by sweeping against the loudest expected sound -- the codec at full
# lift with a full-scale tone -- and taking the setting that leaves a
# dozen dB of headroom on it. Verify rather than trust it: the peak of a
# recording should not sit near full scale.
#
# Order matters. PipeWire's source volume and the ALSA capture volume are
# the same control, so the PipeWire side goes first and the ALSA gain is
# the last write, or PipeWire replaces it with whatever it thinks the
# source should be and the gain this script reports is not the gain in
# effect.
set -u

CARD="${MAEMO_LOOPBACK_CARD:-0}"
GAIN="${MAEMO_MIC_GAIN:-8}"
SRC="${MAEMO_MIC_NODE:-alsa_input.usb-GeneralPlus_USB_Audio_Device-00.mono-fallback}"

pactl set-source-mute "$SRC" 0 2>/dev/null
pactl set-source-volume "$SRC" 100% 2>/dev/null
pactl set-default-source "$SRC" 2>/dev/null

amixer -c "$CARD" cset numid=7 on >/dev/null 2>&1 || echo "mic: capture switch (numid 7) not set" >&2
amixer -c "$CARD" cset numid=9 off >/dev/null 2>&1 || echo "mic: AGC (numid 9) not set" >&2
amixer -c "$CARD" cset numid=8 "$GAIN" >/dev/null 2>&1 || echo "mic: gain (numid 8) not set" >&2

echo "mic: card $CARD gain $(amixer -c "$CARD" cget numid=8 2>/dev/null | grep -o ': values=[0-9]*' | head -1), AGC off, $SRC"
