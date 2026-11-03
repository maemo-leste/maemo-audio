/* Drive the click path the way maemo-input-sounds does, in one process.
 *
 * The point of this is not the sound, it is the shape of the traffic. A
 * fresh process per click makes a fresh connection, and the pooled
 * SAMPLE_PLAY stream that libcanberra's "permanent" cache control
 * produces never appears -- which is the path the applet actually takes
 * and the one that used to leave the codec pinned at the top of the
 * ladder. So: one context, opened once, and the bursts played through
 * it without exiting between them.
 *
 * Props mirror mis-sound.c exactly: the absolute flag, the permanent
 * cache control, and the autorepeat rule that drops the level by 30 dB
 * when two presses are 100 ms or less apart.
 *
 *   ./clickburst            default: 3 bursts of 3 at 1 s, then 3 at 100 ms
 *   ./clickburst -n 4 -i 1000 -b 3 -r 100
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <canberra.h>

static void sleep_ms(long ms)
{
	struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* One press, with MIS's autorepeat applied by the caller. */
static void press(ca_context *ctx, const char *volume, long long at_ms)
{
	int err;

	err = ca_context_play(ctx, 0,
			      CA_PROP_MEDIA_FILENAME,
			      "/usr/share/sounds/ui-key_press.wav",
			      CA_PROP_MEDIA_NAME, "x-maemo-key-pressed",
			      CA_PROP_EVENT_ID, "x-maemo-key-pressed",
			      CA_PROP_CANBERRA_VOLUME, volume,
			      "x-maemo.volume.absolute", "1",
			      CA_PROP_CANBERRA_CACHE_CONTROL, "permanent",
			      NULL);
	printf("  press at +%lld ms  vol=%s%s\n", at_ms, volume,
	       err ? "  (FAILED)" : "");
	fflush(stdout);
}

int main(int argc, char **argv)
{
	/* MIS's own fallback when no per-event level is configured. */
	const char *base_volume = "-25";
	int bursts = 3, per_burst = 3, repeat_ms = 100;
	long burst_interval_ms = 1000;
	ca_context *ctx = NULL;
	int err, i, b;
	long long t0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-n") && i + 1 < argc) bursts = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-b") && i + 1 < argc) per_burst = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-i") && i + 1 < argc)
			burst_interval_ms = atol(argv[++i]);
		else if (!strcmp(argv[i], "-r") && i + 1 < argc) repeat_ms = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-v") && i + 1 < argc) base_volume = argv[++i];
	}

	err = ca_context_create(&ctx);
	if (err) { fprintf(stderr, "ca_context_create: %s\n", ca_strerror(err)); return 1; }

	/* The driver has to be pinned before open, same as MIS: without it
	 * libcanberra probes and may pick something else, and then the
	 * absolute flag never reaches the volume policy. */
	ca_context_set_driver(ctx, "pulse");

	err = ca_context_open(ctx);
	if (err) { fprintf(stderr, "ca_context_open: %s\n", ca_strerror(err)); return 1; }

	t0 = now_ms();
	printf("phase 1: %d bursts of %d consecutive clicks, %ld ms apart\n",
	       bursts, per_burst, burst_interval_ms);

	for (b = 0; b < bursts; b++) {
		int k;

		for (k = 0; k < per_burst; k++) {
			char vol[16];

			/* MIS's autorepeat: a press within 100 ms of the
			 * last is 30 dB quieter, so a fast repeat is not
			 * simply the same click louder and louder. */
			snprintf(vol, sizeof(vol), "%d",
				 atoi(base_volume) - (k ? 30 : 0));
			press(ctx, vol, now_ms() - t0);
			if (k + 1 < per_burst)
				sleep_ms(20);	/* consecutive, not spaced */
		}
		if (b + 1 < bursts)
			sleep_ms(burst_interval_ms - (per_burst - 1) * 20);
	}

	printf("phase 2: %d clicks, %d ms apart (autorepeat applies)\n",
	       per_burst, repeat_ms);
	for (i = 0; i < per_burst; i++) {
		char vol[16];

		snprintf(vol, sizeof(vol), "%d", atoi(base_volume) - 30);
		press(ctx, vol, now_ms() - t0);
		if (i + 1 < per_burst)
			sleep_ms(repeat_ms);
	}

	/* Stay open so the pooled stream is still alive when the last
	 * release is written; tearing the context down here would withdraw
	 * the claim through the teardown path instead of the state edge. */
	printf("holding 2 s with the context still open, then closing\n");
	fflush(stdout);
	sleep_ms(2000);

	ca_context_destroy(ctx);
	return 0;
}
