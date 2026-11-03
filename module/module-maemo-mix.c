/* The control side of the maemo mixer.
 *
 * The division of labour, in one sentence: the mixer mixes and the
 * hardware sets the level.
 *
 * The mixer sums its inputs and applies one software gain to the result.
 * That gain is kept at unity. It is not dead code and it is not to be
 * removed: it is the place a software residual will live when flat volume
 * comes back, and taking it out would mean rebuilding the path from
 * nothing rather than pointing it at something other than 1.0.
 *
 * The user's slider drives the codec. The chain is
 *
 *   applet writes x-maemo.volume.target (0..65536, cubic)
 *     -> amplitude
 *     -> rung of the device's ladder
 *     -> the hardware step that carries that rung
 *     -> that step's amplitude, written to the ALSA route
 *
 * so the loudness the user sets is the loudness the amplifier produces,
 * with nothing multiplied in software on the way. Writing the route is
 * also what persists the level per device: the route store is what makes
 * the handset come back at the level the user left it on.
 *
 * This module owns:
 *   - the mixer node, created here so there is always a sink for streams
 *     to land in whatever the physical device turns out to be;
 *   - the ladder, which is what makes the slider a ladder of discrete
 *     rungs rather than a linear factor;
 *   - the user's target, and the route write it turns into.
 *
 * What it deliberately does not do: per-stream gain, ducking, and any
 * timing that aims a hardware write at a particular audio frame. Those
 * belong to flat volume and will come back as their own change, on top of
 * a base that mixes and follows the slider.
 *
 * This module creates and owns a metadata namespace of its own
 * (x-maemo-mix) rather than listening on "default": a pw_impl_metadata
 * listener on the daemon's default metadata SIGSEGVs in
 * pw_global_update_permissions() when the next client connects. The
 * routing policy binds this namespace and will not act until it exists.
 *
 * The applet's target is read the other way round, as a client: the
 * "default" namespace belongs to the session manager and is relayed by
 * module-metadata, so it is bound over a connection to our own daemon.
 * That is what connect_to_self() is for.
 *
 * Module args:
 *   tuning.default          required; the CSV ladder used when a node
 *                           publishes no curve of its own
 *   tuning.default.incall   the same, for when a call is up; optional,
 *                           and falls back to the ordinary ladder
 *   vsink.enable            create the mixer node (off by default, so a
 *                           half-configured stack fails loudly)
 *   vsink.name              node.name for it; default "maemo_mixer"
 *   route.channels          values to write per route channel; default 2
 *   save                    ask WirePlumber to persist the level against
 *                           the device; default true
 *   debug                   verbose logging; default on
 */

#include "maemo-mix-internal.h"

static void mixer_hold_property(struct impl *impl, bool hold);

/* Let the graph go only if nothing is playing.
 *
 * The hold exists to keep the mixer cycling when nothing else would drive
 * the graph -- a notification on a silent machine. When a stream is
 * running the graph is driven anyway, so the hold is not what keeps the
 * mixer scheduled, and dropping it is pure cost: every change of that
 * property makes the core recalculate the graph, and a recalculation is
 * the scheduler re-deciding who runs when, which is exactly the wrong
 * thing to do in the same instant as a codec write. */
void hold_update(struct impl *impl);


/* ---- the level ---- */

/* Hand a software gain to the mixer.
 *
 * Called with 1.0 and nothing else, for now. The level the user hears is
 * produced by the codec, not by multiplying samples here, so this stays
 * at unity -- but the call and the path through the mixer stay in place,
 * because that path is where the software residual goes when flat volume
 * returns. Removing it now would mean rebuilding it rather than repointing
 * it, and the point of this commit is to be a base, not a demolition.
 *
 * A param on the node rather than a value in some side channel: the node
 * already has SPA_PARAM_Props with a volume, that is what a volume is
 * for, and anything else would be a second answer to a question the node
 * already answers. */
void set_level(struct output *out, float volume)
{
	struct impl *impl = out->impl;
	/* This goes to the mixer through the private gain, not through the
	 * node's volume param: that param is what pipewire-pulse exports as
	 * the sink volume, so writing it announces a change the user never
	 * made and the volume applet duly wakes up for it. */
	if (out->sink.mixer == NULL)
		return;

	maemo_vol_state_set_aux(&out->vstate, volume);

	if (impl->debug)
		pw_log_info("level: %f (slider rung %d)",
			(double) volume, impl->slider_step);
}

/* The bottom of the slider is silence, and silence is a software gain.
 *
 * It cannot be a hardware step: the quietest thing the ladder can ask for
 * is around -40 dB, which is still music through a speaker, and an output
 * with no element to write, Bluetooth or HDMI, has no bottom at all. The
 * mixer ramps whatever it is given, so arriving at silence is not a click.
 *
 * While an absolute stream plays the node stays at unity and the duck
 * carries the silence instead, because the notification is meant to be
 * heard and a zero node gain would take it away as well. */
void apply_silence(struct output *out)
{
	struct impl *impl = out->impl;
	bool silent = maemo_vol_map_is_silence(out->map, impl->slider_step);
	bool abs = vol_loadb(&out->boost.active);

	set_level(impl->out, (silent && !abs) ? 0.0f : 1.0f);
}

/* ---- the ladder ---- */


/* The curve in force: the one announced on the output sink, then the one
 * parked on our own node, then the fallback from the module args. */
struct pw_impl_node *find_sink_node(struct output *out);
static void ensure_mixer_node(struct output *o);



/* ---- the hardware ---- */






struct route_vol {
	uint32_t want;
	bool found;
	float amp;
};



/* Put a level on the codec.
 *
 * One value per channel, all equal: this is the output route of the
 * device the user is listening through, and there is one user volume.
 *
 * `save` asks WirePlumber to persist it against the device, which is what
 * makes the handset come back at the level the user left it on rather
 * than at the compiled-in default. */
static void stream_add(struct impl *impl, struct pw_impl_node *node);

struct stream {
	struct spa_list link;
	struct impl *impl;
	struct pw_impl_node *node;
	struct spa_hook listener;

	/* The peer ids of this stream's ports, as reported by the Port
	 * globals the core registers. A stereo stream has two, and the
	 * absolute bit has to reach both -- leaving one at the duck is a
	 * channel that does not match the other. */
	uint32_t peer[8];
	unsigned int n_peer;
	bool absolute;
	bool running;
};

static void stream_add_peer(struct impl *impl, struct pw_impl_node *node,
		uint32_t peer_id);
static void stream_apply(struct stream *st);

static void port_published(void *user_data, uint32_t peer_id,
		struct maemo_port_data *data);


/* Runs on the control loop, invoked by the mixer from the data loop.
 *
 * All it does is remember the absolute deadline and arm the timer. It does
 * not write the codec: the mixer said *when*, and honouring that means
 * arming a timer rather than acting now. Doing the write here would make
 * the hardware half land whenever the control loop happened to get to it,
 * which is the error this whole arrangement exists to remove. */
/* Arm the timer for whichever request is due first. Re-armed on every push
 * because a later-arriving entry can be due earlier than the one already
 * armed, and a timer can only be set for one instant at a time. */
/* Forward: the realtime path hands the queue back to the control loop,
 * which re-arms through the ordinary route. */


































/* Hand the level back to the applet, as a property on the node.
 *
 * The applet reads the level from the sink node's properties and writes
 * the user's choice to metadata -- read from the node, write to the bus.
 * That asymmetry is the whole reason this function has to exist: on a
 * fresh daemon the metadata key is absent (it lives in the session
 * manager, which is not persisted) while the hardware level is, restored
 * per device by the route store. So the side that can read the hardware
 * has to publish to the side that has to show it, or the slider opens at
 * wherever it was left and the codec sits somewhere else entirely.
 *
 * Written back as a perceptual step, the inverse of the cube applied on
 * the way in, so the applet and this module are working in the same
 * units without either converting behind the other's back.
 *
 * A skip when nothing changed: updating properties broadcasts the node's
 * whole property set, so doing it on every recompute would be loud. */
