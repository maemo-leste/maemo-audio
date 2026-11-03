/* probe-cost -- what the measurement instrumentation costs the loop it
 * measures.
 *
 * The ALSA probe does a line-buffered fprintf plus an fflush once per
 * period, from the data loop. The capture tap additionally writes the
 * period's audio (8 KB at quantum 4096) and flushes an index row. Those
 * are blocking writes on a realtime thread, and every latency measured
 * with them enabled includes whatever they cost.
 *
 * This times exactly those operations, nothing else, so the number can be
 * subtracted from the measurements honestly instead of assumed away.
 *
 * Build: gcc -O2 -o probe-cost probe-cost.c
 * Run:   ./probe-cost [iters] [bytes]
 */
#include <inttypes.h>
#include <stdarg.h>
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

static int64_t s[MAXN];
static int n;

static int cmp(const void *a, const void *b)
{
	int64_t x = *(const int64_t *) a, y = *(const int64_t *) b;
	return x < y ? -1 : x > y;
}

static void report(const char *what)
{
	qsort(s, n, sizeof(int64_t), cmp);
	printf("  %-34s median %7.3f ms   99th %7.3f ms   max %7.3f ms\n",
			what, s[n / 2] / 1e6, s[(int) (n * 0.99)] / 1e6, s[n - 1] / 1e6);
}

int main(int argc, char *argv[])
{
	int iters = argc > 1 ? atoi(argv[1]) : 500;
	int bytes = argc > 2 ? atoi(argv[2]) : 8192;
	FILE *idx, *raw;
	char *buf = calloc(bytes, 1);
	int i;

	if (iters > MAXN)
		iters = MAXN;
	n = 0;

	idx = fopen("/tmp/probe-cost.idx", "w");
	raw = fopen("/tmp/probe-cost.raw", "wb");
	setvbuf(idx, NULL, _IOLBF, 0);

	printf("period cost of the instrumentation, %d iterations, %d bytes/period\n",
			iters, bytes);

	/* what the ALSA probe does per period */
	n = 0;
	for (i = 0; i < iters; i++) {
		uint64_t t0 = now_ns();
		fprintf(idx, "850000000000000,%d,%d,7360,4096,48000\n", i, i * 4096);
		fflush(idx);
		s[n++] = (int64_t) (now_ns() - t0);
	}
	report("fprintf + fflush (alsa probe)");

	/* what the capture tap does per period */
	n = 0;
	for (i = 0; i < iters; i++) {
		uint64_t t0 = now_ns();
		fwrite(buf, 1, bytes, raw);
		fflush(raw);
		fprintf(idx, "850000000000000,%d,%d,4096,0,48000,2\n", i, i * 4096);
		fflush(idx);
		s[n++] = (int64_t) (now_ns() - t0);
	}
	report("fwrite audio + 2x fflush (tap)");

	/* the mixer graph probe's dump: a burst of rows at once */
	n = 0;
	for (i = 0; i < (iters < 50 ? iters : 50); i++) {
		int k;
		uint64_t t0 = now_ns();
		for (k = 0; k < 4096; k++)
			fprintf(idx, "850000000000000,%d,%d,7360,4096,48000\n", k, k * 4096);
		fflush(idx);
		s[n++] = (int64_t) (now_ns() - t0);
	}
	report("4096-row dump in one go");

	fclose(idx);
	fclose(raw);
	free(buf);
	return 0;
}
