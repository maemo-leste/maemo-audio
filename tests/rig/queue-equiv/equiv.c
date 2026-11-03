/* Nokia queue vs our queue: same events in, same writes out?
 *
 * Both queues are driven with an identical event script on a shared
 * virtual clock, and the sequence of hardware writes each one issues is
 * recorded and compared. Nothing about the audio, the mixer or PipeWire
 * is involved -- this is the scheduling law on its own, which is the
 * part that has to be right before any of the acoustic work means
 * anything.
 *
 * Two modes are run:
 *
 *   grid off -- our snap_to_grid has no measured period grid, so a lift
 *              gets a plain +margin and a release is left alone. This is
 *              the mode directly comparable to Nokia's +-margin.
 *
 *   grid on  -- a synthetic 512-frame period grid at 48 kHz, i.e. what
 *              the module publishes on the VM. Divergence here is
 *              expected and is the period quantisation, not a
 *              disagreement about ordering.
 *
 * Out of tree on purpose: it links against the mixer sources but is not
 * part of the package.
 *
 *   make -f Makefile && ./equiv
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "maemo-vol-engine.h"
#include "maemo-vol-map.h"
#include "mix-registry.h"
#include "nokia_queue.h"

/* Same constants as tests/test-vol-engine.c so the two are readable
 * side by side. Nokia's own defaults are 5 ms / 5 ms; both queues get
 * the same pair here so any difference is the algorithm and not the
 * tuning. */
#define EXTRA_NS   (5  * 1000000ULL)
#define MARGIN_NS  (10 * 1000000ULL)
#define LATENCY_NS (20 * 1000000ULL)
#define TICK_NS    (1  * 1000000ULL)
#define HORIZON_NS (400 * 1000000ULL)
#define T0         (1000000000ULL)

static const maemo_vol_timing TIMING = {
	.extra_delay_ns = EXTRA_NS,
	.safety_margin_ns = MARGIN_NS,
};

static const float SLIDER[] = {
	0.0f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f
};
#define N_SLIDER (sizeof(SLIDER)/sizeof(SLIDER[0]))

static const float LADDER[] = { 0.1f, 0.3f, 0.5f, 0.7f, 1.0f };
#define N_LADDER (sizeof(LADDER)/sizeof(LADDER[0]))

/* ---- the event script ---- */

enum ev_kind { EV_CLAIM, EV_RELEASE, EV_SLIDER };

typedef struct {
	uint64_t t_ns;
	enum ev_kind kind;
	int value;
} event;

#define MS(x) ((uint64_t)(x) * 1000000ULL)

/* ---- write traces ---- */

#define MAX_WRITES 64

typedef struct {
	uint64_t at_ns;
	int step;
} write_rec;

typedef struct {
	write_rec w[MAX_WRITES];
	unsigned n;
} trace;

static void trace_add(trace *t, uint64_t at, int step)
{
	if (t->n < MAX_WRITES) {
		t->w[t->n].at_ns = at;
		t->w[t->n].step = step;
		t->n++;
	}
}

/* ---- one run ---- */

struct run {
	const char *name;
	const event *ev;
	unsigned n_ev;
	int base_slider;	/* where the slider rests */
	int base_hw;		/* the hardware step that goes with it */
};

static struct maemo_vol_map *g_map;

/* The hardware step a slider position maps to, for the Nokia side,
 * which has no map of its own and is fed the level directly. */
static int hw_for_slider(int slider)
{
	return maemo_vol_map_hw_step(g_map, (unsigned) slider);
}