static void publish_target_on(struct impl *impl, struct pw_impl_node *node)
{
	if (impl->out == NULL)
		return;
	const char *cur;
	char text[16];
	long want;
	float amp;

	if (node == NULL || impl->out->map == NULL || impl->slider_step < 0)
		return;

	amp = maemo_vol_map_virtual(impl->out->map, (unsigned) impl->slider_step);
	if (amp < 0.0f)
		amp = 0.0f;
	if (amp > 1.0f)
		amp = 1.0f;

	want = (long) (cbrt((double) amp) * 65536.0 + 0.5);

	cur = pw_properties_get(pw_impl_node_get_properties(node), APP_TARGET_KEY);
	if (cur != NULL) {
		long have = 0;
		if (sscanf(cur, "%ld", &have) == 1 && have == want)
			return;
	}

	snprintf(text, sizeof(text), "%ld", want);
	{
		struct spa_dict_item one = SPA_DICT_ITEM(APP_TARGET_KEY, text);
		struct spa_dict dict = SPA_DICT(&one, 1);
		pw_impl_node_update_properties(node, &dict);
	}

	if (impl->debug)
		pw_log_info("published target %s (rung %d, amp %.4f)",
				text, impl->slider_step, (double) amp);
}

void publish_target(struct output *out)
{
	struct impl *impl = out->impl;
	publish_target_on(impl, find_sink_node(impl->out));
}

/* ---- the user's slider ---- */

/* The applet writes a 0..65536 integer. The mapping to amplitude is the
 * perceptual one Fremantle used -- a cube, so equal steps of the slider
 * are equal steps of loudness -- and the result is then quantised to the
 * ladder, because the slider has rungs and not a continuum.
 *
 * Quantising rather than passing the raw factor through is the point: it
 * means the level the user sees, the level the mixer applies and the
 * level any later reader reports are all the same rung, so nothing has
 * to agree with anything else after the fact. */
void reread_target(struct output *out);




/* ---- the applet's metadata ---- */

/* Everything this callback does -- resolving the ladder, writing the ALSA
 * route, publishing the level back onto the node -- belongs to the main
 * loop, so it runs here and nowhere else.
 *
 * An earlier version hopped to the data loop first. That was inherited
 * from a design where a slider move also had to arm a hardware write
 * against the audio clock, and it is fatal now: reread_target ends in
 * pw_impl_node_update_properties and pw_impl_device_set_param, and
 * updating a node's properties off the main loop has the protocol
 * marshalling a node info change to clients while the node is being
 * changed under it. It takes a while to show up, and when it does it is a
 * memcpy to a small integer.
 *
 * The subject is not a gate. It is logged, and that is all:
 *
 *   - There is one user volume. Maemo has a single slider on a single
 *     output path, and every stream goes through the mixer regardless of
 *     which device is active. There is no secondary output with its own
 *     level to protect.
 *   - Per-device volume memory is the applet's job. It keeps its own
 *     per-sink tables and hands us the level for whatever it currently
 *     considers the sink.
 *   - The key is namespaced (x-maemo.volume.target), so a collision with
 *     unrelated metadata cannot happen.
 *
 * Gating here would also be unreliable: the applet's bound sink, our own
 * node and WirePlumber's default are ids that can lag each other by an
 * event or two, and the default legitimately moves between built-in and
 * Bluetooth. Any lag would drop a write.
 */
/* Which output a metadata subject belongs to.
 *
 * The subject is a node id and each mixer knows its own, so with per-output
 * mixers this is the lookup that decides which card's level the applet is
 * moving. One output today, and a subject that does not match is logged
 * rather than rejected, because there is one user volume and the applet may
 * legitimately be looking at the announced sink instead of our mixer.
 */
static struct output *output_for_node(struct impl *impl, uint32_t subject)
{
	(void) subject;
	return impl->out;
}

static int on_app_md_property(void *data, uint32_t subject,
		const char *key, const char *type, const char *value)
{
	struct impl *impl = data;
	(void) subject;

	if (impl->out == NULL)
		return 0;

	(void) type;

	if (key == NULL || value == NULL)
		return 0;
	if (strcmp(key, APP_TARGET_KEY) != 0)
		return 0;

	if (subject != impl->out->sink.bound_id)
		pw_log_debug("target from subject=%u (mixer=%u) -- applied "
				"anyway; one user volume",
				subject, impl->out->sink.bound_id);

	free(impl->app_target);
	impl->app_target = strdup(value);
	reread_target(output_for_node(impl, subject));
	return 0;
}

static const struct pw_metadata_events app_md_events = {
	PW_VERSION_METADATA_EVENTS,
	.property = on_app_md_property,
};

static void on_self_registry_global(void *data, uint32_t id,
		uint32_t permissions, const char *type, uint32_t version,
		const struct spa_dict *props)
{
	struct impl *impl = data;
	const char *name;

	(void) permissions;

	/* A card appearing is what creates an output, and this is the
	 * listener that sees globals on our own registry. */
	if (type != NULL && strcmp(type, PW_TYPE_INTERFACE_Device) == 0) {
		outputs_sync(impl);
		return;
	}

	/* A sink node appearing for a card we know is the sign that the card
	 * can actually be played on, which is the same question the policy
	 * answers later, asked earlier and by someone who cannot be wrong.
	 * The mixer for that card is created now rather than at the
	 * announcement; see ensure_mixer_node(). */
	if (type != NULL && strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
		const char *mc = props ? spa_dict_lookup(props, PW_KEY_MEDIA_CLASS) : NULL;
		const char *dev = props ? spa_dict_lookup(props, PW_KEY_DEVICE_ID) : NULL;
		struct output *o;

		if (mc == NULL || strcmp(mc, "Audio/Sink") != 0 || dev == NULL)
			return;

		spa_list_for_each(o, &impl->outputs, link) {
			struct pw_global *g;
			char id[32];

			if (o->card_dev == NULL || o->sink.mixer != NULL)
				continue;
			/* A node's device.id is the global id of the device it
			 * belongs to, not a property the device carries, so
			 * this is the comparison and not a property lookup. */
			g = pw_impl_device_get_global(o->card_dev);
			if (g == NULL)
				continue;
			snprintf(id, sizeof(id), "%u", pw_global_get_id(g));
			if (strcmp(id, dev) == 0) {
				pw_log_info("a sink appeared for %s; creating its mixer",
						o->mixer_name);
				ensure_mixer_node(o);
				break;
			}
		}
		return;
	}

	if (type == NULL || strcmp(type, PW_TYPE_INTERFACE_Metadata) != 0)
		return;

	name = props ? spa_dict_lookup(props, "metadata.name") : NULL;
	if (name == NULL || strcmp(name, APP_METADATA_NAME) != 0)
		return;
	if (impl->app_md != NULL)
		return;

	impl->app_md = pw_registry_bind(impl->self_registry, id, type,
			version, 0);
	if (impl->app_md == NULL) {
		pw_log_warn("could not bind metadata '%s'", name);
		return;
	}

	pw_metadata_add_listener(impl->app_md, &impl->app_md_hook,
			&app_md_events, impl);

	pw_log_info("watching metadata '%s' for the user's target", name);
}

static const struct pw_registry_events self_registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = on_self_registry_global,
};

