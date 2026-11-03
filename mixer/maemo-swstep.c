/* The software step. See maemo-swstep.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>

#include <spa/support/loop.h>
#include <pipewire/pipewire.h>

#include "maemo-mixer-ext.h"
#include "maemo-swstep.h"

int maemo_swstep_open(struct maemo_swstep *s, const char *path,
		uint64_t at, float to, float hw_to)
{
	memset(s, 0, sizeof(*s));
	if (path == NULL || *path == '\0')
		return -EINVAL;
	s->path = path;
	s->at = at;
	s->to = to;
	s->hw_to = hw_to;
	s->enabled = true;
	return 0;
}

/* Set once by the module. Written on the main loop long before the data
 * loop ever reads it, so no ordering machinery is needed. */
static struct spa_loop *arm_loop;
static spa_invoke_func_t arm_cb;
static void *arm_data;

MAEMO_EXPORT void maemo_swstep_arm(struct spa_loop *loop, spa_invoke_func_t cb,
		void *user_data)
{
	arm_loop = loop;
	arm_cb = cb;
	arm_data = user_data;
}

float maemo_swstep_apply(struct maemo_swstep *s, float gain, uint64_t position,
		uint64_t delay_frames, uint32_t rate)
{
	struct timespec ts;

	if (!s->enabled)
		return gain;
	/* Once taken, the step is permanent. Returning the original gain
	 * after the transition -- which an earlier version did, because
	 * `done' was checked before the value was used -- made the step
	 * last exactly one quantum, and the resulting one-quantum dip in
	 * the recording looked like a coordination error rather than like
	 * the instrument. */
	if (s->done)
		return s->to;
	if (position < s->at)
		return gain;

	s->done = true;		/* one transition, ever; this is a probe */

	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint64_t now = (uint64_t) ts.tv_sec * 1000000000u + ts.tv_nsec;

	/* The software half takes effect on this cycle's samples, so the
	 * hardware half must become audible one playback distance later.
	 * Both numbers come from this node's own clock, which is the only
	 * place that knows where in the stream this cycle actually is. */
	uint64_t deadline = now + (rate ? delay_frames * 1000000000ull / rate : 0);
	if (s->hw_to >= 0.0f && !s->hw_armed && arm_loop != NULL && arm_cb != NULL) {
		struct maemo_hw_change ch;
		ch.deadline_ns = deadline;
		ch.value = s->hw_to;
		s->hw_armed = true;
		spa_loop_invoke(arm_loop, arm_cb, 0, &ch, sizeof(ch), false,
				arm_data);
	}

	if (s->f == NULL) {
		s->f = fopen(s->path, "w");
		if (s->f == NULL)
			return s->to;
		setvbuf(s->f, NULL, _IOLBF, 0);
		fprintf(s->f, "wall_ns,position,from,to,deadline_ns,hw_to\n");
	}
	if (s->f != NULL)
		fprintf(s->f, "%" PRIu64 ",%" PRIu64 ",%.6f,%.6f,%" PRIu64 ",%.6f\n",
				now, position, (double) gain, (double) s->to,
				deadline, (double) s->hw_to);
	return s->to;
}
