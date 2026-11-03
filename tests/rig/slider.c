/* slider -- what the status-menu applet does, without the status menu.
 *
 * The applet is the only thing that moves the hardware in this design, and
 * it is also the only thing the test rig cannot drive: it needs a finger on
 * a volume key. That made every timing test depend on someone being at the
 * machine, and worse, it made the starting condition whatever the last
 * person left -- a slider at the top turns a claim into a no-op, so a whole
 * run can measure nothing and report success.
 *
 * So this is the applet's contract, and nothing else:
 *
 *   - It reads the level from the sink node's x-maemo.volume.target, which
 *     is what the module publishes and which the module itself derives
 *     from the hardware when nothing else is known. Starting here means
 *     this tool and the real applet agree about where the slider is,
 *     including after a restart.
 *
 *   - It writes x-maemo.volume.target into the metadata named "default",
 *     which is where the module listens. The value is a perceptual step
 *     on 0..65536 whose cube is the linear level -- Fremantle's law, and
 *     the module's reread_target() is the authority for it.
 *
 *   - It moves in rungs of the announced ladder, because that is what a
 *     keypress is: one rung. The ladder is read from the node rather than
 *     hardcoded, so a re-tuned card moves this tool too.
 *
 * Everything else -- ducking, claims, when the codec moves -- is the
 * module's business and deliberately not modelled here.
 *
 *   slider                    print where things stand
 *   slider -u / -d            one rung up / down
 *   slider -s 7               straight to a rung
 *   slider -u -n 5 -i 120     five keypresses 120 ms apart (a key hold)
 *
 * The level is read from the bound node's info rather than from the
 * registry's global announcement: the module publishes the ladder and the
 * target with update_properties() after the node was exported, and an
 * update like that reaches a client as node info, not as a new global.
 * Reading globals alone therefore sees a sink with no ladder at all.
 */#include <math.h>
#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <pipewire/proxy.h>
#include <spa/utils/dict.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TARGET_KEY	"x-maemo.volume.target"
#define TUNING_KEY	"x-maemo.volume.tuning"
#define MD_NAME		"default"

#define MAX_RUNGS	64
#define MAX_SINKS	32

struct sink {
	struct pw_node *node;
	struct spa_hook hook;
};

struct app {
	struct pw_main_loop *loop;
	struct pw_context *ctx;
	struct pw_core *core;
	struct pw_registry *reg;
	struct spa_hook reg_hook;

	struct pw_metadata *md;		/* the one named "default" */
	struct spa_hook md_hook;

	struct sink sink[MAX_SINKS];
	int n_sinks;

	uint32_t sink_id;			/* the one with a ladder */
	char *sink_name;
	float rung_amp[MAX_RUNGS];
	int n_rungs;
	long published;
	bool have_published;
};