static void connect_to_self(struct impl *impl)
{
	impl->self_core = pw_context_connect(impl->ctx, NULL, 0);
	if (impl->self_core == NULL) {
		pw_log_warn("could not connect to our own daemon; the "
			    "user's target will not be read");
		return;
	}

	impl->self_registry = pw_core_get_registry(impl->self_core,
			PW_VERSION_REGISTRY, 0);
	if (impl->self_registry == NULL) {
		pw_log_warn("could not get a registry; the user's target "
			    "will not be read");
		return;
	}

	pw_registry_add_listener(impl->self_registry,
			&impl->self_registry_hook,
			&self_registry_events, impl);
}


/* The output sink in force, or NULL until the routing policy names one.
 *
 * Both the curve and the level belong here and nowhere else. They are
 * properties of the output: the ladder is that amplifier's response and
 * the codec's steps, and the level is that device's restored position.
 * Our mixer node is the wrong place for either -- it is device
 * independent and outlives every device, so a ladder on it would be a
 * curve belonging to no hardware, and it would go stale the moment a
 * second output appeared. */
struct sink_find {
	const char *name;
	struct pw_impl_node *node;
};

/* Return 0 to keep walking, non-zero to stop; see pw_context_for_each_global. */
static int sink_name_match(void *data, struct pw_global *global)
{
	struct sink_find *f = data;
	struct pw_impl_node *node;
	const struct pw_properties *p;

	if (!pw_global_is_type(global, PW_TYPE_INTERFACE_Node))
		return 0;

	node = (struct pw_impl_node *) pw_global_get_object(global);
	if (node == NULL)
		return 0;

	p = pw_impl_node_get_properties(node);
	if (p == NULL)
		return 0;

	if (!spa_streq(pw_properties_get(p, PW_KEY_NODE_NAME) ?: "", f->name))
		return 0;

	f->node = node;
	return 1;
}

/* The sink node named by the last announcement, looked up now rather than
 * remembered. Returns NULL if it is not present at the moment. */
/* Publish the graph hold on the mixer node.
 *
 * The mixer used to do this itself when it noticed the hold, from its
 * process() callback. That made the core walk the graph in the realtime
 * thread, and a graph walk there ends with protocol-native aborting on
 * "impl_ext_end_resource called from wrong context". The walk must be
 * requested from the main loop, and the decision was always ours anyway:
 * we raise the hold when a codec change is due and drop it when the write
 * has landed.
 *
 * Setting the property here is also earlier than the mixer noticing it,
 * which was up to a quantum late -- the hold is in place before the cycle
 * that needs it rather than after it. */
/* The hold follows the streams, not the queue.
 *
 * node.always-process is what keeps the graph cycling when nothing else
 * would drive it, and the case that matters is not a silent machine -- it
 * is a machine with a stream running whose links are being torn down and
 * rebuilt around a notification. Measured: with a music stream running,
 * the ALSA sink received nothing for eighty milliseconds, the ring drained
 * to thirty-four frames, and the underrun rebuilt the stream under a
 * codec write that had been armed on the old geometry.
 *
 * So: hold while any stream runs, release only when none do. The property
 * write is skipped when the value is unchanged, so this costs a
 * recalculation on a real transition and nothing otherwise. */
void hold_update(struct impl *impl)
{
	bool hold = impl->out->n_running > 0;
	maemo_vol_state_set_hold(&impl->out->vstate, hold);
	mixer_hold_property(impl, hold);
}

static void mixer_hold_property(struct impl *impl, bool hold)
{
	struct pw_properties *props;

	if (impl->out->sink.mixer == NULL || impl->out->sink.hold == hold)
		return;

	impl->out->sink.hold = hold;
	props = pw_properties_new(PW_KEY_NODE_ALWAYS_PROCESS,
			hold ? "true" : "false",
			PW_KEY_NODE_RELIABLE, "true", NULL);
	pw_impl_node_update_properties(impl->out->sink.mixer, &props->dict);
	pw_properties_free(props);
}

/* The sink node this output's level belongs to, which is a device sink and
 * not our mixer: the policy announces the curve and the sync flag on it.
 * Takes the output rather than the module because during an adoption the
 * output being prepared is not yet the current one, and reading the current
 * one there is reading NULL. */
struct pw_impl_node *find_sink_node(struct output *out)
{
	struct sink_find f = { out->sink.name, NULL };

	if (out->sink.name[0] == '\0')
		return NULL;

	pw_context_for_each_global(out->impl->ctx, sink_name_match, &f);
	return f.node;
}

void read_sync_flag(struct output *out)
{
	struct impl *impl = out->impl;
	struct pw_impl_node *sn = find_sink_node(out);
	const struct pw_properties *p;
	const char *v;
	bool sync = false;

	if (sn == NULL)
		return;

	p = pw_impl_node_get_properties(sn);
	if (p == NULL)
		return;

	v = pw_properties_get(p, "x-maemo.volume.sync");
	if (v != NULL)
		sync = (strcmp(v, "true") == 0 || strcmp(v, "1") == 0);

	if (sync != out->sink.sync) {
		out->sink.sync = sync;
		pw_log_info("sink '%s' %s a synchronisable hardware stage; codec writes %s",
				out->sink.name, sync ? "advertises" : "does not advertise",
				sync ? "are queued and aimed" : "are not aimed");
	}

	/* Margins belong to the output, not to the module: what a release has
	 * to wait out is that device's buffer and control path, and a global
	 * value can only be the least common denominator of every output that
	 * might be connected. Declared per node the way the ladder is, and
	 * falling back to the module arguments when the node says nothing. */
	v = pw_properties_get(p, "x-maemo.volume.raise-margin-usec");
	impl->raise_margin_ns = v != NULL ? strtoll(v, NULL, 10) * 1000
					 : impl->raise_margin_default;
	v = pw_properties_get(p, "x-maemo.volume.lower-margin-usec");
	impl->lower_margin_ns = v != NULL ? strtoll(v, NULL, 10) * 1000
					 : impl->lower_margin_default;

	pw_log_info("sink '%s' margins: raise %lld us, lower %lld us%s",
			out->sink.name,
			(long long) (impl->raise_margin_ns / 1000),
			(long long) (impl->lower_margin_ns / 1000),
			(pw_properties_get(p, "x-maemo.volume.raise-margin-usec") ||
			 pw_properties_get(p, "x-maemo.volume.lower-margin-usec"))
			? "" : " (module default)");
}

void publish_sink_props(struct output *out)
{
	struct impl *impl = out->impl;
	struct pw_impl_node *node = find_sink_node(out);

	publish_tuning_on(out, node);
	publish_target_on(impl, node);
}

/* ------------------------------------------------------------------------
 * The distance the output sink declares for itself.
 *
 * Two backends answer "how far is the point where my volume takes effect"
 * in two different places. ALSA publishes clock.delay on the graph clock
 * and a token port latency; BlueZ publishes clock.delay = 0 and puts the
 * whole distance on the port Latency param -- measured on this rig,
 * 145.19 ms declared and 0 on the clock, against 114-118 ms of real ALSA
 * distance. A deadline that reads only the clock is therefore correct on
 * one backend and a full pipeline depth early on the other.
 *
 * Read here and logged; nothing is aimed at it yet.
 *
 * A listener rather than a one-shot read, because the number is a property
 * of the codec and buffer configuration: SBC versus AAC, or a headset that
 * changes its own buffer, moves it during a session. A value read once at
 * route announcement would be a stale constant wearing the look of a
 * measurement.
 */







/* ---- sinks appearing ---- */

/* A device sink is recreated whenever the card profile changes, the port
 * switches, or the ALSA device is re-enumerated, and a fresh node carries
 * only what the monitor rules put on it. If the routing announcement
 * landed on the previous incarnation, the new node has no curve -- and a
 * sink with no curve gives the applet nothing to map a slider position
 * to, so the control goes grey with the audio path perfectly healthy.
 *
 * So every Audio/Sink that appears is offered the fallback curve and the
 * current level. Both are skipped when the node already carries them,
 * which is what lets a per-device rule win: a monitor that gave this
 * output a curve of its own keeps it, and this only fills holes.
 *
 * Matching is on node.name, not on the announced id, because a recreated
 * sink gets a new id and the announcement for it may not have arrived
 * yet -- and the applet is already looking at the new node. */

