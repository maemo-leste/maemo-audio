/* The one thing the mixer needs from the module: a way to ask for a write
 * on the control loop, at an instant the mixer computed.
 *
 * Why an extension rather than a param: a param is data, and what has to
 * cross is a loop pointer plus a callback. Why not a global: the factory
 * is instantiated more than once, and only one of those instances ever
 * processes, so a singleton would be attached to the wrong node exactly
 * as often as not.
 *
 * The extension is therefore stored inside the node's own impl, with the
 * impl reachable from it, and its functions take the extension itself as
 * the first argument. No globals, no handle spelunking, and a node that
 * never gets armed simply does not arm anything.
 */
#ifndef SPA_MAEMO_MIXER_EXT_H
#define SPA_MAEMO_MIXER_EXT_H

#include <spa/support/loop.h>
#include <stdint.h>

/* The change the mixer asks the control loop to make on its behalf.
 *
 * Passed by value through spa_loop_invoke, which copies `size' bytes into
 * the queue: nothing is allocated on the data loop and nothing has to
 * outlive the call.
 *
 * deadline_ns is absolute, CLOCK_MONOTONIC, and is computed on the data
 * loop from this node's own clock -- never in the callback. spa_loop_invoke
 * makes no promise about when the control loop gets around to running the
 * callback, so a deadline measured from the arrival would inherit every
 * millisecond of whatever WirePlumber was doing. If the deadline has
 * already passed on arrival the write is issued immediately and the
 * overrun logged, so a late hand-off is visible rather than silent.
 */
struct maemo_hw_change {
	uint64_t deadline_ns;
	float value;
};

#define SPA_MAEMO_MIXER_EXT_ID		"maemo.mixer.v1"
#define SPA_MAEMO_MIXER_EXT_VERSION	1

struct spa_maemo_mixer_ext {
	uint32_t version;
	/* Hand the mixer the control loop, the callback to run there and
	 * the opaque pointer to hand back to it. Called once, at creation.
	 * Until it is called the mixer still switches its software gain
	 * and simply arms nothing, which is the correct degraded
	 * behaviour: half a coordinated change is worse than none. */
	int (*arm_sink_write)(struct spa_maemo_mixer_ext *ext,
			struct spa_loop *control_loop,
			spa_invoke_func_t cb,
			void *user_data);
};

#endif /* SPA_MAEMO_MIXER_EXT_H */
