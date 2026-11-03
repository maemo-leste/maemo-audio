/* The software step, the fallback for an output with no hardware control.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MAEMO_SWSTEP_H
#define MAEMO_SWSTEP_H

#include "maemo-export.h"

#include <spa/support/loop.h>

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* A software gain step anchored to a graph position rather than to a
 * wall-clock instant.
 *
 * That anchoring is the whole point: a step taken on the cycle whose
 * position is named has no scheduling jitter in it at all, so the
 * software half of a software/hardware pair is exactly where it says it
 * will be. Whatever bump or dip survives in the recording then belongs to
 * the hardware write and to nothing else -- which is the thing under
 * test. A step taken from a timer would put main-loop jitter into the
 * half that is supposed to be the reference.
 */
struct maemo_swstep {
	bool enabled;
	uint64_t at;			/* graph position of the step */
	float to;			/* software gain from that cycle on */
	float hw_to;			/* route volume to arm, < 0 for none */
	bool hw_armed;
	bool done;
	const char *path;
	FILE *f;
};

int maemo_swstep_open(struct maemo_swstep *s, const char *path,
		uint64_t at, float to, float hw_to);
/* Returns the gain to use for this cycle, arms the hardware write once,
 * and logs the transition once. delay_frames and rate come from this
 * node's own clock: the deadline has to be computed where the position is
 * known, which is not the control loop. */
float maemo_swstep_apply(struct maemo_swstep *s, float gain, uint64_t position,
		uint64_t delay_frames, uint32_t rate);

/* Hand the mixer the control loop and the callback to run there.
 *
 * The module calls this once, after it has created the mixer node. Both
 * live in the same process and the module links against this library, so
 * a plain exported call beats a spa extension: there is nothing to look
 * up, nothing to version-negotiate, and the symbol either exists at link
 * time or the build fails.
 *
 * One slot, because there is one mixer node that matters and one control
 * loop. Until this is called the mixer switches its software gain and
 * arms nothing -- half a coordinated change is worse than none. */
MAEMO_EXPORT void maemo_swstep_arm(struct spa_loop *loop, spa_invoke_func_t cb,
		void *user_data);

#endif /* MAEMO_SWSTEP_H */
