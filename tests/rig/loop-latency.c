/* loop-latency -- how closely can a PipeWire main loop hit a chosen instant?
 *
 * The mix module lives on the main loop and the graph lives on the RT loop.
 * The module's whole job is to issue a codec write at an instant computed
 * from the graph clock, so the question that decides the architecture is
 * not whether the main loop is "fast enough" in the abstract but how far a
 * pw_loop timer lands from the deadline it was given, and how much of that
 * is bias (which can be subtracted) versus spread (which cannot).
 *
 * Everything here is in CLOCK_MONOTONIC nanoseconds, the same clock the
 * graph's spa_io_clock uses, so the numbers are directly comparable to a
 * quantum.
 *
 * A load generator is included because an idle main loop proves nothing:
 * the real one carries WirePlumber, D-Bus and metadata traffic, and a
 * timer that fires late only when something else is running is a
 * different problem from one that is always a little late.
 *
 * Build: gcc -O2 -o loop-latency loop-latency.c -lpipewire-0.3 -lpthread
 * Run:   ./loop-latency [period_us] [iters] [load_us] [load_every_us]
 */
#include <pipewire/pipewire.h>
#include <pipewire/main-loop.h>
#include <pipewire/loop.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAXN 20000

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000000000u + ts.tv_nsec;
}

struct probe {
	struct pw_loop *loop;
	struct pw_main_loop *ml;
	struct spa_source *timer;
	uint64_t target;			/* deadline of the next firing */
	uint64_t period_ns;
	uint64_t left;
	int64_t err[MAXN];
	int n;
	/* optional load: burn load_us every load_every_us */
	struct spa_source *load;
	uint64_t load_us;
	uint64_t load_every_ns;
	uint64_t load_next;
};

static void ns_to_ts(uint64_t ns, struct timespec *ts)
{
	ts->tv_sec = (time_t) (ns / 1000000000u);
	ts->tv_nsec = (long) (ns % 1000000000u);
}

static void burn(uint64_t us)
{
	uint64_t until = now_ns() + us * 1000;
	volatile double x = 1.0;
	while (now_ns() < until)
		x = x * 1.0000001 + 1e-9;
}

static void load_cb(void *data, uint64_t expirations)
{
	struct probe *p = data;
	struct timespec v;
	(void) expirations;
	burn(p->load_us);
	p->load_next += p->load_every_ns;
	ns_to_ts(p->load_next, &v);
	pw_loop_update_timer(p->loop, p->load, &v, NULL, true);
}

static void timer_cb(void *data, uint64_t expirations)
{
	struct probe *p = data;
	uint64_t t = now_ns();
	struct timespec v;
	(void) expirations;

	if (p->n < MAXN)
		p->err[p->n++] = (int64_t) t - (int64_t) p->target;
	if (--p->left == 0) {
		pw_main_loop_quit(p->ml);
		return;
	}
	p->target += p->period_ns;
	ns_to_ts(p->target, &v);
	pw_loop_update_timer(p->loop, p->timer, &v, NULL, true);
}

static int cmp_i64(const void *a, const void *b)
{
	int64_t x = *(const int64_t *) a, y = *(const int64_t *) b;
	return x < y ? -1 : x > y;
}

int main(int argc, char *argv[])
{
	uint64_t period_us = argc > 1 ? strtoull(argv[1], NULL, 10) : 20000;
	uint64_t iters = argc > 2 ? strtoull(argv[2], NULL, 10) : 500;
	uint64_t load_us = argc > 3 ? strtoull(argv[3], NULL, 10) : 0;
	uint64_t load_every_us = argc > 4 ? strtoull(argv[4], NULL, 10) : 5000;
	struct probe p;
	struct pw_main_loop *ml;
	size_t i;
	int64_t sum = 0;
	double mean, var = 0.0;

	if (iters > MAXN)
		iters = MAXN;

	pw_init(&argc, &argv);

	ml = pw_main_loop_new(NULL);
	if (ml == NULL) {
		fprintf(stderr, "could not create main loop\n");
		return 1;
	}
	p.loop = pw_main_loop_get_loop(ml);
	p.ml = ml;
	p.period_ns = period_us * 1000;
	p.left = iters;
	p.n = 0;
	p.target = now_ns() + p.period_ns;
	{
		struct timespec v;
		ns_to_ts(p.target, &v);
		p.timer = pw_loop_add_timer(p.loop, timer_cb, &p);
		pw_loop_update_timer(p.loop, p.timer, &v, NULL, true);
	}

	if (load_us > 0) {
		p.load_us = load_us;
		p.load_every_ns = load_every_us * 1000;
		p.load_next = now_ns() + p.load_every_ns;
		{
			struct timespec v;
			ns_to_ts(p.load_next, &v);
			p.load = pw_loop_add_timer(p.loop, load_cb, &p);
			pw_loop_update_timer(p.loop, p.load, &v, NULL, true);
		}
		printf("# load: %" PRIu64 " us burned every %" PRIu64 " us\n",
				load_us, load_every_us);
	}

	pw_main_loop_run(ml);

	for (i = 0; i < p.n; i++)
		sum += p.err[i];
	mean = (double) sum / (double) p.n;
	for (i = 0; i < p.n; i++) {
		double d = (double) p.err[i] - mean;
		var += d * d;
	}
	var = p.n > 1 ? var / (double) (p.n - 1) : 0.0;
	qsort(p.err, p.n, sizeof(int64_t), cmp_i64);

	printf("period %" PRIu64 " us, %" PRIu64 " iterations, %zu measured\n",
			period_us, iters, p.n);
	printf("  late by:  min %+.3f ms   median %+.3f ms   max %+.3f ms\n",
			p.err[0] / 1e6, p.err[p.n / 2] / 1e6, p.err[p.n - 1] / 1e6);
	printf("  mean %+.3f ms   sd %.3f ms\n", mean / 1e6, sqrt(var) / 1e6);
	printf("  99th pct %+.3f ms\n", p.err[(size_t) (p.n * 0.99)] / 1e6);
	printf("  bias is subtractable; the spread is the budget: %.3f ms\n",
			(p.err[p.n - 1] - p.err[0]) / 1e6);

	pw_main_loop_destroy(ml);
	pw_deinit();
	return 0;
}