/* ---- the other outputs ---- */

/* A card is something we can make an output for when it has a name of its
 * own to derive a mixer name from and it is not the card the announcement
 * bound. That second test is what keeps the built-in from being made twice,
 * once as the announced output and once here. */
static bool card_is_output_candidate(struct impl *impl, struct pw_impl_device *dev,
		const char *dev_name)
{
	struct output *o;

	if (dev_name == NULL || strchr(dev_name, '.') == NULL)
		return false;

	spa_list_for_each(o, &impl->outputs, link) {
		if (o->card_dev == dev)
			return false;
	}
	return true;
}

struct card_walk {
	struct impl *impl;
	struct pw_impl_device *found[8];
	uint32_t n_found;
};

static int card_walk_cb(void *data, struct pw_global *global)
{
	struct card_walk *w = data;
	struct pw_impl_device *dev;
	const struct pw_properties *p;

	if (!pw_global_is_type(global, PW_TYPE_INTERFACE_Device))
		return 0;

	dev = (struct pw_impl_device *) pw_global_get_object(global);
	if (dev == NULL)
		return 0;

	p = pw_impl_device_get_properties(dev);
	if (p == NULL)
		return 0;

	if (!card_is_output_candidate(w->impl, dev, pw_properties_get(p, "device.name")))
		return 0;

	if (w->n_found < sizeof(w->found)/sizeof(w->found[0]))
		w->found[w->n_found++] = dev;
	return 0;
}

static void output_free(struct output *o)
{
	if (o->sink.mixer != NULL)
		pw_impl_node_destroy(o->sink.mixer);
	maemo_vol_state_unregister(&o->vstate);
	if (o->ladder_cache != NULL)
		maemo_ladder_cache_free(o->ladder_cache);
	spa_list_remove(&o->link);
	pw_log_info("output for %s gone", o->sink.mixer_name);
	free(o);
}

/* The card an enumerated output belonged to is gone. Like the announced
 * device's handler this only unlinks and marks, the output itself is freed
 * by outputs_sync() outside the device's own event walk. */
static void on_card_dev_destroy(void *data)
{
	struct output *o = data;

	o->card_dev = NULL;
	spa_hook_remove(&o->card_dev_hook);
}

static const struct pw_impl_device_events card_dev_events = {
	PW_VERSION_IMPL_DEVICE_EVENTS,
	.destroy = on_card_dev_destroy,
};

/* Make an output for one card, its own mixer node named after the card and
 * its own volume state, so a level on this card is not a level on that one.
 * Nothing is announced to it, so it has no route, no control and no codec
 * thread, which is the state that says the plumbing works and shows what the
 * policy still owes. */
/* Make one output the current one: the slider, the level publication and
 * the duck all reach through impl->out, so handing it over is what makes
 * them follow the card the user is now heard on. */
/* The mixer node for an output, created at most once.
 *
 * Two things can bring it about: the routing policy announcing this card,
 * which is the ordering that used to be the only one, and a sink node for
 * the card appearing, which happens during startup and is what puts our
 * node inside the window where WirePlumber enumerates the nodes that
 * already exist rather than relying on it catching a live event, a path
 * that has been seen to drop one. */
static void ensure_mixer_node(struct output *o)
{
	struct impl *impl = o->impl;

	if (!impl->vsink_enable || o->sink.mixer != NULL)
		return;

	/* Register before the node exists, not after: the mixer is
	 * instantiated while the node is being created, and it resolves the
	 * state it reads by the name the node carries. Registered afterwards,
	 * that lookup finds nothing at the moment it runs and falls back to
	 * the module's own state, so the level and the duck this output
	 * publishes never reach the mixer that is mixing it. */
	maemo_vol_state_register(o->mixer_name, &o->vstate);

	if (create_virtual_sink(o, o->mixer_name) < 0) {
		pw_log_error("could not create a mixer for %s", o->mixer_name);
		maemo_vol_state_unregister(&o->vstate);
		return;
	}

	/* The timer that meets a codec deadline belongs to the output whose
	 * queue it drains. */
	o->codec.hw_timer = pw_loop_add_timer(
			pw_context_get_main_loop(impl->ctx), hw_timer_cb, impl);
}

void output_adopt(struct impl *impl, struct output *o)
{
	struct output *prev = impl->out;

	if (o == NULL)
		return;

	/* Being current and being adopted are not the same question, and the
	 * single test that covered both lost a case: the first card to appear
	 * is made current so the module has something to point at, and when
	 * the policy then announces that same card the adoption returned as a
	 * no-op, so the card the user is heard on never got a node at all.
	 * The node is therefore ensured whatever the ordering, and only the
	 * hand-over is skipped when nothing actually changed. */

	/* The node comes with the adoption rather than with the card: until
	 * the policy says this card is the one being heard on, a sink for it
	 * would only be another candidate for streams to land on wrong. */
	ensure_mixer_node(o);

	/* Exactly one mixer node at a time. Streams follow the default
	 * output, so a node left behind for a card that is no longer the one
	 * being heard on is only a sink with no route for something to land
	 * on, which is how a click gets raised with nothing left to lower. */
	if (prev == o)
		return;		/* already current, and the node exists now */

	if (prev != NULL && prev->sink.mixer != NULL) {
		pw_log_info("retiring the mixer for %s", prev->mixer_name);
		maemo_vol_state_unregister(&prev->vstate);
		pw_impl_node_destroy(prev->sink.mixer);
		prev->sink.mixer = NULL;
		prev->sink.bound_id = 0;
	}

	impl->out = o;
	maemo_vol_state_attach(&o->vstate, port_published, impl);

	/* The way back from this output's mixer to its codec, armed against
	 * the main loop until the thread exists and against the thread's own
	 * loop afterwards. Per state rather than per module: with two cards
	 * live, a request from one mixer must never be delivered to whichever
	 * output the module happens to consider current. */
	{
		struct pw_loop *ml = pw_context_get_main_loop(impl->ctx);
		maemo_vol_state_arm(&o->vstate, ml->loop, codec_request, o);
	}

	/* A thread only where there is a deadline to meet, which means a
	 * control this output can write. */
	if (impl->vsink_enable && !o->codec.running)
		codec_thread_start(o);
}

static void output_new(struct impl *impl, struct pw_impl_device *dev)
{
	struct output *o;
	char card[64];

	o = calloc(1, sizeof(*o));
	if (o == NULL)
		return;

	o->impl = impl;
	o->card_dev = dev;
	o->enumerated = true;
	spa_list_init(&o->streams);

	/* The rung amplitude has to start at unity, not at zero: until a
	 * target arrives this is both the codec target and the duck, and a
	 * zero here would duck every normal stream to silence the first time
	 * a notification played. */
	o->codec.amp = 1.0f;
	o->boost.max = 1.0f;			/* until a ladder is announced */
	o->boost.restore = -1.0f;
	o->boost.restore_step = -1;
	o->boost.duck = -1.0f;
	o->boost.slider_during = false;
	o->route.written = -1.0f;
	o->route.channels = impl->default_channels;
	o->codec.written = -1.0f;		/* never written; see the field */

	o->ladder_cache = maemo_ladder_cache_new();
	if (o->ladder_cache == NULL) {
		free(o);
		return;
	}

	maemo_vol_state_init(&o->vstate);

	maemo_card_name(dev, card, sizeof(card));
	snprintf(o->mixer_name, sizeof(o->mixer_name), "%s.%s",
			impl->vsink_prefix, card);

	/* No mixer node here, and the reason is not cosmetic: a mixer node is
	 * an Audio/Sink, so a node per card is a candidate per card in the
	 * default-sink election, and a stream that lands on a node with no
	 * route, no control and no codec thread raises the codec with nothing
	 * left to lower it, which is a click that never comes back down. The
	 * node comes with adoption, in output_adopt(). */
	pw_log_info("output for card %s: recorded as %s", card, o->mixer_name);

	pw_impl_device_add_listener(dev, &o->card_dev_hook, &card_dev_events, o);
	spa_list_append(&impl->outputs, &o->link);

	/* The first card to appear is the current output until an
	 * announcement says otherwise, but "current" is only a pointer: no
	 * node, no thread, no state registration, because a card that nobody
	 * has chosen must not offer a sink to the default-sink election. */
	if (impl->out == NULL) {
		impl->out = o;
		maemo_vol_state_attach(&o->vstate, port_published, impl);
	}
}

