/* Transcription of Nokia's delayed hardware-volume queue -- see
 * nokia_queue.h for exactly what is literal and what was substituted.
 * Line-for-line correspondence to the patch is noted at each step. */
#include <stdlib.h>
#include <string.h>

#include "nokia_queue.h"

void nokia_init(nokia_q *q, int current_hw,
		uint64_t extra_delay_ns, uint64_t safety_margin_ns)
{
	memset(q, 0, sizeof(*q));
	q->head = q->tail = NULL;
	q->current_hw = current_hw;
	q->extra_delay = extra_delay_ns;
	q->safety_margin = safety_margin_ns;
}

void nokia_done(nokia_q *q)
{
	nchange *c = q->head;

	while (c != NULL) {
		nchange *n = c->next;
		free(c);
		c = n;
	}
	q->head = q->tail = NULL;
}

static nchange *nchange_new(void)
{
	nchange *c = calloc(1, sizeof(*c));
	return c;
}

static void prepend(nokia_q *q, nchange *nc)
{
	nc->prev = NULL;
	nc->next = q->head;
	if (q->head != NULL)
		q->head->prev = nc;
	else
		q->tail = nc;
	q->head = nc;
}

/* PA_LLIST_INSERT_AFTER(list, c, nc) */
static void insert_after(nokia_q *q, nchange *c, nchange *nc)
{
	nc->prev = c;
	nc->next = c->next;
	if (c->next != NULL)
		c->next->prev = nc;
	else
		q->tail = nc;
	c->next = nc;
}

/* ---- pa_sink_volume_change_push() ---- */
void nokia_push(nokia_q *q, int desired, uint64_t latency_ns, uint64_t now_ns)
{
	nchange *c = NULL;
	nchange *nc = nchange_new();
	uint64_t margin = q->safety_margin;
	const char *direction = NULL;

	nc->step = desired;

	/*  if (!s->thread_info.volume_changes &&
	 *      pa_cvolume_equal(&nc->hw_volume, &s->thread_info.current_hw_volume))
	 *      { pa_log_debug("Volume not changing"); return; }
	 *
	 * Note the guard on an empty queue: a repeat of the resting level
	 * is only ignored when nothing is in flight. */
	if (q->head == NULL && desired == q->current_hw) {
		free(nc);
		return;
	}

	/*  if (process_msg(GET_LATENCY, &nc->at) < 0) nc->at = 0;
	 *  nc->at += pa_rtclock_now() + extra_delay; */
	nc->at = latency_ns + now_ns + q->extra_delay;

	/* The tail walk. Back from the newest entry, stop at the first one
	 * this new transition may be shifted past. The margin is applied
	 * only if the shifted instant still lands after the entry being
	 * compared against, which is what keeps a burst of close-together
	 * requests from each pushing the next one further out. */
	if (q->tail != NULL) {
		for (c = q->tail; c != NULL; c = c->prev) {
			if (desired > c->step) {
				if (nc->at + margin > c->at) {
					nc->at += margin;
					direction = "up";
					break;
				}
			} else if (nc->at - margin > c->at) {
				nc->at -= margin;
				direction = "down";
				break;
			}
		}
	}

	if (c == NULL) {
		/* Nothing to sit behind: aim against the level the hardware
		 * is actually at. */
		if (desired > q->current_hw) {
			nc->at += margin;
			direction = "up";
		} else {
			nc->at -= margin;
			direction = "down";
		}
		prepend(q, nc);
	} else {
		insert_after(q, c, nc);
	}

	(void) direction;

	/*  "We can ignore volume events that came earlier but should happen
	 *   later than this." Everything behind the new entry is dropped. */
	{
		nchange *x = nc->next;
		nc->next = NULL;
		q->tail = nc;
		while (x != NULL) {
			nchange *nx = x->next;
			free(x);
			x = nx;
		}
	}
}

/* ---- pa_sink_volume_change_apply() ---- */
bool nokia_apply(nokia_q *q, uint64_t now_ns, int *step_out, uint64_t *at_out)
{
	bool ret = false;
	int step = q->current_hw;
	uint64_t at = 0;

	/* Strict >, and every due entry is drained before a single write
	 * is issued: the codec jumps to the newest level, it does not
	 * visit the intermediates. */
	while (q->head != NULL && now_ns > q->head->at) {
		nchange *c = q->head;

		q->head = c->next;
		if (q->head != NULL)
			q->head->prev = NULL;
		else
			q->tail = NULL;

		step = c->step;
		at = c->at;
		free(c);
		ret = true;
	}

	if (ret) {
		q->current_hw = step;
		q->writes++;
		if (step_out != NULL)
			*step_out = step;
		if (at_out != NULL)
			*at_out = at;
	}

	if (q->head == NULL)
		q->tail = NULL;

	return ret;
}

/* ---- pa_sink_volume_change_flush() ---- */
void nokia_flush(nokia_q *q, bool write_to_hw, int *step_out)
{
	nchange *c = q->head;
	nchange *last = NULL;

	q->head = NULL;
	q->tail = NULL;

	while (c != NULL) {
		nchange *next = c->next;
		if (next == NULL)
			last = c;
		free(c);
		c = next;
	}

	/* Only the newest entry is honoured, and only when asked. */
	if (last != NULL) {
		if (write_to_hw) {
			q->current_hw = last->step;
			q->writes++;
			if (step_out != NULL)
				*step_out = last->step;
		} else if (step_out != NULL) {
			*step_out = -1;
		}
		free(last);
	} else if (step_out != NULL) {
		*step_out = -1;
	}
}

unsigned nokia_depth(const nokia_q *q)
{
	unsigned n = 0;
	const nchange *c;

	for (c = q->head; c != NULL; c = c->next)
		n++;
	return n;
}
