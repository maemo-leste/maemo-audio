/* codec-step -- write an ALSA control at a known instant and say exactly when.
 *
 * The question it answers: after the register write returns, how long until
 * the change is audible? The mixer's software path has a measured 153 ms of
 * queue in front of the DAC, but a codec gain change acts on whatever the
 * converter is turning into electricity right now, so its latency is the
 * part's own -- a register write plus any internal ramp or latch. That
 * number is what decides how far apart to place a software duck and a
 * hardware step if they are to land on the same frame.
 *
 * amixer would do the write but not say when, and its process startup is
 * several milliseconds, which is the entire quantity being measured. So:
 * prepare everything, sleep, then write, and print the instants on both
 * sides of it.
 *
 * Build: gcc -O2 -o codec-step codec-step.c -lasound
 * Run:   ./codec-step <card> <numid> <percent> [delay_ms]
 */
#include <alsa/asoundlib.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000000000u + ts.tv_nsec;
}

int main(int argc, char *argv[])
{
	const char *card = argc > 1 ? argv[1] : "0";
	long numid = argc > 2 ? atol(argv[2]) : 6;
	long percent = argc > 3 ? atol(argv[3]) : 100;
	long delay_ms = argc > 4 ? atol(argv[4]) : 0;
	snd_ctl_t *ctl = NULL;
	snd_ctl_elem_id_t *id;
	snd_ctl_elem_info_t *info;
	snd_ctl_elem_value_t *val;
	long lo, hi, v;
	int res;
	uint64_t t_before, t_after;

	if ((res = snd_ctl_open(&ctl, card, 0)) < 0) {
		fprintf(stderr, "open %s: %s\n", card, snd_strerror(res));
		return 1;
	}

	snd_ctl_elem_id_alloca(&id);
	snd_ctl_elem_info_alloca(&info);
	snd_ctl_elem_value_alloca(&val);

	snd_ctl_elem_id_set_numid(id, numid);
	snd_ctl_elem_info_set_numid(info, numid);
	if ((res = snd_ctl_elem_info(ctl, info)) < 0) {
		fprintf(stderr, "elem_info numid=%ld: %s\n", numid, snd_strerror(res));
		return 1;
	}
	if (snd_ctl_elem_info_get_type(info) != SND_CTL_ELEM_TYPE_INTEGER) {
		fprintf(stderr, "numid=%ld is not an integer control\n", numid);
		return 1;
	}
	lo = snd_ctl_elem_info_get_min(info);
	hi = snd_ctl_elem_info_get_max(info);
	v = lo + (hi - lo) * percent / 100;

	printf("# %s numid=%ld range=%ld..%ld -> %ld (%ld%%)\n",
			card, numid, lo, hi, v, percent);

	/* Everything that can block has now blocked: handle open, element
	 * looked up, value computed. What is left is the sleep and the write,
	 * so the printed instants bracket the thing being timed and not the
	 * program's own startup. */
	if (delay_ms > 0)
		usleep((useconds_t) delay_ms * 1000);

	t_before = now_ns();
	for (int i = 0; i < snd_ctl_elem_info_get_count(info); i++)
		snd_ctl_elem_value_set_integer(val, i, v);
	snd_ctl_elem_value_set_id(val, id);
	res = snd_ctl_elem_write(ctl, val);
	t_after = now_ns();

	if (res < 0) {
		fprintf(stderr, "elem_write: %s\n", snd_strerror(res));
		return 1;
	}

	printf("t_before_ns=%" PRIu64 "\nt_after_ns=%" PRIu64 "\nwrite_us=%" PRIu64 "\n",
			t_before, t_after, (t_after - t_before) / 1000);
	snd_ctl_close(ctl);
	return 0;
}