/* Reconcile the outputs with the cards that exist. Called after an
 * announcement, because that is when the announced card becomes known, and
 * whenever a device global appears. */
void outputs_sync(struct impl *impl)
{
	struct card_walk w = { .impl = impl };
	struct output *o, *tmp;
	uint32_t i;

	/* Only the enumerated ones, the announced output lives as long as
	 * the module and is embedded in it, freeing it would be freeing a
	 * member of struct impl. */
	spa_list_for_each_safe(o, tmp, &impl->outputs, link) {
		if (o->enumerated && o->card_dev == NULL)
			output_free(o);
	}

	pw_context_for_each_global(impl->ctx, card_walk_cb, &w);
	for (i = 0; i < w.n_found; i++)
		output_new(impl, w.found[i]);
}

static void on_global_added(void *data, struct pw_global *global)
{
	struct impl *impl = data;
	struct pw_impl_node *node;
	const struct pw_properties *p;
	const char *klass;

	/* A card appearing is what creates an output, so this comes before
	 * any test on there being one, otherwise the first card never gets
	 * its mixer. */
	if (pw_global_is_type(global, PW_TYPE_INTERFACE_Device)) {
		outputs_sync(impl);
		return;
	}

	if (impl->out == NULL)
		return;

	/* Ports are globals too, and their global id is exactly the number
	 * the mixer later reports as the peer of a mix port. That is the
	 * whole identification: no private headers, no guessing from
	 * neighbouring ids, no reaching into a node's internals. */
	if (pw_global_is_type(global, PW_TYPE_INTERFACE_Port)) {
		struct pw_impl_port *port = pw_global_get_object(global);
		struct pw_impl_node *owner = port != NULL ?
			pw_impl_port_get_node(port) : NULL;
		if (owner != NULL)
			stream_add_peer(impl, owner, pw_global_get_id(global));
		return;
	}

	if (!pw_global_is_type(global, PW_TYPE_INTERFACE_Node))
		return;

	node = (struct pw_impl_node *) pw_global_get_object(global);
	if (node == NULL)
		return;

	p = pw_impl_node_get_properties(node);
	if (p == NULL)
		return;

	klass = pw_properties_get(p, PW_KEY_MEDIA_CLASS);

	/* Playback streams: this is where "absolute" and "running" are
	 * learned. Tracked for every stream rather than only the absolute
	 * ones, because a stream acquires the property after it is created
	 * and the release has to be seen on a stream that was absolute and
	 * no longer is. */
	if (klass != NULL && strcmp(klass, "Stream/Output/Audio") == 0) {
		stream_add(impl, node);
		return;
	}

	if (klass == NULL || strcmp(klass, "Audio/Sink") != 0)
		return;

	/* Not our own mixer. It has no amplifier and no codec, so a curve on
	 * it would be a curve belonging to no hardware -- and it is the one
	 * sink that is never the default, so nothing would ever read it. */
	if (spa_streq(pw_properties_get(p, PW_KEY_NODE_NAME) ?: "",
			impl->out->sink.mixer_name))
		return;

	apply_tuning(impl->out, tuning_for(impl->out));
	publish_tuning_on(impl->out, node);
	publish_target_on(impl, node);
}

static const struct pw_context_events context_events = {
	PW_VERSION_CONTEXT_EVENTS,
	.global_added = on_global_added,
};

/* ---- absolute streams ----
 *
 * What we know about each stream, and the only place the words "absolute"
 * and "running" are ever compared. The mixer never sees this list: it reads
 * one published word and one refcounted per-port bit, which is everything it
 * needs and nothing it can get stale on.
 */

/* Recompute the one bit the mixer acts on and publish it with the duck.
 *
 * Both go in one store: an absolute stream starting changes both, and a
 * mixer that saw abs_active with a stale duck would arm the codec against a
 * level that had not landed. A slider move during absolute playback changes
 * the duck with the bit steady, and the mixer applies it and arms nothing. */
/* Tell the ALSA device to stop reporting hardware volume changes.
 *
 * During an absolute window the codec is pinned at the ceiling, and an
 * element change event from that write would be read by WirePlumber as a
 * user setting the volume to maximum, which is how the applet used to
 * snap to full scale on every notification. The gate is the device's own
 * filter, in the ACP element callback, and it belongs to whoever raises
 * the hardware: this module knows the window and it knows the device,
 * which is the announced output and never any of the others.
 *
 * It used to be a WirePlumber script, and the cost of it not knowing which
 * device mattered was a Props write to every audio device on every edge,
 * measured at over a thousand transitions in twelve seconds of clicking.
 */