static void run_both(const struct run *r, bool grid_on,
		trace *ours, trace *theirs)
{
	maemo_vol_engine e;
	nokia_q q;
	uint64_t now = T0;
	unsigned i;
	int base_hw;
	int live_claim = -1;
	int cur_slider = r->base_slider;

	/* The grid is process-global state in the registry, so it has to
	 * be cleared explicitly for the grid-off mode. */
	if (grid_on)
		maemo_mix_registry_set_grid(512, 48000, 0, now, now);
	else
		maemo_mix_registry_set_grid(0, 0, -1, 0, 0);

	base_hw = hw_for_slider(r->base_slider);
	maemo_vol_engine_init(&e, g_map, r->base_slider, base_hw);
	nokia_init(&q, base_hw, EXTRA_NS, MARGIN_NS);
	ours->n = 0;
	theirs->n = 0;

	for (i = 0; i <= r->n_ev; i++) {
		uint64_t stop = (i < r->n_ev) ? r->ev[i].t_ns : T0 + HORIZON_NS;

		/* Advance the shared clock and let both drain. Draining on
		 * the same ticks is what makes the comparison about the
		 * schedule rather than about who was polled when. */
		for (; now <= stop; now += TICK_NS) {
			int step;
			uint64_t at;

			while (maemo_vol_engine_due(&e, now, &step, &at)) {
				trace_add(ours, now, step);
				maemo_vol_engine_hw_applied(&e, step);
			}

			if (nokia_apply(&q, now, &step, &at))
				trace_add(theirs, now, step);
		}

		if (i == r->n_ev)
			break;

		switch (r->ev[i].kind) {
		case EV_CLAIM:
			live_claim = r->ev[i].value;
			maemo_vol_engine_set_claim(&e, r->ev[i].value,
					r->ev[i].t_ns, LATENCY_NS, &TIMING);
			nokia_push(&q, hw_for_slider(r->base_slider) > r->ev[i].value
					? hw_for_slider(r->base_slider) : r->ev[i].value,
					LATENCY_NS, r->ev[i].t_ns);
			break;
		case EV_RELEASE:
			live_claim = -1;
			maemo_vol_engine_set_claim(&e, -1,
					r->ev[i].t_ns, LATENCY_NS, &TIMING);
			/* Back to wherever the slider rests now, which is
			 * not necessarily where it rested at the start. */
			nokia_push(&q, hw_for_slider(cur_slider),
					LATENCY_NS, r->ev[i].t_ns);
			break;
		case EV_SLIDER:
			cur_slider = r->ev[i].value;
			maemo_vol_engine_set_step(&e, r->ev[i].value,
					r->ev[i].t_ns, LATENCY_NS, &TIMING);
			{
				/* The same max() the engine applies: a slider
				 * move while a claim is live still has to
				 * honour the claim. */
				int want = hw_for_slider(r->ev[i].value);
				if (live_claim > want)
					want = live_claim;
				nokia_push(&q, want, LATENCY_NS, r->ev[i].t_ns);
			}
			break;
		}
	}

	if (grid_on)
		maemo_mix_registry_set_grid(0, 0, -1, 0, 0);
}

/* ---- reporting ---- */

static void dump(const char *who, const trace *t)
{
	unsigned i;

	printf("    %-6s", who);
	if (t->n == 0) {
		printf("(no writes)\n");
		return;
	}
	for (i = 0; i < t->n; i++)
		printf(" [t+%3" PRIu64 "ms step %d]",
				(t->w[i].at_ns - T0) / 1000000ULL, t->w[i].step);
	printf("\n");
}

static const char *compare(const trace *a, const trace *b,
		char *note, size_t note_len)
{
	unsigned i;
	uint64_t worst = 0;

	if (a->n != b->n) {
		snprintf(note, note_len, "write count %u vs %u", a->n, b->n);
		return "COUNT";
	}
	for (i = 0; i < a->n; i++) {
		if (a->w[i].step != b->w[i].step) {
			snprintf(note, note_len, "write %u: step %d vs %d",
					i, a->w[i].step, b->w[i].step);
			return "STEP";
		}
	}
	for (i = 0; i < a->n; i++) {
		uint64_t d = a->w[i].at_ns > b->w[i].at_ns
			? a->w[i].at_ns - b->w[i].at_ns
			: b->w[i].at_ns - a->w[i].at_ns;
		if (d > worst)
			worst = d;
	}
	if (worst > TICK_NS) {
		snprintf(note, note_len, "same steps, worst timing skew %" PRIu64 " ms",
				worst / 1000000ULL);
		return "TIME";
	}
	snprintf(note, note_len, "identical");
	return "MATCH";
}

