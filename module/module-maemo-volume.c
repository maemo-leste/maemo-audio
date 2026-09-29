/* Maemo volume: delayed route writer
 *
 * The WirePlumber policy computes the level the hardware should carry, but it
 * cannot time the write.  A route write lands in the codec immediately, while
 * the stream-side gain compensation that must accompany it is still travelling
 * through the client and the graph.  The mismatch is what makes music swell
 * under a key-press click.
 *
 * This module takes over the *execution* of the route write:
 *
 *   policy --metadata-->  x-maemo.route-volume  (in our own namespace)
 *   module --timer--->    pw_impl_device_set_param(Route, ...)
 *
 * The timer lives on the context data loop, so the write executes in the
 * graph driver thread rather than the main loop.  The delay is the live ALSA
 * ring-buffer fill read from /proc: that is exactly how long a sample written
 * into the ring right now waits before the DAC plays it.
 *
 * Why our own metadata namespace instead of the "default" one:
 *
 *   Adding a pw_impl_metadata listener to the daemon's "default" metadata
 *   corrupts the daemon -- it dies with a SIGSEGV on a NULL hook-list head
 *   inside pw_global_update_permissions() the next time a client connects.
 *   Owning the namespace ourselves is clean on both startup and teardown,
 *   and WirePlumber can still write into it: the remote proxy exposes
 *   wp_metadata_set(), surfaced to Lua as md:set(subject, key, type, value).
 *
 * Deliberately NOT done here:
 *  - no raw snd_ctl write.  We go through the Route param so device
 *    resolution, port selection, mute state and WirePlumber's per-port
 *    persistence all keep working, and so there is only ever one writer.
 *  - no delay when there is no running stream.  With nothing in the ring
 *    there is no uncompensated tail to wait out.
 *
 * Payload format (metadata value):
 *   "index=<N> devidx=<D> device=<G> vol=<f>[,<f>...] mute=<0|1> save=<0|1>"
 *
 *   device  = the global id of the pw_impl_device (what we find and write)
 *   devidx  = the card.profile.device index the route belongs to
 *   index   = the route index on that device
 *   mute    = carried through so handing the write to us does not drop the
 *             port's mute state
 *
 * Module config (key=value pairs in the module args):
 *   alsa.card      ALSA card index for the /proc fill read   (default 0)
 *   alsa.pcm       ALSA pcm device index                    (default 0)
 *   extra.ms       extra delay on top of the ring fill      (default 5)
 *   margin.ms      direction-aware bias                     (default 5)
 *   metadata.name  namespace to own                       (default x-maemo-volume)
 *   debug          1 = verbose logging                     (default 1)
 */
#define _GNU_SOURCE
#include <pipewire/pipewire.h>
#include <pipewire/global.h>
#include <pipewire/impl.h>
#include <pipewire/module.h>
#include <pipewire/data-loop.h>
#include <pipewire/loop.h>
#include <pipewire/extensions/metadata.h>

#include <spa/pod/pod.h>
#include <spa/pod/builder.h>
#include <spa/param/route.h>
#include <spa/param/props.h>

#include <pthread.h>
#include <sched.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define MAX_VOL 8
#define DEFAULT_METADATA_NAME "x-maemo-volume"
#define PAYLOAD_KEY "x-maemo.route-volume"

/* Private-but-exported accessors.  Declared here so the module builds against
 * the installed headers instead of inside the pipewire source tree. */
struct pw_impl_device;

/* Not in the installed impl-device.h, but exported by libpipewire. */
int pw_impl_device_set_param(struct pw_impl_device *device, uint32_t id,
			    uint32_t flags, const struct spa_pod *param);

struct impl {
	struct pw_context *ctx;
	struct pw_impl_module *module;
	struct pw_loop	*data_loop;
	struct spa_source *timer;

	struct pw_impl_metadata *metadata;
	struct spa_hook	 md_hook;
	struct spa_hook	 ctx_hook;
	struct spa_hook	 mod_hook;

	/* pending route write; newest replaces older */
	struct pw_impl_device *dev;
	uint32_t	 route_index;
	uint32_t	 dev_idx;
	float		 vols[MAX_VOL];
	uint32_t	 n_vols;
	bool		 save;
	bool		 mute;
	bool		 pending;
	uint64_t	 due_us;	/* when we meant to fire */
	uint64_t	 requested_us;	/* when the policy asked */
	float		 last_written;	/* for the direction-aware bias */
	bool		 have_last;