static void hw_gate(struct output *out, bool on)
{
	struct impl *impl = out->impl;
	uint8_t buf[128];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	struct spa_pod_frame f[2];
	const struct spa_pod *param;

	if (!dev_usable(impl->out))
		return;

	spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(&b, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(&b, &f[1]);
	spa_pod_builder_string(&b, "maemo.hw-volume-gate");
	spa_pod_builder_bool(&b, on);
	spa_pod_builder_pop(&b, &f[1]);
	param = spa_pod_builder_pop(&b, &f[0]);

	if (pw_impl_device_set_param(out->route.dev, SPA_PARAM_Props, 0, param) < 0)
		pw_log_warn("hardware volume gate %s not accepted", on ? "up" : "down");
	else if (impl->debug)
		pw_log_info("hardware volume gate %s", on ? "up" : "down");
}

static void duck_publish(struct impl *impl)
{
	bool active = false;
	bool was_active = impl->out->boost.active;
	struct stream *st;

	{
		unsigned n = 0, any = 0;
		spa_list_for_each(st, &impl->out->streams, link) {
			if (st->running)
				any++;
			if (st->absolute && st->running) {
				active = true;
				n++;
			}
		}
		impl->out->n_running = any;
		hold_update(impl);
	}

	/* The duck is the ladder's amplitude for the current rung, not a
	 * readback of the control.
	 *
	 * A readback looks more honest -- "duck by what the hardware is
	 * actually doing rather than what we asked for" -- and on this rig it
	 * agreed with the ladder to within a thousandth, so it bought nothing.
	 * What it did cost was severe: with overlapping absolute streams the
	 * control is already at MAX when the second stream starts, so the
	 * readback returns MAX, and that value is both the duck and the
	 * release target. The release then "restores" unity and the codec
	 * sticks at full volume with nothing pending. Measured, not
	 * theorised: five pooled clicks left the codec at 1.0000.
	 *
	 * The ladder tracks the slider, which is the only thing that may
	 * legitimately move the codec while no absolute stream is playing. */
	/* Opening: remember what the control holds, and start fresh on slider
	 * moves. Only here -- see codec_request for why a readback taken on a
	 * later overlapping stream is poison. */
	if (active && !was_active) {
		hw_gate(impl->out, true);
		float held = read_route_volume(impl->out);
		vol_storef(&impl->out->boost.restore, held);
		vol_storeb(&impl->out->boost.slider_during, false);
		/* On a substituted port the step read at the bind is the only
		 * record of what to put back, so the opening edge leaves it
		 * alone; on the ordinary path the remembered level is the
		 * amplitude above and this step belongs to nothing. */
		if (!impl->out->boost.sub)
			impl->out->boost.restore_step = -1;
	}
	if (!active && was_active) {
		/* Lifted on the edge, not after the restore write lands: the
		 * release is meant to be announced, that is the level the user
		 * is left at, and the script that used to do this said so. */
		hw_gate(impl->out, false);
	}
	if (!active) {
		vol_storef(&impl->out->boost.restore, -1.0f);
		vol_storeb(&impl->out->boost.slider_during, false);
		if (!impl->out->boost.sub)
			impl->out->boost.restore_step = -1;
	}

	/* The invariant is `duck x MAX == slider level`, and MAX is the top
	 * rung of the ladder rather than unity, so the duck is the rung
	 * divided by that ceiling. With the ladder's top at -9 dB this is a
	 * third less attenuation than before, and the notification lands at
	 * the loudest the slider can produce instead of above it. */
	vol_storeb(&impl->out->boost.active, active);
	apply_silence(impl->out);
	{
		float duck = 1.0f;
		if (active && impl->out->boost.sub)
			/* Resolved at bind from the two steps; -1 means the
			 * stage could not be read, and guessing a ratio is
			 * worse than not ducking at all. */
			duck = impl->out->boost.duck >= 0.0f ? impl->out->boost.duck : 1.0f;
		else if (active)
			duck = maemo_vol_map_is_silence(impl->out->map,
					impl->slider_step) ?
				0.0f : impl->out->codec.amp / impl->out->boost.max;
		if (duck > 1.0f)
			duck = 1.0f;
		maemo_vol_state_publish(&impl->out->vstate, duck, active);
		pw_log_info("absolute volume %s (duck %.4f = %.4f / %.4f)",
				active ? "ON" : "off", (double) duck,
				(double) impl->out->codec.amp, (double) impl->out->boost.max);
	}

	/* Arm the codec here, on the stream's own state change.
	 *
	 * This used to be the mixer's decision, reached through an edge it
	 * noticed while processing. That had two failure modes and both were
	 * silent: an absolute stream can stop while the graph is idle, in
	 * which case nobody processes, nobody sees the edge and the codec
	 * stays at MAX; and with the edge consumed by whichever instance
	 * won the compare-exchange, a losing arming path lost the transition
	 * outright with nothing to retry it.
	 *
	 * The module already watches every stream's node state, so the
	 * transition is observed unconditionally. The mixer still owns the
	 * timing -- it publishes the graph's delay and rate -- so the
	 * deadline is still computed from the same clock as before, just
	 * from the side that cannot miss the event. */
	if (active != was_active) {
		/* Whether a change is due is decided here, on the stream's own
		 * state, because this is the one place that is guaranteed to
		 * see the transition. *When* it must become audible is not:
		 * that belongs to the cycle whose samples the duck lands on,
		 * which only the mixer can name. So the decision is published
		 * in the word above, the graph is held up so the mixer is
		 * certainly running to notice it, and the mixer comes back
		 * with an absolute deadline. */
		maemo_vol_state_set_hold(&impl->out->vstate, true);
		mixer_hold_property(impl, true);
	}
}

/* Push this stream's absolute bit onto every port it owns.
 *
 * Called whenever either half changes -- the bit, or the set of ports -- so
 * neither order of arrival matters: the Port globals can precede the mixer's
 * publication or follow it, and the last writer states the truth either way. */
static void stream_apply(struct stream *st)
{
	struct impl *impl = st->impl;
	unsigned int i, j;

	/* What kind of stream this is comes from the node's own property and is
	 * true from the moment the node exists, so read it here rather than
	 * carry what the last info change happened to see. With a cached stream
	 * the info change arrives at activation, which is after the graph
	 * already holds the first samples, and a mark that lands after the data
	 * it describes makes the raise follow the sound instead of leading it.
	 * The mark is inert on its own: it says what a port is, the data is what
	 * says something is happening. */
	{
		const struct pw_properties *p = pw_impl_node_get_properties(st->node);
		const char *a = pw_properties_get(p, "x-maemo.volume.absolute");
		st->absolute = a != NULL && strcmp(a, "1") == 0;
	}

	/* The table is keyed by the stream's peer ids, which are known as soon as
	 * the node's ports are published and do not depend on a link existing.
	 * That is the whole point: the answer is here before the first buffer of
	 * a play, whereas the port flag can only be set once the port exists and
	 * the main loop has got round to it. */
	for (i = 0; i < st->n_peer; i++) {
		if (st->absolute) {
			maemo_vol_state_abs_peer_add(&impl->out->vstate, st->peer[i]);
		} else {
			maemo_vol_state_abs_peer_remove(&impl->out->vstate, st->peer[i]);
		}
	}

	/* Every object for that peer, not the first: see peer_drop(). */
	for (i = 0; i < st->n_peer; i++) {
		for (j = 0; j < impl->out->n_peers; j++) {
			if (impl->peers[j].peer_id != st->peer[i])
				continue;
			maemo_port_data_set(impl->peers[j].data,
					MAEMO_PORT_STREAM, true);
			maemo_port_data_set(impl->peers[j].data,
					MAEMO_PORT_ABSOLUTE, st->absolute);
			pw_log_debug("join: peer %u of '%s' marked%s",
					st->peer[i],
					pw_properties_get(pw_impl_node_get_properties(st->node),
							  PW_KEY_NODE_NAME) ?: "?",
					st->absolute ? " ABSOLUTE" : " duckable");
		}
	}
}

/* The mixer named a mix port by peer id. Keep our own reference to it; the
 * mixer holds its own and drops it when the port goes. */
static void peer_add(struct impl *impl, uint32_t peer_id,
		struct maemo_port_data *data)
{
	if (impl->out->n_peers >= PEERS) {
		pw_log_warn("peer table full (%u); a stream will not be "
				"recognised as absolute", impl->out->n_peers);
		return;
	}
	impl->peers[impl->out->n_peers].peer_id = peer_id;
	impl->peers[impl->out->n_peers].data = maemo_port_data_ref(data);
	impl->out->n_peers++;
}

/* Remove every object published for this peer id.
 *
 * There is usually more than one: the core instantiates the mixer factory
 * more than once, and each instance gets its own port and therefore its own
 * port data for the same peer. Deduplicating by peer id -- which is what an
 * earlier version did -- keeps the first object and marks that one, while a
 * different instance is the one that actually mixes. The symptom was a click
 * ducked to the rung even though the codec had gone to MAX: the flag was set,
 * just on an object nobody was reading. */
static void peer_drop(struct impl *impl, uint32_t peer_id)
{
	unsigned int i;

	for (i = 0; i < impl->out->n_peers; ) {
		if (impl->peers[i].peer_id != peer_id) {
			i++;
			continue;
		}
		maemo_port_data_set(impl->peers[i].data, MAEMO_PORT_ABSOLUTE, false);
		maemo_port_data_unref(impl->peers[i].data);
		impl->peers[i] = impl->peers[impl->out->n_peers - 1];
		impl->out->n_peers--;
	}
}

/* A Port global appeared: this is what makes a peer id name a stream.
 *
 * The core registers a node's ports as globals long before anything links to
 * them, so in the usual order this runs first and the mixer's later
 * publication finds the id already recorded. Recording rather than matching
 * here is what keeps the two halves independent: neither has to know the
 * other's schedule, and the join happens in stream_apply().
 */
static void stream_add_peer(struct impl *impl, struct pw_impl_node *node,
		uint32_t peer_id)
{
	struct stream *st;

	spa_list_for_each(st, &impl->out->streams, link) {
		unsigned int i;

		if (st->node != node)
			continue;
		for (i = 0; i < st->n_peer; i++)
			if (st->peer[i] == peer_id)
				return;
		if (st->n_peer >= sizeof(st->peer)/sizeof(st->peer[0])) {
			pw_log_warn("stream has more ports than expected; "
					"a port will not be marked absolute");
			return;
		}
		st->peer[st->n_peer++] = peer_id;
		stream_apply(st);
		pw_log_info("stream %s port peer %u%s",
				pw_properties_get(pw_impl_node_get_properties(node),
						  PW_KEY_NODE_NAME) ?: "?",
				peer_id, st->absolute ? " [absolute]" : "");
		return;
	}
}

static void stream_info_changed(void *data, const struct pw_node_info *info)
{
	struct stream *st = data;
	const struct pw_properties *p = pw_impl_node_get_properties(st->node);
	const struct pw_node_info *now;
	const char *a;
	bool running;

	a = pw_properties_get(p, "x-maemo.volume.absolute");
	st->absolute = a != NULL && strcmp(a, "1") == 0;

	now = pw_impl_node_get_info(st->node);
	running = now != NULL && now->state == PW_NODE_STATE_RUNNING;
	if (running != st->running) {
		st->running = running;
		pw_log_info("stream %s %s",
				pw_properties_get(p, PW_KEY_NODE_NAME) ?: "?",
				running ? "running" : "idle");
	}

	stream_apply(st);
	duck_publish(st->impl);
}

static void stream_destroy(void *data)
{
	struct stream *st = data;

	spa_hook_remove(&st->listener);
	st->n_peer = 0;		/* the port objects belong to the peer table */
	spa_list_remove(&st->link);
	free(st);
}

static const struct pw_impl_node_events stream_events = {
	PW_VERSION_IMPL_NODE_EVENTS,
	.info_changed = stream_info_changed,
	.destroy = stream_destroy,
};

static void stream_add(struct impl *impl, struct pw_impl_node *node);

/* Runs on the control loop, handed over from the data loop by the mixer.
 *
 * The mixer supplies the instant and the direction only. The value is
 * resolved when the timer fires, not here: a slider move during absolute
 * playback changes the rung that the release must land on, and freezing the
 * number at decision time would restore a level the user has since moved
 * away from. */
/* The bump/dip harness still speaks the older payload shape; adapt it to
 * the same queue so the test exercises the real path. */

static void stream_add(struct impl *impl, struct pw_impl_node *node)
{
	struct stream *st = calloc(1, sizeof(*st));

	if (st == NULL)
		return;
	st->impl = impl;
	st->node = node;
	spa_list_append(&impl->out->streams, &st->link);
	pw_impl_node_add_listener(node, &st->listener, &stream_events, st);
	/* The join is re-stated here: the mixer may have published this
	 * stream's ports before this stream was tracked, in which case
	 * nothing has yet pushed this stream's bits onto them. */
	stream_apply(st);
	stream_info_changed(st, NULL);
}


/* The mixer tells us a mix port learned its peer.
 *
 * This runs on the main loop: the core reaches port_set_mix_info through
 * port negotiation (do_negotiate -> pw_impl_port_set_param -> setup_mixer),
 * not from the data loop. That was assumed the other way round first and the
 * assumption showed up as a crash in a re-entrant invoke onto the loop we
 * were already on, so it is stated here rather than inferred.
 *
 * Being on our own loop is what makes holding a reference safe: nothing can
 * be freed underneath us mid-callback.
 */
static void port_published(void *user_data, uint32_t peer_id,
		struct maemo_port_data *data)
{
	struct impl *impl = user_data;
	struct stream *st;

	if (data == NULL) {
		/* The mix port went away. The stream may still exist -- a
		 * cached SAMPLE_PLAY stream is paused, not destroyed -- so
		 * this is not the stream's end and must not be treated as
		 * one; only the association is dropped. */
		peer_drop(impl, peer_id);
		return;
	}

	peer_add(impl, peer_id, data);
	pw_log_debug("mixer published peer %u (%p)", peer_id, (void *) data);

	/* State the bit now in case the Port globals arrived first, which
	 * is the usual order: a node's ports are registered long before
	 * anything links to them. */
	spa_list_for_each(st, &impl->out->streams, link)
		stream_apply(st);
}

/* ---- module lifetime ---- */

static void on_module_destroy(void *data)
{
	struct impl *impl = data;

	/* The sink node owns this list, not us: if we are torn down with the
	 * hook still linked, the next emit on that node calls through freed
	 * memory in the impl we are about to free. Three different crash
	 * signatures in three minutes, all of them hook lists. */
	spa_hook_remove(&impl->ctx_hook);
	spa_hook_remove(&impl->mod_hook);

	/* Tear the client connection down before anything else that could
	 * still hold a reference into it, and in the reverse order of
	 * creation: the metadata proxy lives on the registry, the registry
	 * on the core. */
	spa_hook_remove(&impl->app_md_hook);
	spa_hook_remove(&impl->self_registry_hook);
	if (impl->app_md)
		pw_proxy_destroy((struct pw_proxy *) impl->app_md);
	if (impl->self_registry)
		pw_proxy_destroy((struct pw_proxy *) impl->self_registry);
	if (impl->self_core)
		pw_core_disconnect(impl->self_core);

	spa_hook_remove(&impl->md_hook);

	/* The codec thread writes through the snd_ctl handle and posts back
	 * to the main loop, so it has to be stopped before either is gone.
	 * The poll is blocking, hence the wake before the join. */
	if (impl->out->codec.thr) {
		impl->out->codec.running = false;
		spa_loop_invoke(impl->out->codec.loop, rt_wake_cb, 0, NULL, 0, false, impl);
		spa_thread_utils_join(impl->out->codec.tu, impl->out->codec.thr, NULL);
		impl->out->codec.thr = NULL;
		/* Nothing may be asked of a thread that has gone, so this
		 * output is re-armed against the main loop, and against its
		 * own state: the callback takes the output it was armed for,
		 * and handing it the module instead makes it write through a
		 * struct it does not own, which is corruption that surfaces
		 * somewhere unrelated a few events later. */
		maemo_vol_state_arm(&impl->out->vstate,
				pw_context_get_main_loop(impl->ctx)->loop,
				codec_request, impl->out);
	}
	if (impl->out->codec.pwloop) {
		if (impl->out->codec.timer_added) {
			pw_loop_lock(impl->out->codec.pwloop);
			spa_loop_remove_source(impl->out->codec.loop, &impl->out->codec.timer);
			pw_loop_unlock(impl->out->codec.pwloop);
			close(impl->out->codec.timer.fd);
			impl->out->codec.timer_added = false;
		}
		pw_loop_destroy(impl->out->codec.pwloop);
		impl->out->codec.pwloop = NULL;
		impl->out->codec.loop = NULL;
	}

	free(impl->app_target);
	free(impl->default_tuning);
	free(impl->default_tuning_incall);
	/* The map is the cache's, so freeing the cache releases every map
	 * this module ever used. Do not free impl->out->map as well. */
	if (impl->out->ladder_cache != NULL)
		maemo_ladder_cache_free(impl->out->ladder_cache);
	free(impl);
}

static const struct pw_impl_module_events mod_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = on_module_destroy,
};