int main(void)
{
	struct run runs[] = {
		{ "one click",
		  (const event[]){ { T0, EV_CLAIM, 4 }, { T0 + MS(60), EV_RELEASE } },
		  2, 5, 2 },

		{ "click then click 5ms later",
		  (const event[]){ { T0, EV_CLAIM, 4 }, { T0 + MS(20), EV_RELEASE },
		                   { T0 + MS(25), EV_CLAIM, 4 }, { T0 + MS(85), EV_RELEASE } },
		  4, 5, 2 },

		{ "release before the lift lands",
		  (const event[]){ { T0, EV_CLAIM, 4 }, { T0 + MS(5), EV_RELEASE } },
		  2, 5, 2 },

		{ "burst of 4 clicks, 8ms apart",
		  (const event[]){ { T0, EV_CLAIM, 4 }, { T0 + MS(8), EV_RELEASE },
		                   { T0 + MS(16), EV_CLAIM, 4 }, { T0 + MS(24), EV_RELEASE },
		                   { T0 + MS(32), EV_CLAIM, 4 }, { T0 + MS(40), EV_RELEASE },
		                   { T0 + MS(48), EV_CLAIM, 4 }, { T0 + MS(56), EV_RELEASE } },
		  8, 5, 2 },

		{ "slider moved during a claim",
		  (const event[]){ { T0, EV_CLAIM, 4 }, { T0 + MS(10), EV_SLIDER, 7 },
		                   { T0 + MS(60), EV_RELEASE } },
		  3, 5, 2 },

		{ "slider staircase",
		  (const event[]){ { T0, EV_SLIDER, 6 }, { T0 + MS(15), EV_SLIDER, 7 },
		                   { T0 + MS(30), EV_SLIDER, 8 }, { T0 + MS(45), EV_SLIDER, 3 } },
		  4, 5, 2 },

		{ "claim quieter than the slider",
		  (const event[]){ { T0, EV_CLAIM, 1 }, { T0 + MS(60), EV_RELEASE } },
		  2, 8, 4 },
	};
	unsigned r_i, mode;
	int verdict_count[4] = { 0, 0, 0, 0 };
	trace ours, theirs;
	char note[128];
	const char *verdict;
	static const char *modes[2] = { "grid off", "grid on " };

	g_map = maemo_vol_map_new(SLIDER, N_SLIDER, LADDER, N_LADDER);
	if (g_map == NULL) {
		fprintf(stderr, "cannot build map\n");
		return 1;
	}

	printf("Nokia queue vs maemo_hw_queue\n");
	printf("latency %" PRIu64 " ms, extra delay %" PRIu64 " ms, margin %" PRIu64 " ms,"
	       " tick %" PRIu64 " ms\n",
			LATENCY_NS / 1000000ULL, EXTRA_NS / 1000000ULL,
			MARGIN_NS / 1000000ULL, TICK_NS / 1000000ULL);
	printf("ladder: 0=0.1 1=0.3 2=0.5 3=0.7 4=1.0;  slider rests at 5 (0.5)\n\n");

	for (mode = 0; mode < 2; mode++) {
		bool grid_on = (mode == 1);

		printf("=== %s ===\n", modes[mode]);
		for (r_i = 0; r_i < sizeof(runs)/sizeof(runs[0]); r_i++) {
			run_both(&runs[r_i], grid_on, &ours, &theirs);
			verdict = compare(&ours, &theirs, note, sizeof(note));
			printf("  %-32s %-6s %s\n", runs[r_i].name, verdict, note);
			dump("ours", &ours);
			dump("nokia", &theirs);
			if (!strcmp(verdict, "MATCH")) verdict_count[0]++;
			else if (!strcmp(verdict, "TIME")) verdict_count[1]++;
			else if (!strcmp(verdict, "STEP")) verdict_count[2]++;
			else verdict_count[3]++;
		}
		printf("\n");
	}

	printf("totals over both modes: MATCH %d  TIME %d  STEP %d  COUNT %d\n",
			verdict_count[0], verdict_count[1],
			verdict_count[2], verdict_count[3]);

	maemo_vol_map_free(g_map);
	return 0;
}
