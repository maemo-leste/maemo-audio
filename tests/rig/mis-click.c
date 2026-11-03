/* mis-click -- a resident stand-in for maemo-input-sounds.
 *
 * The point of this tool is not the sound, it is the lifetime of the stream
 * that carries it. MIS loads each UI sample once and libcanberra keeps the
 * SAMPLE_PLAY stream alive in pipewire-pulse's permanent cache, so the node
 * exists for the whole session and every key press only moves it between
 * running and idle. A tool that opens a fresh stream per click -- pw-play, or
 * a one-shot canberra client -- exercises a completely different path: the
 * claim there is born and dies with the sound, and the pooled stream, which
 * is what the applet actually has to reason about, never appears.
 *
 * So: start this once, and fire SIGUSR1 to make a click. The node is created
 * at startup and reused forever, exactly as on the device.
 *
 *   mis-click [-f file] [-e event-id] [-v dB] [-i auto-repeat-ms] [-n count]
 *   kill -USR1 <pid>        make one click
 *   kill -USR2 <pid>        report what it thinks it is holding
 */

#include <canberra.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static ca_context *ctx;
static const char *event_id = "ui-key-press";
/* libcanberra's pulse driver parses this with strtod and rejects anything
 * with trailing text: "-5.0 dB" yields EINVAL, "-5.0" is what it wants.
 * The unit is dB by definition, so the suffix was never meaningful. */
static const char *volume = "-5.0";
static const char *file = "/usr/share/sounds/ui-key_press.wav";
static unsigned long fired;

static void
stamp(const char *what)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	fprintf(stderr, "[%ld.%06ld] mis-click: %s\n",
		(long)ts.tv_sec % 100000, (long)(ts.tv_nsec / 1000), what);
}

/* Play the already-cached event. No filename: libcanberra replays what it
 * cached under this event id, which is what makes the stream pooled rather
 * than per-click. */
static void
click(void)
{
	int res;

	fired++;
	stamp("play");
	res = ca_context_play(ctx, 0,
			CA_PROP_EVENT_ID, event_id,
			CA_PROP_EVENT_DESCRIPTION, "Key press",
			CA_PROP_MEDIA_ROLE, "Notification",
			CA_PROP_CANBERRA_VOLUME, volume,
			CA_PROP_CANBERRA_CACHE_CONTROL, "permanent",
			"x-maemo.volume.absolute", "1",
			NULL);
	if (res != 0)
		fprintf(stderr, "mis-click: play failed: %s\n", ca_strerror(res));
}

int
main(int argc, char *argv[])
{
	sigset_t set;
	int opt, res;
	int repeat_ms = 0, count = -1;

	while ((opt = getopt(argc, argv, "f:e:v:i:n:h")) != -1) {
		switch (opt) {
		case 'f': file = optarg; break;
		case 'e': event_id = optarg; break;
		case 'v': volume = optarg; break;
		case 'i': repeat_ms = atoi(optarg); break;
		case 'n': count = atoi(optarg); break;
		default:
			fprintf(stderr,
	"usage: mis-click [-f file] [-e event-id] [-v dB] [-i ms] [-n count]\n"
	"       kill -USR1 <pid> to click, -USR2 to report\n");
			return 2;
		}
	}

	/* Blocked, then waited on explicitly: no handler running asynchronously
	 * in the middle of a play call. */
	sigemptyset(&set);
	sigaddset(&set, SIGUSR1);
	sigaddset(&set, SIGUSR2);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGTERM);
	sigprocmask(SIG_BLOCK, &set, NULL);

	res = ca_context_create(&ctx);
	if (res != 0) {
		fprintf(stderr, "mis-click: create failed: %s\n", ca_strerror(res));
		return 1;
	}
	res = ca_context_open(ctx);
	if (res != 0) {
		fprintf(stderr, "mis-click: open failed: %s\n", ca_strerror(res));
		return 1;
	}

	ca_context_change_props(ctx,
			CA_PROP_APPLICATION_NAME, "maemo-input-sounds",
			CA_PROP_APPLICATION_ID, "com.nokia.maemo-input-sounds",
			CA_PROP_MEDIA_ROLE, "Notification",
			NULL);

	/* Prime the cache once. This is the call that creates the pooled
	 * stream; everything after it reuses that node. */
	{
		struct timespec ts = { 0, 200 * 1000 * 1000 };
		stamp("prime");
		res = ca_context_play(ctx, 0,
				CA_PROP_MEDIA_FILENAME, file,
				CA_PROP_MEDIA_NAME, event_id,
				CA_PROP_EVENT_ID, event_id,
				CA_PROP_CANBERRA_VOLUME, volume,
				"x-maemo.volume.absolute", "1",
				CA_PROP_CANBERRA_CACHE_CONTROL, "permanent",
				NULL);
		if (res != 0) {
			fprintf(stderr, "mis-click: prime failed: %s\n",
				ca_strerror(res));
			return 1;
		}
		nanosleep(&ts, NULL);
	}

	printf("mis-click pid %ld holding event '%s' from %s at %s\n",
		(long)getpid(), event_id, file, volume);
	fflush(stdout);

	while (count != 0) {
		siginfo_t si;
		int s;

		if (repeat_ms > 0) {
			struct timespec ts = {
				repeat_ms / 1000,
				(repeat_ms % 1000) * 1000 * 1000 };
			s = sigtimedwait(&set, &si, &ts);
			if (s < 0 && errno == EAGAIN) {
				click();
				if (count > 0)
					count--;
				continue;
			}
		} else {
			s = sigwaitinfo(&set, &si);
		}

		if (s < 0)
			continue;

		if (si.si_signo == SIGUSR1) {
			click();
			if (count > 0)
				count--;
		} else if (si.si_signo == SIGUSR2) {
			stamp("report");
			fprintf(stderr, "mis-click: %lu clicks fired\n", fired);
		} else {
			stamp("exit");
			break;
		}
	}

	ca_context_destroy(ctx);
	return 0;
}
