/* A faithful transcription of Nokia's delayed hardware-volume queue.
 *
 * Source: patch 0081 "core: Add infrastructure for delayed HW volume
 * setting", functions pa_sink_volume_change_push() / _apply() / _flush()
 * in src/pulsecore/sink.c.
 *
 * Transcribed literally, with three substitutions and nothing else:
 *
 *   pa_cvolume  -> int step          (Nokia compares pa_cvolume_avg(),
 *                                     i.e. a single monotone number, so
 *                                     a ladder index carries the same
 *                                     ordering)
 *   pa_rtclock  -> a `now` argument  (so the harness owns the clock)
 *   pa_usec_t   -> uint64_t ns
 *
 * Rewind is deliberately absent: PipeWire 1.6.9 has no rewind protocol,
 * so pa_sink_volume_change_rewind() has no counterpart and is not a
 * behaviour we claim to match.
 *
 * Every branch, comparison operator and constant below is kept as it is
 * in the patch, including the unsigned subtraction in the down case and
 * the strict `now > at` in apply. Where our queue differs, the
 * difference is supposed to show up as a mismatch here rather than be
 * hidden by a tidy-up.
 */
#ifndef NOKIA_QUEUE_H
#define NOKIA_QUEUE_H

#include <stdint.h>
#include <stdbool.h>

typedef struct nchange {
	uint64_t at;			/* pa_usec_t at */
	int step;			/* was hw_volume */
	struct nchange *next, *prev;
} nchange;

typedef struct {
	nchange *head, *tail;		/* PA_LLIST_HEAD + volume_changes_tail */
	int current_hw;			/* thread_info.current_hw_volume */
	uint64_t extra_delay;		/* volume_change_extra_delay */
	uint64_t safety_margin;		/* volume_change_safety_margin */
	unsigned writes;		/* write_volume() calls, for reporting */
} nokia_q;

void nokia_init(nokia_q *q, int current_hw,
		uint64_t extra_delay_ns, uint64_t safety_margin_ns);
void nokia_done(nokia_q *q);

/* pa_sink_volume_change_push(). `latency` is what
 * PA_SINK_MESSAGE_GET_LATENCY returned, i.e. how long audio written now
 * takes to reach the DAC. */
void nokia_push(nokia_q *q, int desired_hw_step, uint64_t latency_ns,
		uint64_t now_ns);

/* pa_sink_volume_change_apply(). Returns true when write_volume() would
 * have been called, i.e. when at least one entry became due. Nokia calls
 * write_volume() once for the whole drained batch, so one call here
 * means one hardware write. */
bool nokia_apply(nokia_q *q, uint64_t now_ns, int *step_out, uint64_t *at_out);

/* pa_sink_volume_change_flush(). */
void nokia_flush(nokia_q *q, bool write_to_hw, int *step_out);

unsigned nokia_depth(const nokia_q *q);

#endif