	/* config */
	int		 alsa_card;
	int		 alsa_pcm;
	int64_t		 extra_us;	/* extra delay beyond the fill */
	int64_t		 margin_us;	/* direction-aware bias */
	bool		 debug;

	unsigned	 writes;
};

static uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000000ull + ts.tv_nsec / 1000ull;
}

static float top_of(struct impl *impl)
{
	float t = 0.0f;
	for (uint32_t i = 0; i < impl->n_vols; i++)
		if (impl->vols[i] > t)
			t = impl->vols[i];
	return t;
}

static void thread_tag(char *out, size_t len)
{
	char name[32] = "?";
	int policy = -1;
	struct sched_param sp;
	const char *p = "OTHER";

	pthread_getname_np(pthread_self(), name, sizeof(name));
	pthread_getschedparam(pthread_self(), &policy, &sp);
	if (policy == SCHED_FIFO) p = "FIFO";
	else if (policy == SCHED_RR) p = "RR";

	snprintf(out, len, "%s/%s:%d", name, p, sp.sched_priority);
}

/* Read the live ALSA ring fill, in microseconds.
 *
 * This is a kernel quantity: how much audio sits between the write pointer
 * and the DAC.  Readable by any process, which is why we can get it here
 * when we cannot get it from the PipeWire API (SPA Latency/ProcessLatency
 * come back zeroed for this device). */
static int64_t read_fill_us(int card, int pcm)
{
	char path[128], line[256];
	long delay_frames = -1, rate = 0;
	bool first = true;
	FILE *f;

	snprintf(path, sizeof(path), "/proc/asound/card%d/pcm%dp/sub0/status",
		 card, pcm);
	f = fopen(path, "r");
	if (!f) {
		pw_log_warn("fill: cannot open %s (%m)", path);
		return -1;
	}
	while (fgets(line, sizeof(line), f)) {
		long v;
		if (first) {
			line[strcspn(line, "\n")] = 0;
			pw_log_info("fill: first line of %s is '%s'", path, line);
			first = false;
		}
		if (sscanf(line, " delay : %ld", &v) == 1)
			delay_frames = v;
	}
	fclose(f);

	if (delay_frames < 0) {
		pw_log_warn("fill: no 'delay' line in %s", path);
		return -1;
	}

	snprintf(path, sizeof(path), "/proc/asound/card%d/pcm%dp/sub0/hw_params",
		 card, pcm);
	f = fopen(path, "r");
	if (!f) {
		pw_log_warn("fill: cannot open %s (%m)", path);
		return -1;
	}
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, " rate : %d", (int *) &rate) == 1)
			break;
	}
	fclose(f);

	if (rate <= 0) {
		pw_log_warn("fill: no rate in %s", path);
		return -1;
	}
	pw_log_info("fill: %ld frames @ %d Hz", delay_frames, (int) rate);
	return (int64_t) delay_frames * 1000000 / rate;
}