/* pipewire hands module args over as a single string with the config's
 * spacing intact, so `key = value` arrives with the '=' detached from
 * its key. Collapse the whitespace around each '=' so the rest can split
 * on whitespace and still find the pair.
 *
 * The comments come along too, because the string is the config text and
 * not a parsed dictionary: a drop-in with a dozen explanatory lines
 * produced a hundred "malformed arg" warnings per start, one per word of
 * prose, which buried anything worth reading. So a '#' outside a quoted
 * value drops everything up to the end of that line, which is what a
 * comment means in this format. */
static char *normalize_args(const char *args)
{
	size_t n = strlen(args);
	char *out = malloc(n + 1);
	size_t o = 0;
	const char *p = args;
	bool in_q = false;

	if (out == NULL)
		return NULL;

	while (*p) {
		if (*p == '"')
			in_q = !in_q;

		if (*p == '#' && !in_q) {
			while (*p != '\0' && *p != '\n')
				p++;
			continue;
		}

		if (*p == '=') {
			while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '\t' ||
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
	out[o] = '\0';
	return out;
}

/* Trim surrounding whitespace and a matching pair of quotes, in place.
 *
 * The caller keeps ownership of the buffer, so this must hand back exactly
 * the pointer it was given. An earlier version advanced past a leading
 * quote and returned that, so the free() at module teardown was handed a
 * pointer one byte into the allocation and aborted with
 * "free(): invalid pointer". It only ever fired on shutdown, which is how
 * it survived every functional test run against it. */
static char *strip_quotes(char *s)
{
	char *base = s;
	size_t n;

	while (*s == ' ' || *s == '\t')
		s++;
	n = strlen(s);
	while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t'))
		s[--n] = '\0';

	if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') ||
		       (s[0] == '\'' && s[n - 1] == '\''))) {
		s[n - 1] = '\0';
		memmove(base, s + 1, n - 1);
	}
	return base;
}