static void msleep(int ms)
{
	struct timespec ts = { .tv_sec = ms / 1000,
			       .tv_nsec = (long) (ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

/* One turn of the loop, with a bound. Blocking forever is how a tool meant
 * to take a reading ends up needing a Ctrl-C: if nothing else is happening
 * on the socket there is nothing to deliver. */
static void spin(struct app *a, int timeout_ms)
{
	pw_loop_iterate(pw_main_loop_get_loop(a->loop), timeout_ms);
}

/* The perceptual step the module expects: the cube root of the linear
 * level, on 0..65536. Inverse of the cube it applies on the way in. */
static long perceptual(float amp)
{
	if (amp < 0.0f) amp = 0.0f;
	if (amp > 1.0f) amp = 1.0f;
	return (long) (cbrt((double) amp) * 65536.0 + 0.5);
}

static float linear(long raw)
{
	double n = (double) raw / 65536.0;
	if (n < 0.0) n = 0.0;
	if (n > 1.0) n = 1.0;
	return (float) (n * n * n);
}

/* "-4000,-3800,...,0" in centibels, ascending, the last one the top of
 * the ladder. Read from the node so a re-tuned card moves this tool. */
static int parse_tuning(const char *csv, float *amp_out, int max)
{
	int n = 0;
	const char *p = csv;

	while (p && *p && n < max) {
		char *end = NULL;
		long cd = strtol(p, &end, 10);
		if (end == p)
			break;
		amp_out[n++] = (float) pow(10.0, (double) cd / 2000.0);
		p = strchr(p, ',');
		if (p)
			p++;
	}
	return n;
}

/* Which rung a level sits on. Nearest in decibels, because that is how
 * the ear -- and the module's own step_for_amp() -- decides. */
static int rung_for_amp(const float *ladder, int n, float amp)
{
	int best = 0;
	double best_db = 1e9;

	for (int i = 0; i < n; i++) {
		double db = fabs(20.0 * log10((double) amp / (double) ladder[i]));
		if (db < best_db) {
			best_db = db;
			best = i;
		}
	}
	return best;
}

static void print_state(struct app *a, int rung)
{
	printf("sink %u (%s)\n", a->sink_id, a->sink_name ? a->sink_name : "?");
	printf("  ladder   %d rungs, %.4f .. %.4f\n", a->n_rungs,
			a->n_rungs ? a->rung_amp[0] : 0.0f,
			a->n_rungs ? a->rung_amp[a->n_rungs - 1] : 0.0f);
	if (a->have_published)
		printf("  published target %ld -> amp %.4f\n",
				a->published, linear(a->published));
	if (rung >= 0)
		printf("  rung %2d  amp %.4f  %+.1f dB  perceptual %ld\n",
				rung, a->rung_amp[rung],
				20.0 * log10((double) a->rung_amp[rung]),
				perceptual(a->rung_amp[rung]));
}

static int write_target(struct app *a, int rung)
{
	char text[32];

	if (a->md == NULL) {
		fprintf(stderr, "no metadata named '%s' -- the module "
				"listens there, so nothing would read this\n",
				MD_NAME);
		return -1;
	}
	snprintf(text, sizeof(text), "%ld", perceptual(a->rung_amp[rung]));
	if (pw_metadata_set_property(a->md, a->sink_id, TARGET_KEY,
			"Spa:Long", text) < 0) {
		fprintf(stderr, "could not write %s\n", TARGET_KEY);
		return -1;
	}
	printf("wrote rung %2d  amp %.4f  %+.1f dB  -> %s\n", rung,
			a->rung_amp[rung],
			20.0 * log10((double) a->rung_amp[rung]), text);
	fflush(stdout);
	return 0;
}

static int on_md_property(void *data, uint32_t subject, const char *key,
		const char *type, const char *value)
{
	(void) data; (void) subject; (void) type;

	if (key == NULL || value == NULL)
		return 0;
	if (strcmp(key, TARGET_KEY) != 0)
		return 0;
	/* The module echoes the level it actually landed on. Printing it is
	 * what distinguishes "the write went out" from "the level moved". */
	printf("  <- %s = %s\n", key, value);
	fflush(stdout);
	return 0;
}

static const struct pw_metadata_events md_events = {
	PW_VERSION_METADATA_EVENTS,
	.property = on_md_property,
};

/* The ladder and the published level arrive here, not in the global. */
static void on_node_info(void *data, const struct pw_node_info *info)
{
	struct app *a = data;
	const char *tuning, *target;

	if (info == NULL || info->props == NULL)
		return;

	tuning = spa_dict_lookup(info->props, TUNING_KEY);
	target = spa_dict_lookup(info->props, TARGET_KEY);

	if (getenv("SLIDER_DEBUG"))
		fprintf(stderr, "  sink %u %s tuning=%s target=%s\n", info->id,
			spa_dict_lookup(info->props, "node.name"),
			tuning ? "yes" : "no", target ? target : "-");

	if (tuning == NULL)
		return;		/* not the mixer: no ladder, no target */

	a->sink_id = info->id;
	free(a->sink_name);
	a->sink_name = NULL;
	{
		const char *n = spa_dict_lookup(info->props, "node.name");
		if (n)
			a->sink_name = strdup(n);
	}
	a->n_rungs = parse_tuning(tuning, a->rung_amp, MAX_RUNGS);
	if (target) {
		a->published = strtol(target, NULL, 10);
		a->have_published = true;
	}
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.info = on_node_info,
};

static void on_global(void *data, uint32_t id, uint32_t permissions,
		const char *type, uint32_t version,
		const struct spa_dict *props)
{
	struct app *a = data;
	const char *s;

	(void) permissions;

	if (type && strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
		s = props ? spa_dict_lookup(props, "metadata.name") : NULL;
		if (s && strcmp(s, MD_NAME) == 0 && a->md == NULL) {
			a->md = pw_registry_bind(a->reg, id, type, version, 0);
			if (a->md)
				pw_metadata_add_listener(a->md, &a->md_hook,
						&md_events, a);
			else
				fprintf(stderr, "could not bind '%s'\n", MD_NAME);
		}
		return;
	}

	if (!type || strcmp(type, PW_TYPE_INTERFACE_Node) != 0 || !props)
		return;
	s = spa_dict_lookup(props, "media.class");
	if (!s || strcmp(s, "Audio/Sink") != 0)
		return;
	if (a->n_sinks == MAX_SINKS)
		return;

	{
		struct pw_node *n = pw_registry_bind(a->reg, id, type, version, 0);
		if (n == NULL)
			return;
		a->sink[a->n_sinks].node = n;
		pw_node_add_listener(n, &a->sink[a->n_sinks].hook,
				&node_events, a);
		a->n_sinks++;
	}
}

static const struct pw_registry_events reg_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = on_global,
};

int main(int argc, char *argv[])
{
	struct app a = { 0 };
	int opt, set = -1, delta = 0, repeat = 1, interval = 0;

	pw_init(&argc, &argv);

	while ((opt = getopt(argc, argv, "s:udn:i:h")) != -1) {
		switch (opt) {
		case 's': set = atoi(optarg); break;
		case 'u': delta += 1; break;
		case 'd': delta -= 1; break;
		case 'n': repeat = atoi(optarg); break;
		case 'i': interval = atoi(optarg); break;
		default:
			fprintf(stderr,
				"usage: slider [-s N | -u | -d] [-n N] [-i ms]\n");
			return 2;
		}
	}

	if ((a.loop = pw_main_loop_new(NULL)) == NULL) {
		fprintf(stderr, "no main loop\n");
		return 1;
	}
	a.ctx = pw_context_new(pw_main_loop_get_loop(a.loop), NULL, 0);
	if (!a.ctx) {
		fprintf(stderr, "could not connect: is pipewire running?\n");
		return 1;
	}
	a.core = pw_context_connect(a.ctx, NULL, 0);
	if (!a.core) {
		fprintf(stderr, "could not connect to the daemon\n");
		return 1;
	}
	a.reg = pw_core_get_registry(a.core, PW_VERSION_REGISTRY, 0);
	pw_registry_add_listener(a.reg, &a.reg_hook, &reg_events, &a);

	/* Globals, and then each bound node's info, all arrive as events.
	 * Two seconds is generous for a local socket and finite, which is
	 * the property that matters for a tool run from a script. */
	for (int i = 0; i < 200 && (a.md == NULL || a.n_rungs == 0); i++)
		spin(&a, 10);

	if (a.n_rungs == 0) {
		fprintf(stderr, "no sink with a " TUNING_KEY " -- the mixer "
				"is not running, or has not been announced yet\n");
		return 1;
	}

	int here = a.have_published ? rung_for_amp(a.rung_amp, a.n_rungs,
			linear(a.published)) : a.n_rungs - 1;

	if (delta == 0 && set < 0) {
		print_state(&a, here);
		return 0;
	}

	int to = set >= 0 ? set : here + delta;
	if (to < 0) to = 0;
	if (to > a.n_rungs - 1) to = a.n_rungs - 1;

	for (int i = 0; i < repeat; i++) {
		if (write_target(&a, to) < 0)
			return 1;
		if (i + 1 < repeat)
			msleep(interval);
	}

	/* Let the echo come back so the round trip is visible rather than
	 * assumed: the write leaving this process is not the level moving. */
	for (int i = 0; i < 50; i++)
		spin(&a, 10);
	return 0;
}