/* Perform the actual route write.  Called from the data loop (driver thread). */
static int do_route_write(struct impl *impl)
{
	uint8_t buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	struct spa_pod_frame f[2];
	struct spa_pod *param;
	uint64_t t = now_us();
	int64_t late;
	char th[64];

	thread_tag(th, sizeof(th));

	spa_pod_builder_push_object(&b, &f[0],
			SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route);
	spa_pod_builder_add(&b,
			SPA_PARAM_ROUTE_index,  SPA_POD_Int(impl->route_index),
			SPA_PARAM_ROUTE_device, SPA_POD_Int(impl->dev_idx),
			0);
	spa_pod_builder_prop(&b, SPA_PARAM_ROUTE_props, 0);
	spa_pod_builder_push_object(&b, &f[1],
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_add(&b,
			SPA_PROP_channelVolumes,
			SPA_POD_Array(sizeof(float), SPA_TYPE_Float,
				      impl->n_vols, impl->vols), 0);
	spa_pod_builder_prop(&b, SPA_PROP_mute, 0);
	spa_pod_builder_bool(&b, impl->mute);
	spa_pod_builder_pop(&b, &f[1]);
	if (impl->save) {
		spa_pod_builder_prop(&b, SPA_PARAM_ROUTE_save, 0);
		spa_pod_builder_bool(&b, true);
	}
	param = spa_pod_builder_pop(&b, &f[0]);

	late = (int64_t) t - (int64_t) impl->due_us;

	if (pw_impl_device_set_param(impl->dev, SPA_PARAM_Route, 0, param) < 0) {
		pw_log_error("route write failed dev=%p index=%u",
			     impl->dev, impl->route_index);
		impl->pending = false;
		return -1;
	}

	impl->writes++;
	impl->last_written = top_of(impl);
	impl->have_last = true;
	impl->pending = false;

	if (impl->debug)
		pw_log_info("WROTE route idx=%u vol[0]=%.4f on %s  "
			    "late=%+lld ms (total since request %.1f ms)",
			    impl->route_index,
			    impl->n_vols ? impl->vols[0] : 0.0f, th,
			    (long long) late / 1000,
			    (double) (t - impl->requested_us) / 1000.0);

	return 0;
}

/* Data-loop timer callback: runs in the graph driver thread. */
static void on_timer(void *data, uint64_t expirations)
{
	struct impl *impl = data;
	(void) expirations;
	do_route_write(impl);
}

static struct pw_impl_device *find_device(struct pw_context *ctx, uint32_t id)
{
	struct pw_global *g = pw_context_find_global(ctx, id);
	if (!g)
		return NULL;
	if (!pw_global_is_type(g, PW_TYPE_INTERFACE_Device))
		return NULL;
	return (struct pw_impl_device *) pw_global_get_object(g);
}

/* The ALSA device knows which card and substream it drives, so prefer that
 * over the configured numbers -- then the module needs no per-device config
 * for the common case.  Config still wins when the device does not say. */
static void fill_from_device(struct impl *impl, struct pw_impl_device *dev)
{
	const struct pw_properties *p = pw_impl_device_get_properties(dev);
	const char *v;

	if (!p)
		return;

	v = pw_properties_get(p, "alsa.card");
	if (v && *v) {
		/* may be a card index or a card name */
		char *end = NULL;
		long n = strtol(v, &end, 10);
		if (end && *end == 0)
			impl->alsa_card = (int) n;
	}

	v = pw_properties_get(p, "alsa.device");
	if (v && *v)
		impl->alsa_pcm = atoi(v);
}

/* metadata property callback -- runs on the core (main) loop.
 *
 * We read /proc here rather than in the driver thread: the fill drifts slowly
 * enough that taking the syscall off the RT thread costs nothing, and it
 * keeps the driver callback free of blocking I/O. */
static int on_metadata_property(void *data, uint32_t subject,
			       const char *key, const char *type,
			       const char *value)
{
	struct impl *impl = data;
	(void) type;
	int64_t fill_us;
	uint32_t idx = 0, dev_id = 0, dev_idx = 0, n = 0;
	float top = 0.0f, v[MAX_VOL];
	bool save = false, mute = false;
	char buf[256], th[64];
	char *tok, *s;
	int64_t delay_us;
	struct pw_impl_device *dev;

	if (!key || strcmp(key, PAYLOAD_KEY) != 0)
		return 0;
	if (!value)
		return 0;

	thread_tag(th, sizeof(th));
	if (impl->debug)
		pw_log_info("request on %s: %s", th, value);

	/* parse "index=N device=D vol=f[,f...] save=0|1" */
	snprintf(buf, sizeof(buf), "%s", value);
	s = buf;
	while ((tok = strsep(&s, " ")) != NULL) {
		if (*tok == 0)
			continue;
		if (sscanf(tok, "index=%u", &idx) == 1)
			continue;
		if (sscanf(tok, "devidx=%u", &dev_idx) == 1)
			continue;
		if (sscanf(tok, "device=%u", &dev_id) == 1)
			continue;
		if (strcmp(tok, "save=1") == 0) { save = true; continue; }
		if (strcmp(tok, "mute=1") == 0) { mute = true; continue; }
		if (strcmp(tok, "mute=0") == 0) { mute = false; continue; }
		if (strncmp(tok, "vol=", 4) == 0) {
			char *p = tok + 4, *t2;
			while ((t2 = strsep(&p, ",")) != NULL && n < MAX_VOL) {
				if (*t2 == 0)
					continue;
				v[n++] = strtof(t2, NULL);
			}
			continue;
		}
		pw_log_warn("unrecognised token '%s'", tok);
	}

	if (n == 0) {
		pw_log_warn("no volumes in payload");
		return 0;
	}

	if (dev_id == 0)
		dev_id = subject;

	dev = find_device(impl->ctx, dev_id);
	if (dev == NULL) {
		pw_log_warn("device %u not found or not a Device", dev_id);
		return 0;
	}

	for (uint32_t i = 0; i < n; i++)
		if (v[i] > top)
			top = v[i];

	impl->dev = dev;
	impl->route_index = idx;
	impl->dev_idx = dev_idx;
	memcpy(impl->vols, v, sizeof(float) * n);
	impl->n_vols = n;
	impl->save = save;
	impl->mute = mute;
	impl->requested_us = now_us();

	fill_from_device(impl, dev);

	fill_us = read_fill_us(impl->alsa_card, impl->alsa_pcm);

	/* Nothing running: no uncompensated tail in the ring, so there is
	 * nothing to wait out.  Write straight away. */
	if (fill_us <= 0) {
		if (impl->debug)
			pw_log_info("sink idle (fill=%" PRId64 " us) -> immediate write",
				    fill_us);
		impl->due_us = now_us();
		do_route_write(impl);
		return 0;
	}

	/* Nokia's direction bias: going up we land late (a brief dip beats a
	 * swell), going down we land early (same reason).  Never louder than
	 * what was asked for. */
	delay_us = fill_us + impl->extra_us;
	if (impl->have_last) {
		if (top > impl->last_written)
			delay_us += impl->margin_us;
		else if (top < impl->last_written)
			delay_us -= impl->margin_us;
	}
	if (delay_us < 0)
		delay_us = 0;

	impl->due_us = now_us() + (uint64_t) delay_us;
	impl->pending = true;

	struct timespec tv = {
		.tv_sec  = delay_us / 1000000,
		.tv_nsec = (delay_us % 1000000) * 1000,
	};

	if (impl->debug)
		pw_log_info("armed for %" PRId64 " ms (fill=%" PRId64
			    " + extra=%" PRId64 " ms, bias=%s)",
			    delay_us / 1000, fill_us / 1000,
			    impl->extra_us / 1000,
			    !impl->have_last ? "none" :
			    (top > impl->last_written ? "up/late" :
			     (top < impl->last_written ? "down/early" : "same")));

	pw_loop_update_timer(impl->data_loop, impl->timer, &tv, NULL, false);
	return 0;
}

static const struct pw_impl_metadata_events md_events = {
	PW_VERSION_IMPL_METADATA_EVENTS,
	.property = on_metadata_property,
};

/* Tear everything down in the reverse order we built it.  Without this the
 * module takes the daemon down with it during context teardown: a SIGSEGV in
 * pw_global_destroy() called from pw_impl_module_destroy(). */
static void on_module_destroy(void *data)
{
	struct impl *impl = data;

	if (impl->timer) {
		pw_loop_update_timer(impl->data_loop, impl->timer,
				    NULL, NULL, true);
		pw_loop_remove_source(impl->data_loop, impl->timer);
		impl->timer = NULL;
	}

	spa_hook_remove(&impl->ctx_hook);

	if (impl->metadata) {
		spa_hook_remove(&impl->md_hook);
		pw_impl_metadata_destroy(impl->metadata);
		impl->metadata = NULL;
	}

	pw_log_info("torn down (%u route writes performed)", impl->writes);
}

static const struct pw_impl_module_events mod_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = on_module_destroy,
};