int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *ctx = pw_impl_module_get_context(module);
	struct impl *impl;
	struct pw_impl_metadata *md;
	const char *md_name = ROUTE_NS;
	char vsink_name[128] = "maemo_mixer";
	struct pw_properties *cfg;

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL)
		return -ENOMEM;

	impl->ctx = ctx;
	impl->debug = true;
	impl->slider_step = -1;

	spa_list_init(&impl->outputs);

	cfg = pw_properties_new(NULL, NULL);
	if (args != NULL && *args != '\0') {
		char *dup = normalize_args(args);
		char *tok, *p = dup;
		while (p != NULL && (tok = strsep(&p, " \t\n\r")) != NULL) {
			char *eq;
			if (*tok == '\0' || !strcmp(tok, "{") || !strcmp(tok, "}"))
				continue;
			eq = strchr(tok, '=');
			if (eq == NULL) {
				pw_log_warn("ignoring malformed arg '%s'", tok);
				continue;
			}
			*eq = '\0';
			pw_properties_set(cfg, tok, eq + 1);
		}
		free(dup);
	}

	impl->debug = pw_properties_get_int32(cfg, "debug", 1) != 0;
	impl->default_channels = (uint32_t) pw_properties_get_int32(cfg,
			"route.channels", 2);
	/* On unless turned off: this is what persists the user's level
	 * against the device so the handset restores where it was left. */
	impl->save_requested = pw_properties_get_bool(cfg, "save", true);
	impl->vsink_enable = pw_properties_get_bool(cfg, "vsink.enable", 0);
	/* Copied, not borrowed: cfg is gone by the time the sink is built,
	 * and a pointer into its dict would outlive it. */
	{
		const char *n = pw_properties_get(cfg, "vsink.name");
		if (n != NULL)
			snprintf(vsink_name, sizeof(vsink_name), "%s", n);
		snprintf(impl->vsink_prefix, sizeof(impl->vsink_prefix), "%s", vsink_name);
	}

	/* The fallback ladder is required configuration, not a nicety.
	 * Without it a node that no rule gave a curve to has no ladder,
	 * and the slider would have nothing to quantise against. Refuse to
	 * load instead: a module running with no ladder looks alive while
	 * the volume is wrong, which is worse than absent.
	 *
	 * Duplicated, not borrowed: cfg is freed once init finishes and
	 * this is read again on every later ladder resolution. */
	{
		const char *dt = pw_properties_get(cfg, DEFAULT_TUNING_KEY);
		const char *dti = pw_properties_get(cfg, DEFAULT_TUNING_INCALL_KEY);

		if (dt == NULL || *dt == '\0') {
			pw_log_error("missing required '%s' in module args; "
				     "refusing to load", DEFAULT_TUNING_KEY);
			pw_properties_free(cfg);
			free(impl);
			return -EINVAL;
		}
		impl->default_tuning = strip_quotes(strdup(dt));
		/* Optional: a device without a call ladder falls back to the
		 * ordinary one, which is what a sink with no telephony is. */
		if (dti != NULL && *dti != '\0')
			impl->default_tuning_incall = strip_quotes(strdup(dti));
	}


	pw_context_add_listener(ctx, &impl->ctx_hook, &context_events, impl);
	pw_impl_module_add_listener(module, &impl->mod_hook, &mod_events, impl);


	{
		/* From the parsed args, not from the module properties: this
		 * module reads its own args string, and pw_impl_module_get_-
		 * properties() does not carry them -- reading the wrong one
		 * silently produced no margin at all.
		 *
		 * Absent means no margin, which is the honest default for a
		 * pipeline whose published delay has been checked. */
		const char *v;
		if ((v = pw_properties_get(cfg, "maemo.raise-margin-usec")))
			impl->raise_margin_ns = strtoll(v, NULL, 10) * 1000;
		if ((v = pw_properties_get(cfg, "maemo.lower-margin-usec")))
			impl->lower_margin_ns = strtoll(v, NULL, 10) * 1000;
		impl->raise_margin_default = impl->raise_margin_ns;
		impl->lower_margin_default = impl->lower_margin_ns;
		pw_log_info("codec margins: raise %lld us, lower %lld us",
				(long long) (impl->raise_margin_ns / 1000),
				(long long) (impl->lower_margin_ns / 1000));

		/* There is no configured control to open. The route the policy
		 * announces names the control that carries its volume, and
		 * that is the only source: a board fact written into a config
		 * line is a second truth for the same thing, and the two of
		 * them were already disagreeing, the config naming a control
		 * the card does not have and the announcement arriving empty
		 * because the lookup was scoped to the card rather than the
		 * output. A route with no element, a Bluetooth sink among
		 * them, leaves the codec path off, which is the truth for
		 * that output. */
	}


	{
		struct pw_loop *ml = pw_context_get_main_loop(impl->ctx);
		/* Test instrumentation, separate channel: lets the bump/dip
		 * harness drive the same crossing without an absolute
		 * stream. Harmless when the environment variables are unset. */
		maemo_swstep_arm(ml->loop, swstep_request_cb, impl);
	}

	/* One output per card that exists. There may be none yet, the device
	 * monitor enumerates cards on its own schedule and this module does
	 * not have to be the last one loaded, so everything below and every
	 * per-output entry point has to live with impl->out being NULL until
	 * a card turns up. */
	outputs_sync(impl);


	/* The routing policy will not act until our namespace exists, so
	 * this is load-bearing rather than cosmetic. */
	md = pw_context_create_metadata(ctx, md_name,
			pw_properties_new(NULL, NULL), 0);
	if (md == NULL) {
		pw_log_error("could not create metadata '%s'; the routing "
				"policy will not route", md_name);
	} else {
		static const struct pw_impl_metadata_events md_events = {
			PW_VERSION_IMPL_METADATA_EVENTS,
			.property = on_route_property,
		};
		pw_impl_metadata_add_listener(md, &impl->md_hook,
				&md_events, impl);
		if (pw_impl_metadata_register(md, NULL) < 0)
			pw_log_error("could not register metadata '%s'", md_name);
	}

	connect_to_self(impl);

	pw_properties_free(cfg);
	return 0;
}
