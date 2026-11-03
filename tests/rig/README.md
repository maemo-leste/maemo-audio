# Measurement rig

A copy of `/home/user/maemo-testrig`, which is where these are built and run
from. That directory is not under version control, and `docs/timing.md`
section 4 documents the tools by that path -- so this copy exists so the
instruments a measurement was made with survive the machine it was made on.

Work happens in `/home/user/maemo-testrig`; copy back and forth rather than
editing here, or the rig and the copy drift and a later run cannot be
reproduced from either.

## Building

Individual tools, not a project:

```
cc -O2 -o slider slider.c -lpipewire-0.3
cc -O2 -o mis-click mis-click.c -lcanberra        # -lcanberra is required
cc -O2 -o codec-step codec-step.c -lasound
cc -O2 -o loop-latency loop-latency.c -lpthread
```

`queue-equiv/` has its own makefile: it is a transcription of Nokia's
delayed-HW-volume queue with an equivalence harness, and it is the reference
the module's queue was checked against.

## The ones that matter

| tool | what it answers |
|---|---|
| `duck-acoustic-run.sh` + `duck-envelope.py` | does the music bed hold level during a notification? Restarts the stack with the capture tap on, plays a bed, fires a click, prints the envelope and the bed shift in dB. This is what turned "the music gets louder" into a number. |
| `t-codec2-run.sh` + `t-codec3.py` | `t_codec`, the codec's apply latency, by emitting a beep and a codec step together so the capture path's delay is common mode. |
| `mis-click.c` | resident MIS stand-in: primes one pooled sample, fires it on `SIGUSR1`. Note `CA_PROP_CANBERRA_VOLUME` must be a bare dB number -- `"-5.0 dB"` is rejected with an `EINVAL` that names neither the property nor the reason. |
| `slider.c` | set the volume target the applet would write, `-s <rung>`. |
| `codec-step.c` | write an ALSA control at a logged instant. Takes a **percent**, not a raw value, and wants `hw:0` not `0`. Raw writes do not survive: the ALSA node re-asserts the control. |
| `calibrate-mic.sh` | put the capture in a known state before any acoustic run. Read its header: the capture gain is part of the instrument, and both saturation and noise floor look exactly like "the change made no difference". |

## Before any acoustic measurement

`docs/timing.md` section 5 is not optional reading. The short version: check
the recording level before trusting a level, never delete a file a running
process has open, and prefer a difference within one clock domain to any
absolute timestamp.

## Rig state survives the test that changed it

`wpctl set-default <id>` is **not** transient. It writes
`default.configured.audio.sink` (or `.source`) into
`~/.local/state/wireplumber/default-nodes`, and a configured default
outranks every priority rule in the tree -- `monitor.alsa.rules`,
`monitor.bluez.rules`, the lot. A test that aimed the default at a device
therefore changes the rig for every run afterwards, and the symptom is
"auto-switch to the headset stopped working", which points nowhere near
the file responsible.

Check it before believing a routing failure:

    cat ~/.local/state/wireplumber/default-nodes

To undo, delete the line and restart the stack. The non-persistent way to
aim one stream at a device is that stream's own `target.node`; the routing
script deliberately does not use the configured default for anything, for
exactly this reason.

The same file also holds `default.configured.audio.source`, which is
usually legitimate -- it gets written the first time something picks a
microphone.

## Bringing the headset back quickly

After a stack restart the headset takes 10-70 s to reconnect on its own, and
test loops that poll for it waste most of their time. The ACL link is
already up; only the audio profile is missing, and that is a call away:

    bluetoothctl connect 64:68:76:19:CE:16     # sink appears in ~1 s

`hcitool con` shows whether the ACL link exists. Use this instead of
sleeping on EDIFIER appearing in `wpctl status`.

## wpctl set-default is persistent -- clean it after switch loops

Restated because it was hit again: a switch loop that ends on a `wpctl
set-default` leaves `default.configured.audio.sink` in
`~/.local/state/wireplumber/default-nodes`, and a configured default
outranks every priority rule, so the headset stops taking over on connect
and the cause looks like a policy bug. Delete the
`default.configured.audio.sink*` lines and restart the stack.