/* Our drop-in can load before libpipewire-module-metadata, so watch the
 * context.  We only ever create our own namespace; we never attach to a
 * foreign metadata object. */
static void on_global_added(void *data, struct pw_global *global)
{
	(void) data;
	(void) global;
}

static const struct pw_context_events ctx_events = {
	PW_VERSION_CONTEXT_EVENTS,
	.global_added = on_global_added,
};

static void apply_config(struct impl *impl, const char *key, const char *val)
{
	long n = val ? atol(val) : 0;

	if (!strcmp(key, "alsa.card"))
		impl->alsa_card = (int) n;
	else if (!strcmp(key, "alsa.pcm"))
		impl->alsa_pcm = (int) n;
	else if (!strcmp(key, "extra.ms"))
		impl->extra_us = n * 1000;
	else if (!strcmp(key, "margin.ms"))
		impl->margin_us = n * 1000;
	else if (!strcmp(key, "debug"))
		impl->debug = (n != 0);
	else
		pw_log_warn("ignoring unknown config key '%s'", key);
}

/* pipewire hands us the config block verbatim, so "key = value" arrives with
 * whitespace around the '=' and wrapped in braces.  Glue the '=' back onto
 * its key and value so a plain whitespace split works. */
static char *normalize_args(const char *args)
{
	size_t n = strlen(args);
	char *out = malloc(n + 1);
	size_t o = 0;
	const char *p = args;

	if (!out)
		return NULL;

	while (*p) {
		if (*p == '=') {
			while (o > 0 && (out[o - 1] == ' '  || out[o - 1] == '\t' ||
					 out[o - 1] == '\n' || out[o - 1] == '\r'))
				o--;
			out[o++] = '=';
			p++;
			while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
				p++;
		} else {
			out[o++] = *p++;
		}
	}
	out[o] = 0;
	return out;
}

