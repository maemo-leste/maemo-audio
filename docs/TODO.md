# Open items

The voice call path has been walked end to end and looks right. What follows
is what has not been exercised since the per-output rewrite, in the order I
would take it.

## A second card

The per-card adoption, the per-state codec arm and the substituted absolute
element all exist for this case and none of it has ever run with a second
playable card: Bluetooth, HDMI, a USB dock. What to watch for is a stream
that takes the absolute claim on one card while the other is the announced
output, and a codec thread that belongs to the wrong output.

## A stream destroyed while it is running

The absolute claim has to come down with the stream that raised it, or the
codec sits at MAX until some other stream's release corrects it. The port
release hands the module a NULL association, which is the right shape, but
the case has not been proved end to end.

## A longer cold-start soak

The session item race was ours, a `-ENOTSUP` answer to a param the factory
declared readable, and six restarts came out clean after the fix. The bug
used to show up within four, so twenty is the number I would trust.

## The slider during startup

Fast clicks while the stack is still coming up. This is the case the
per-state arm was partly written for and it has not been run since.
