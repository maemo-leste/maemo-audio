/* Play a notification the way the real clients do: libcanberra, with the
 * sample cached in the server.
 *
 * paplay cannot stand in for this, for three reasons that each broke a
 * measurement in ways that looked like faults in the volume stack:
 *
 *   The absolute flag does not arrive on the ordinary path. libcanberra
 *   uploads the sample once and then plays it with
 *   pa_context_play_sample_with_proplist, and the server merges the
 *   sample's properties into the stream's at sample-play.c. A stream
 *   created without that merge carries no x-maemo.volume.absolute, so
 *   paplay --property=x-maemo.volume.absolute=1 exercises a path no
 *   notification ever takes.
 *
 *   paplay names every instance "paplay", and the restore cache keys on
 *   application name, so successive runs restored each other's volumes.
 *   The identity here is explicit and can be made unique per case.
 *
 *   --volume attenuates inside the stream node, upstream of the mixer,
 *   where forcing the mixer port to unity cannot undo it. Canberra's
 *   level is CA_PROP_CANBERRA_VOLUME, in dB, which is what MIS passes.
 *
 * The sink is chosen with PULSE_SINK in the environment.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <canberra.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s FILE [options]\n"
		"  -a            mark the sound absolutely played\n"
		"  -n NAME       application name (default cb-play)\n"
		"  -e EVENT      event id (default ui-popup-message)\n"
		"  -v DB         CA_PROP_CANBERRA_VOLUME, dB (default -25)\n"
		"  -r N          repeat N times, 1 s apart (default 1)\n"
		"  -c            do not cache the sample permanently\n",
		argv0);
}

int main(int argc, char **argv)
{
	const char *file = NULL;
	const char *app = "cb-play";
	const char *event = "ui-popup-message";
	const char *vol = "-25";
	int absolute = 0, repeat = 1, cache = 1;
	ca_context *ca = NULL;
	int rc, opt;
	int serve = 0;

	while ((opt = getopt(argc, argv, "san:e:v:r:ch")) != -1) {
		switch (opt) {
		case 'a': absolute = 1; break;
		case 's': serve = 1; break;
		case 'n': app = optarg; break;
		case 'e': event = optarg; break;
		case 'v': vol = optarg; break;
		case 'r': repeat = atoi(optarg); break;
		case 'c': cache = 0; break;
		default: usage(argv[0]); return 2;
		}
	}
	if (optind >= argc) {
		usage(argv[0]);
		return 2;
	}
	file = argv[optind];

	if ((rc = ca_context_create(&ca)) != 0) {
		fprintf(stderr, "ca_context_create: %s\n", ca_strerror(rc));
		return 1;
	}
	if ((rc = ca_context_open(ca)) != 0) {
		fprintf(stderr, "ca_context_open: %s\n", ca_strerror(rc));
		return 1;
	}

	/* Set after open: the context sends its properties on open, and
	 * the application name is what the restore cache keys on. */
	ca_context_change_props(ca,
			CA_PROP_APPLICATION_NAME, app,
			CA_PROP_APPLICATION_ID, "org.maemo.cbplay",
			CA_PROP_MEDIA_ROLE, absolute ? "Notification" : "Music",
			NULL);

	if (serve) {
		/* Resident mode: the pooled stream is created by the first
		 * play and kept, and each SIGUSR1 replays the cached sample
		 * through that same node. That lifetime -- not the sound --
		 * is what makes this look like MIS to the applet: a one-shot
		 * client's claim is born and dies with the sound, so it never
		 * exercises a pooled stream at all. */
		sigset_t set;
		sigemptyset(&set);
		sigaddset(&set, SIGUSR1);
		sigaddset(&set, SIGINT);
		sigaddset(&set, SIGTERM);
		sigprocmask(SIG_BLOCK, &set, NULL);
		printf("resident pid %ld; kill -USR1 %ld to click\n",
				(long)getpid(), (long)getpid());
		fflush(stdout);
		for (;;) {
			int sp = sigwaitinfo(&set, NULL);
			if (sp != SIGUSR1)
				break;
			/* The filename goes on every play, not just the
			 * first: there is nothing to replay without it. The
			 * pooling is the server's, not ours -- pipewire-pulse
			 * caches SAMPLE_PLAY under the sample name that
			 * libcanberra derives from the event id, so a
			 * constant event id is what makes the node get
			 * reused. Drop the id or vary it and each click
			 * becomes a fresh stream instead, which is the wrong
			 * lifetime to be testing. */
			rc = ca_context_play(ca, 0,
					CA_PROP_MEDIA_FILENAME, file,
					CA_PROP_MEDIA_NAME, event,
					CA_PROP_EVENT_ID, event,
					CA_PROP_CANBERRA_VOLUME, vol,
					"x-maemo.volume.absolute", "1",
					CA_PROP_CANBERRA_CACHE_CONTROL, "permanent",
					NULL);
			if (rc != 0)
				fprintf(stderr, "serve play: %s\n", ca_strerror(rc));
		}
		ca_context_destroy(ca);
		return 0;
	}

	while (repeat-- > 0) {
		printf("playing %s (%s%s)\n", file, app,
				absolute ? ", absolute" : "");
		rc = ca_context_play(ca, 0,
				CA_PROP_MEDIA_FILENAME, file,
				CA_PROP_MEDIA_NAME, event,
				CA_PROP_EVENT_ID, event,
				CA_PROP_CANBERRA_VOLUME, vol,
				absolute ? "x-maemo.volume.absolute" : "x-maemo.noop",
				absolute ? "1" : "0",
				cache ? CA_PROP_CANBERRA_CACHE_CONTROL : "x-maemo.noop",
				cache ? "permanent" : "0",
				NULL);
		if (rc != 0)
			fprintf(stderr, "ca_context_play: %s\n", ca_strerror(rc));
		if (repeat > 0)
			sleep(1);
	}

	/* The sample is played asynchronously; leaving here too early
	 * tears the context down mid-sound, which looks like a truncated
	 * notification rather than a short one. */
	usleep(200 * 1000);
	ca_context_destroy(ca);
	return 0;
}