int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *ctx = pw_impl_module_get_context(module);
	struct impl *impl;
	struct pw_loop *dl;
	struct pw_impl_metadata *md;
	char th[64];
	const char *md_name = DEFAULT_METADATA_NAME;

	thread_tag(th, sizeof(th));
	pw_log_info("init: context=%p (thread %s)", ctx, th);

	impl = calloc(1, sizeof(*impl));
	if (!impl)
		return -ENOMEM;

	impl->ctx = ctx;
	impl->module = module;
	impl->alsa_card = 0;
	impl->alsa_pcm = 0;
	impl->extra_us = 5000;
	impl->margin_us = 5000;
	impl->debug = true;

	/* parse "key=value key=value ..." */
	if (args && *args) {
		char *dup = normalize_args(args), *tok, *p = dup;
		while (p && (tok = strsep(&p, " \t\n\r")) != NULL) {
			char *eq;
			if (!*tok || !strcmp(tok, "{") || !strcmp(tok, "}"))
				continue;
			eq = strchr(tok, '=');
			if (!eq) {
				pw_log_warn("ignoring malformed arg '%s'", tok);
				continue;
			}
			*eq = 0;
			if (!strcmp(tok, "metadata.name"))
				md_name = eq + 1;
			else
				apply_config(impl, tok, eq + 1);
		}
		free(dup);
	}

	dl = pw_data_loop_get_loop(pw_context_get_data_loop(ctx));
	if (!dl) {
		pw_log_error("no data loop");
		free(impl);
		return -ENODEV;
	}
	impl->data_loop = dl;
	pw_log_info("data loop = %p name=%s", dl, dl->name ? dl->name : "-");

	impl->timer = pw_loop_add_timer(dl, on_timer, impl);
	if (!impl->timer) {
		pw_log_error("no timer");
		free(impl);
		return -ENOMEM;
	}
	pw_log_info("timer source = %p", impl->timer);

	pw_context_add_listener(ctx, &impl->ctx_hook, &ctx_events, impl);

	/* Own our namespace.  Do this before registering so the listener is in
	 * place before any client can bind and write. */
	md = pw_context_create_metadata(ctx, md_name,
				      pw_properties_new(NULL, NULL), 0);
	if (!md) {
		pw_log_error("could not create metadata '%s'", md_name);
		return -ENOMEM;
	}
	impl->metadata = md;

	pw_impl_metadata_add_listener(md, &impl->md_hook, &md_events, impl);

	if (pw_impl_metadata_register(md, NULL) < 0) {
		pw_log_error("could not register metadata '%s'", md_name);
		return -EIO;
	}

	pw_impl_module_add_listener(module, &impl->mod_hook, &mod_events, impl);

	pw_log_info("ready: owns metadata '%s' card=%d pcm=%d "
		    "extra=%" PRId64 "ms margin=%" PRId64 "ms",
		    md_name, impl->alsa_card, impl->alsa_pcm,
		    impl->extra_us / 1000, impl->margin_us / 1000);
	return 0;
}
