/* maemo mixer module, the hardware level path.
 *
 * The queue of level changes the mixer asked for, the thread that delivers
 * them on time, the ALSA control it writes, and the route parameter that
 * carries the level to the card. One of these per output.
 *
 * SPDX-License-Identifier: MIT
 */

#include "maemo-mix-internal.h"

/* What crosses to the data loop. The step is computed on the control
 * loop, from the ladder and the element's dB scale, so the realtime
 * callback holds an integer and writes it: no libm, no map, no ladder on
 * the timing path. The amplitude rides along only so the control loop can
 * account for what was written. */
struct rt_cmd {
	uint64_t deadline_ns;
	long step;
	float amp;
};
/* What comes back from the realtime write: the level that went out and the
 * moment it went out. Everything measured on the realtime thread for
 * reporting only was removed; re-add it as its own struct if the codec
 * timing ever needs measuring again. */
struct rt_done {
	uint64_t now_ns;
	long step;
	float amp;
};
uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000000000u + ts.tv_nsec;
}
/* Open the mixer and resolve the element.
 *
 * The name arrives the way the device announced it, `name,index`, which is
 * the simple layer's own vocabulary, an element being what the mixer calls
 * a group of controls and the index separating two of them that share a
 * name. Nothing here reconstructs a raw control name: the write goes
 * through the same layer the name came from, so there is nothing to put
 * back or guess at, and the dB scale comes from the same handle, which is
 * why this does not need a second one.
 *
 * All of this blocks, so it runs once on the control loop and the handle
 * is only ever used from the codec loop afterwards, and a rebind quiesces
 * that thread before closing. */
int rt_ctl_open(struct output *out, const char *card, const char *elem)
{
	snd_mixer_selem_id_t *sid;
	char name[128];
	long index = 0;
	int res;

	if (card == NULL || elem == NULL)
		return -EINVAL;

	/* Split the index off the announced spelling. The quoted form is
	 * accepted too, since that is how the mixer layer spells an indexed
	 * element internally and an older announcer used it. */
	snprintf(name, sizeof(name), "%s", elem);
	{
		char *comma = strrchr(name, ',');
		if (comma != NULL) {
			*comma = '\0';
			index = strtol(comma + 1, NULL, 10);
		}
		if (name[0] == '\'' || name[0] == '"') {
			char q = name[0];
			char *close = strchr(name + 1, q);
			if (close != NULL)
				*close = '\0';
			memmove(name, name + 1, strlen(name));
		}
	}

	if ((res = snd_mixer_open(&out->codec.ctl.mixer, 0)) < 0) {
		pw_log_warn("rt control: could not open '%s': %s", card, snd_strerror(res));
		out->codec.ctl.mixer = NULL;
		return -EIO;
	}
	snd_mixer_selem_register(out->codec.ctl.mixer, NULL, NULL);

	if ((res = snd_mixer_attach(out->codec.ctl.mixer, card)) < 0) {
		pw_log_warn("rt control: could not attach '%s': %s", card, snd_strerror(res));
		snd_mixer_close(out->codec.ctl.mixer);
		out->codec.ctl.mixer = NULL;
		return -EIO;
	}
	snd_mixer_load(out->codec.ctl.mixer);

	snd_mixer_selem_id_alloca(&sid);
	snd_mixer_selem_id_set_name(sid, name);
	snd_mixer_selem_id_set_index(sid, (unsigned int) index);
	out->codec.ctl.elem = snd_mixer_find_selem(out->codec.ctl.mixer, sid);

	if (out->codec.ctl.elem == NULL) {
		pw_log_warn("rt control: no element '%s' index %ld on '%s'",
				name, index, card);
		snd_mixer_close(out->codec.ctl.mixer);
		out->codec.ctl.mixer = NULL;
		return -ENOENT;
	}

	/* A switch or an enum is not a volume however it is named, and
	 * writing one as though it were is how a codec ends up quietly
	 * wrong, or loudly muted. */
	if (!snd_mixer_selem_has_playback_volume(out->codec.ctl.elem)) {
		pw_log_warn("rt control: '%s' has no playback volume; refusing it", name);
		snd_mixer_close(out->codec.ctl.mixer);
		out->codec.ctl.mixer = NULL;
		out->codec.ctl.elem = NULL;
		return -EINVAL;
	}

	snd_mixer_selem_get_playback_volume_range(out->codec.ctl.elem,
			&out->codec.ctl.lo, &out->codec.ctl.hi);

	/* Without the element's dB scale there is no honest way to turn a
	 * rung into a step, so the realtime path is refused rather than
	 * guessed at; the ordinary route still works. The range call is the
	 * test: a simple element with no dB in it fails it, there is no
	 * separate predicate to ask. */
	if (snd_mixer_selem_get_playback_dB_range(out->codec.ctl.elem,
			&out->codec.ctl.db_lo, &out->codec.ctl.db_hi) == 0 &&
	    out->codec.ctl.db_hi > out->codec.ctl.db_lo) {
		out->codec.ctl.has_db = true;
	} else {
		pw_log_warn("rt control: '%s' has no usable dB scale "
				"(linear or unknown); staying on the ordinary path", name);
	}

	snprintf(out->codec.card, sizeof(out->codec.card), "%s", card);
	snprintf(out->codec.elem, sizeof(out->codec.elem), "%s", elem);

	out->codec.ctl.ok = true;
	pw_log_info("rt control: '%s' on '%s', range %ld..%ld",
			elem, card, out->codec.ctl.lo, out->codec.ctl.hi);
	if (out->codec.ctl.has_db)
		pw_log_info("rt control: dB scale %ld..%ld (0.01 dB), so rung "
				"amplitudes map onto %ld..%ld",
				out->codec.ctl.db_lo, out->codec.ctl.db_hi,
				out->codec.ctl.lo, out->codec.ctl.hi);
	return 0;
}
/* Close the control. Only ever called with the realtime thread quiesced,
 * which is what keeps "one thread touches rt_ctl" true across a route
 * change rather than only across the lifetime of the module. */
void rt_ctl_close(struct output *out)
{
	if (out->codec.ctl.mixer != NULL) {
		snd_mixer_close(out->codec.ctl.mixer);
		out->codec.ctl.mixer = NULL;
	}
	out->codec.ctl.elem = NULL;
	out->codec.ctl.ok = false;
	out->codec.ctl.has_db = false;
	out->codec.card[0] = '\0';
	out->codec.elem[0] = '\0';
}
/* The realtime thread reads the control through these pointers, so a swap
 * has to be ordered against it: quiesce on that loop and wait for it, do
 * the blocking work here, publish on that loop. The blocking part stays
 * off the realtime loop on purpose, opening a mixer and loading its
 * elements is milliseconds and a millisecond there is an xrun. */
int rt_ctl_quiesce_cb(struct spa_loop *loop, bool async, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	struct impl *impl = user_data;

	(void) loop; (void) async; (void) seq; (void) data; (void) size;
	impl->out->codec.ctl.ok = false;
	return 0;
}
int rt_ctl_publish_cb(struct spa_loop *loop, bool async, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	struct impl *impl = user_data;

	(void) loop; (void) async; (void) seq; (void) data; (void) size;
	impl->out->codec.ctl.ok = true;
	return 0;
}
/* Returns what the open did, because the caller may need to decide about
 * the element that was just handed over. `rt_ctl.ok` cannot answer that:
 * it is cleared here and published by the realtime loop, so on this thread
 * it reads false for a moment after even a successful open. */
int rt_ctl_rebind(struct output *out, const char *card, const char *elem)
{
	struct impl *impl = out->impl;
	bool want = card != NULL && elem != NULL && card[0] != '\0' && elem[0] != '\0';
	int res = -ENOENT;

	if (out->codec.loop != NULL)
		spa_loop_invoke(out->codec.loop, rt_ctl_quiesce_cb, 0, NULL, 0,
				false, impl);
	else
		out->codec.ctl.ok = false;

	rt_ctl_close(impl->out);

	if (want) {
		res = rt_ctl_open(impl->out, card, elem);
		if (res < 0)
			pw_log_warn("codec rebind: '%s' on '%s' refused", elem, card);
	} else {
		pw_log_info("codec rebind: this output has no hardware volume; "
				"absolute levels have no register to reach");
	}

	if (out->codec.ctl.ok && out->codec.loop != NULL) {
		/* Published by the realtime loop, never written here, so the
		 * thread that reads it is the one that flips it. */
		out->codec.ctl.ok = false;
		spa_loop_invoke(out->codec.loop, rt_ctl_publish_cb, 0, NULL, 0,
				false, impl);
	}
	return res;
}
int device_route_cb(void *data, int seq, uint32_t id, uint32_t index,
		uint32_t next, struct spa_pod *param)
{
	struct hw_vol_read *r = data;
	struct spa_pod_object *obj;
	const struct spa_pod_prop *found = NULL;
	struct spa_pod *item, *key;
	unsigned n = 0;

	(void) seq; (void) index; (void) next;

	if (id != SPA_PARAM_Route || param == NULL)
		return 0;

	obj = (struct spa_pod_object *) param;

	/* The route we were told about, and no other. */
	{
		const struct spa_pod_prop *pi = NULL;
		int32_t idx = -1;
		pi = spa_pod_object_find_prop(obj, pi, SPA_PARAM_ROUTE_index);
		if (pi == NULL || spa_pod_get_int(&pi->value, &idx) < 0 ||
		    (uint32_t) idx != r->want_index)
			return 0;
	}

	r->saw_route = true;

	found = spa_pod_object_find_prop(obj, found, SPA_PARAM_ROUTE_info);
	if (found == NULL || !spa_pod_is_struct(&found->value))
		return 0;

	/* The hint dict is a struct whose first member is the item count and
	 * whose rest are (String key, String value) pairs, so step over the
	 * count and then two at a time. */
	key = NULL;
	SPA_POD_STRUCT_FOREACH((struct spa_pod_struct *) &found->value, item) {
		if (n++ == 0)
			continue;
		if (key == NULL) {
			key = item;
			continue;
		}
		if (spa_pod_is_string(key) && spa_pod_is_string(item)) {
			const char *name = NULL;

			/* spa_pod_get_string reports success as 0, not as a
			 * length; checking > 0 here silently never matched
			 * and the element read as absent. */
			if (spa_pod_get_string(key, &name) == 0 &&
			    spa_streq(name, HW_VOLUME_ELEMENT_KEY)) {
				r->key_seen = true;
				if (spa_pod_get_string(item, &name) == 0)
					snprintf(r->elem, sizeof(r->elem), "%s", name);
				else
					r->elem[0] = '\0';
			}
		}
		key = NULL;
	}
	return 0;
}
/* Called on the control loop whenever the routing policy announces a
 * route, which is also the moment the element can have changed under us:
 * speaker to headphone is a route change on the same card, and the
 * control that carries the volume changes with it. */
void bind_announced_control(struct output *out)
{
	struct impl *impl = out->impl;
	struct hw_vol_read r = { .want_index = out->route.index };
	const struct pw_properties *dp;
	const char *cnum;
	char card[64];
	int res;

	if (!dev_usable(impl->out))
		return;

	pw_impl_device_for_each_param(out->route.dev, 0, SPA_PARAM_Route, 0,
			UINT32_MAX, NULL, device_route_cb, &r);

	if (!r.saw_route) {
		if (impl->debug)
			pw_log_info("announced route %u not found on the device; "
					"keeping the configured codec control",
					out->route.index);
		return;
	}

	if (!r.key_seen || r.elem[0] == '\0') {
		if (out->codec.ctl.elem != NULL) {
			pw_log_info("announced output has no hardware volume "
					"control; codec path off");
			(void) rt_ctl_rebind(impl->out, NULL, NULL);
		}
		return;
	}

	dp = pw_impl_device_get_properties(out->route.dev);
	cnum = dp != NULL ? pw_properties_get(dp, "alsa.card") : NULL;
	if (cnum == NULL) {
		pw_log_warn("announced element '%s' but device carries no card number",
				r.elem);
		return;
	}
	snprintf(card, sizeof(card), "hw:%s", cnum);

	read_abs_pair(impl->out);

	/* The override is a substitution, not a second control: absolute
	 * playback moves this name and nothing else about the port changes.
	 * It is off when the port asked for nothing, when it asked for the
	 * name the route already declares -- then the two writers are the
	 * same writer and the deferral still has to happen -- and when the
	 * name cannot be opened. All three fail to today's behaviour.
	 *
	 * Warn every time rather than once per name: a match is not a
	 * configuration anyone should expect, route announcements are already
	 * deduplicated, and keeping a suppression table across a device that
	 * walks out of range costs more than the noise it saves. */
	const char *want = r.elem;
	bool sub = false;

	if (out->boost.elem[0] != '\0') {
		if (spa_streq(out->boost.elem, r.elem))
			pw_log_warn("codec control: '%s' is the announced control too; "
					"ignoring the configured absolute control",
					r.elem);
		else {
			want = out->boost.elem;
			sub = true;
		}
	}

	if (out->codec.ctl.elem != NULL && spa_streq(card, out->codec.card) &&
	    spa_streq(want, out->codec.elem)) {
		bind_abs_ceiling(impl->out, sub);
		return;
	}

	pw_log_info("codec control: '%s' on '%s' (was '%s' on '%s')",
			want, card, out->codec.elem, out->codec.card);
	res = rt_ctl_rebind(impl->out, card, want);

	if (sub && res < 0) {
		pw_log_warn("codec control: '%s' could not be opened; falling back to "
				"the announced '%s'", want, r.elem);
		rt_ctl_rebind(impl->out, card, r.elem);
		sub = false;
	} else if (sub && (out->boost.step < out->codec.ctl.lo ||
			out->boost.step > out->codec.ctl.hi)) {
		pw_log_warn("codec control: step %ld is outside '%s' (%ld..%ld); "
				"falling back to the announced '%s'",
				out->boost.step, want, out->codec.ctl.lo, out->codec.ctl.hi,
				r.elem);
		rt_ctl_rebind(impl->out, card, r.elem);
		sub = false;
	}

	bind_abs_ceiling(impl->out, sub);
}
/* The ceiling an absolute stream is put at, as the amplitude the duck
 * divides by.
 *
 * On a substituted port that is the amplitude the configured step means,
 * not the top rung of a ladder that describes a different control. The
 * conversion happens here, once per announcement, so the write path can
 * carry the step itself and never meet a float. */
void bind_abs_ceiling(struct output *out, bool sub)
{
	struct impl *impl = out->impl;
	bool changed = sub != out->boost.sub;
	float raise_amp = -1.0f;

	out->boost.sub = sub;
	out->boost.duck = -1.0f;
	out->boost.restore_step = -1;

	if (sub) {
		raise_amp = rt_step_to_amp(impl->out, out->boost.step);

		if (raise_amp > 0.0f)
			vol_storef(&out->boost.max, raise_amp);
		else
			pw_log_warn("absolute control: '%s' step %ld cannot be turned into "
					"an amplitude; using the announced ceiling",
					out->boost.elem, out->boost.step);

		/* What the stage holds now is what the release has to put back
		 * and what the duck has to compensate for. Read here rather
		 * than at the opening edge: this is the moment UCM's own
		 * sequence has finished, and the value only moves with the
		 * profile, which is what re-enters this function. */
		if (raise_amp > 0.0f) {
			long held;
			if (rt_ctl_read(impl->out, &held)) {
				float held_amp = rt_step_to_amp(impl->out, held);

				out->boost.restore_step = held;
				if (held_amp > 0.0f) {
					out->boost.duck = held_amp / raise_amp;
					if (out->boost.duck > 1.0f)
						out->boost.duck = 1.0f;
					pw_log_info("absolute control: '%s' step %ld -> %ld, "
							"duck %.4f for the window",
							out->boost.elem, held, out->boost.step,
							(double) out->boost.duck);
				}
			} else {
				pw_log_warn("absolute control: could not read '%s'; the "
						"window will raise without a duck or a "
						"restore", out->boost.elem);
			}
		}

		return;
	}

	if (changed)
		apply_tuning(impl->out, tuning_for(impl->out));
}
/* A rung amplitude to a control step, on the control loop.
 *
 * The amplitude is first pulled back onto its rung, so the ladder keeps
 * deciding which levels exist and where the ceiling is; only then is it
 * converted, through the element's own dB scale. Rounding is ours and
 * visible here, which is an improvement over the ALSA node doing it
 * silently -- the VM's ladder and codec disagree by about a decibel and
 * that disagreement used to be invisible. */
long rt_amp_to_step(struct output *out, float amp)
{
	long db, v;
	int rung;

	if (!out->codec.ctl.elem || !out->codec.ctl.has_db || out->map == NULL)
		return -1;

	rung = maemo_vol_map_step_for_amp(out->map, amp);

	if (amp <= 0.0f)
		return out->codec.ctl.lo;

	/* 0.01 dB, the unit the TLV uses. The amplitude is used as it
	 * arrived, because it already is a rung: the mixer only ever asks
	 * for levels the ladder has, and its ceiling is the ladder's top
	 * rung rather than the control's, so the ladder keeps deciding what
	 * exists and this only decides how to ask the hardware for it. */
	db = lround(2000.0 * log10((double) amp));

	/* The ladder's centibels are measured from PipeWire's unity, and
	 * PipeWire puts unity at the top of the control's TLV, not at its
	 * 0 dB -- which is how the tuning tables were generated: their own
	 * derivation is cB = (hardware_dB - tlv_max) * 100. So the rung's
	 * dB have to be shifted by the control's own maximum before they
	 * mean anything to the hardware. On a control that tops out at
	 * 0 dB this is invisible, which is how it stayed unnoticed; on the
	 * d4, whose HiFi volume runs to +12 dB, leaving it out puts every
	 * rung twelve decibels low and the ladder's top at -9 dB instead
	 * of the +3 dB the table says. */
	db += out->codec.ctl.db_hi;

	/* And the element itself answers the question, in its own steps:
	 * alsa-lib knows this element's scale, its steps need not be evenly
	 * spaced in dB and nothing here assumes they are. */
	if (snd_mixer_selem_ask_playback_dB_vol(out->codec.ctl.elem, db, 0, &v) < 0) {
		pw_log_warn("rt control: '%s' cannot produce %ld (0.01 dB)",
				out->codec.elem, db);
		return -1;
	}

	if (v < out->codec.ctl.lo)
		v = out->codec.ctl.lo;
	if (v > out->codec.ctl.hi)
		v = out->codec.ctl.hi;

	pw_log_debug("rt step: amp %.4f rung %d -> %ld (0.01 dB) -> step %ld",
			(double) amp, rung, db, v);
	return v;
}
/* The write itself, from the realtime thread: one call, one control
 * write underneath. Everything that had to be thought about was thought
 * about on the control loop before this was armed. */
void rt_ctl_write(struct output *out, long step)
{
	if (!out->codec.ctl.ok)
		return;

	if (snd_mixer_selem_set_playback_volume_all(out->codec.ctl.elem, step) < 0)
		pw_log_warn("rt control: write failed");
}
/* The inverse of rt_amp_to_step, and deliberately not a rung lookup: a
 * substituted control holds a level the ladder never produced, so the
 * answer has to come from the control's own dB scale. */
float rt_step_to_amp(struct output *out, long step)
{
	long db;

	if (!out->codec.ctl.elem || !out->codec.ctl.has_db)
		return -1.0f;

	if (snd_mixer_selem_ask_playback_vol_dB(out->codec.ctl.elem, step, &db) < 0)
		return -1.0f;

	/* The ladder's zero is PipeWire unity, which sits at the top of the
	 * control's dB range, not at its 0 dB -- the same shift
	 * rt_amp_to_step applies on the way out. */
	db -= out->codec.ctl.db_hi;

	return (float) pow(10.0, (double) db / 2000.0);
}
/* Read the control as it stands.
 *
 * The mixer caches values, and a bind can land while a window is open with
 * a write pending on the codec loop, so ask again rather than trusting
 * what the load left behind. */
bool rt_ctl_read(struct output *out, long *step)
{
	long v;

	if (!out->codec.ctl.elem)
		return false;

	if (snd_mixer_elem_value(out->codec.ctl.elem) < 0 ||
	    snd_mixer_selem_get_playback_volume(out->codec.ctl.elem,
			SND_MIXER_SCHN_MONO, &v) < 0) {
		pw_log_warn("rt control: read failed");
		return false;
	}

	*step = v;
	return true;
}
/* Runs on the control loop: hand the head of the queue to the data loop
 * so it can be armed there. Passed by value, so the data loop never
 * touches queue state it does not own. */
/* Our own realtime thread, with a loop of its own that holds exactly one
 * source: the codec timer.
 *
 * It used to sit on the mixer's data loop, which shares its poll with the
 * ALSA node. A timer queued there waits for whatever that loop is doing,
 * and when the USB ring is full that is a blocking write -- which is how
 * a timer that should answer in tens of microseconds was answering in six
 * milliseconds. Here there is nothing to wait behind, and the millisecond
 * a control write costs belongs to a thread no audio depends on. */
void *rt_thread_fn(void *data)
{
	struct impl *impl = data;

	pw_loop_enter(impl->out->codec.pwloop);
	while (impl->out->codec.running)
		pw_loop_iterate(impl->out->codec.pwloop, -1);
	pw_loop_leave(impl->out->codec.pwloop);
	return NULL;
}
/* Bring up the realtime codec loop and its thread.
 *
 * Started unconditionally at module init and not tied to a route: the
 * control name now arrives with the route announcement, which is after
 * init, and a thread that has to be conjured up in the middle of a route
 * change is a thread that is missing until the first announcement. When
 * the output has no hardware volume the thread simply has nothing to do.
 * Idempotent, so a second call is free.
 */
void codec_thread_start(struct output *out)
{
	struct impl *impl = out->impl;
	if (out->codec.pwloop != NULL)
		return;

	struct spa_dict_item item = SPA_DICT_ITEM_INIT(SPA_KEY_THREAD_NAME,
							"maemo-codec");
	struct spa_dict props = SPA_DICT_INIT(&item, 1);

	out->codec.pwloop = pw_loop_new(NULL);
	if (out->codec.pwloop == NULL) {
		pw_log_warn("rt control: could not create the codec loop; "
				"staying on the control-loop timer");
		out->codec.ctl.ok = false;
	} else {
		out->codec.loop = out->codec.pwloop->loop;
		out->codec.running = true;
		/* The thread utils matter. pw_thread_utils_create() uses
		 * the default implementation, which is the plain pthread
		 * one and has no RTKit to talk to; module-rt publishes its
		 * own into the context, and that is the one that asks
		 * RTKit for a priority and an RT budget. Asking the
		 * default for realtime just fails. */
		out->codec.tu = pw_context_get_object(impl->ctx,
					SPA_TYPE_INTERFACE_ThreadUtils);
		if (out->codec.tu == NULL)
			out->codec.tu = pw_thread_utils_get();
		out->codec.thr = spa_thread_utils_create(out->codec.tu, &props,
					rt_thread_fn, impl);
		if (out->codec.thr != NULL) {
			/* From here the request path is realtime end to
			 * end: the mixer's cycle arms the codec loop
			 * directly, so a release no longer queues behind
			 * the graph teardown a notification leaves behind
			 * it on the control loop. */
			maemo_vol_state_arm(&out->vstate, out->codec.loop,
					codec_request, out);
			pw_log_info("rt control: codec loop owns the queue");
		}
		if (out->codec.thr == NULL) {
			pw_log_warn("rt control: could not start the codec "
					"thread; staying on the control-loop timer");
			out->codec.running = false;
			out->codec.loop = NULL;
			pw_loop_destroy(out->codec.pwloop);
			out->codec.pwloop = NULL;
			out->codec.ctl.ok = false;
		} else {
			/* -1 means whatever RTKit is configured to hand
			 * out, which is what the audio loops get. */
			int r = spa_thread_utils_acquire_rt(out->codec.tu,
					out->codec.thr, -1);
			if (r == 0)
				pw_log_info("rt control: codec thread up, realtime granted");
			else
				pw_log_warn("rt control: codec thread up, realtime refused: %s",
						spa_strerror(r));
		}
	}
}
/* Nothing to do; used to wake the loop out of its poll when stopping. */
int rt_wake_cb(struct spa_loop *loop, bool async, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	(void) loop; (void) async; (void) seq; (void) data; (void) size;
	(void) user_data;
	return 0;
}
uint64_t rt_next_wake(uint64_t deadline, uint64_t now)
{
	uint64_t left;

	if (now >= deadline)
		return now;
	left = deadline - now;
	if (left > RT_COARSE_WINDOW) {
		/* Sleep the excess, but never in one long piece: a coarse
		 * wake that arrives later than the window leaves the fine
		 * steps nothing to recover with, and the deep idle tail on
		 * this hardware is wider than the window. Capping the piece
		 * bounds how far out of touch we can ever be. */
		uint64_t w = left - RT_COARSE_WINDOW;
		return now + (w > RT_COARSE_MAX ? RT_COARSE_MAX : w);
	}
	if (left > RT_FINE_STEP)
		return now + RT_FINE_STEP;
	return deadline;
}
void rt_arm_at(struct output *out, uint64_t when)
{
	struct itimerspec its = {
		.it_value = {
			.tv_sec = (time_t) (when / 1000000000u),
			.tv_nsec = (long) (when % 1000000000u),
		},
	};

	if (timerfd_settime(out->codec.timer.fd, TFD_TIMER_ABSTIME, &its, NULL) < 0)
		pw_log_warn("rt control: could not arm: %s", strerror(errno));
}
/* The codec loop's own timer, created once. The SPA timer interface
 * belongs to the loop utils, which a plain loop does not carry, so a
 * timerfd added to the codec loop is how a timed callback gets onto
 * the realtime thread. */
int rt_timer_ensure(struct output *out)
{
	struct impl *impl = out->impl;
	int fd;

	if (out->codec.timer_added)
		return 0;

	fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (fd < 0) {
		pw_log_warn("rt control: no timerfd: %s", strerror(errno));
		return -errno;
	}
	out->codec.timer.fd = fd;
	out->codec.timer.mask = SPA_IO_IN;
	out->codec.timer.func = rt_timer_cb;
	out->codec.timer.data = impl;
	if (spa_loop_add_source(out->codec.loop, &out->codec.timer) < 0)
		pw_log_warn("rt control: could not add the timer to the codec loop");
	out->codec.timer_added = true;
	return 0;
}
/* Runs on the realtime thread. Two things only: write the control, and
 * hand back to the control loop so it can retire what was consumed.
 * Nothing here reads the queue. */
void rt_timer_cb(struct spa_source *source)
{
	struct impl *impl = source->data;
	struct timespec ts;
	struct rt_done done;
	uint64_t now, expirations = 0;

	if (read(impl->out->codec.timer.fd, &expirations, sizeof(expirations)) < 0) {
		pw_log_warn("rt timer fired but read failed: %s", strerror(errno));
		return;
	}
	if (!impl->out->codec.pending)
		return;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	now = (uint64_t) ts.tv_sec * 1000000000u + ts.tv_nsec;

	/* Not due. Nothing distinguishes this wake from any other except
	 * that the clock has not arrived yet, so the answer is to ask
	 * again for whatever the deadline still wants, and nothing is
	 * written and nothing is retired until it has. */
	if (now < impl->out->codec.deadline) {
		pw_log_debug("rt step: early wake %.3f ms before the deadline",
			(double) (impl->out->codec.deadline - now) / 1e6);
		rt_arm_at(impl->out, rt_next_wake(impl->out->codec.deadline, now));
		return;
	}

	impl->out->codec.pending = false;
	{
		rt_ctl_write(impl->out, impl->out->codec.step);
	}

	/* The guard band, checked where it can actually be violated.
	 *
	 * "Raise late, lower early" is not a jitter allowance: it is the
	 * interval that guarantees the bed is already ducked before the
	 * codec goes up, and already back down before the duck is lifted.
	 * Violate it and the music is audible at codec maximum, which is
	 * the one failure a listener hears as a bump. So: a release that
	 * lands later than its margin, or a raise that lands earlier than
	 * its margin, has exposed the music for the difference. */
	{
		int64_t margin = impl->out->codec.raise ? impl->raise_margin_ns
					       : impl->lower_margin_ns;
		int64_t exposed = impl->out->codec.raise
			? ((int64_t) impl->out->codec.deadline - (int64_t) now) - margin
			: ((int64_t) now - (int64_t) impl->out->codec.deadline) - margin;
		if (exposed > 0)
			pw_log_warn("EXPOSED: music at codec max for %.3f ms (%s; "
					"late %.3f ms, guard %+.3f ms)",
					(double) exposed / 1e6,
					impl->out->codec.raise ? "raise landed early"
						       : "release landed late",
					(double) (now > impl->out->codec.deadline ?
						  now - impl->out->codec.deadline : 0) / 1e6,
					(double) margin / 1e6);
	}

	done.now_ns = now;
	done.step = impl->out->codec.step;
	done.amp = impl->out->codec.value;
	spa_loop_invoke(pw_context_get_main_loop(impl->ctx)->loop,
			rt_done_cb, 0, &done, sizeof(done), false, impl);

	/* Retire here. The queue belongs to this thread now, so recording
	 * what was written does not wait for a control loop that may be
	 * busy with graph work; the accounting invoke above is only for
	 * the log. */
	while (impl->out->codec.hwq_len > 0 && impl->out->codec.hwq[0].deadline <= now) {
		impl->out->codec.written = impl->out->codec.hwq[0].value;
		memmove(&impl->out->codec.hwq[0], &impl->out->codec.hwq[1],
				(impl->out->codec.hwq_len - 1) * sizeof(impl->out->codec.hwq[0]));
		impl->out->codec.hwq_len--;
	}
	hwq_arm(impl->out);
}
/* Runs on the control loop after a realtime write: retire the entries the
 * write covered, then arm the next one the ordinary way. */
int rt_done_cb(struct spa_loop *loop, bool async, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	struct impl *impl = user_data;
	const struct rt_done *done = data;
	struct timespec ts;
	uint64_t now;

	(void) loop; (void) async; (void) seq;
	if (size != sizeof(*done))
		return -EINVAL;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	now = (uint64_t) ts.tv_sec * 1000000000u + ts.tv_nsec;

	/* The codec has jumped to the newest level, so anything due between
	 * the write and now collapses into it -- the same rule the
	 * control-loop drain applies. */
	while (impl->out->codec.hwq_len > 0 && impl->out->codec.hwq[0].deadline <= now) {
		memmove(&impl->out->codec.hwq[0], &impl->out->codec.hwq[1],
				(impl->out->codec.hwq_len - 1) * sizeof(impl->out->codec.hwq[0]));
		impl->out->codec.hwq_len--;
	}

	if (impl->out->codec.hwq_len == 0)
		hold_update(impl);
	hwq_arm(impl->out);
	return 0;
}
void hwq_arm(struct output *out)
{
	struct impl *impl = out->impl;
	struct timespec ts;
	uint64_t due;

	if (out->codec.hwq_len == 0)
		return;

	/* When the sink can take a direct write, the timer belongs on the
	 * mixer's realtime loop and nothing here is armed: the control loop
	 * is out of the timing path entirely. */
	if (out->codec.ctl.ok && out->codec.loop != NULL) {
		/* A substituted control arrives with its own step: that is
		 * already the number the hardware wants, and snapping it onto
		 * a rung of a ladder that describes a different control would
		 * be a corruption, not a rounding. */
		long step = out->codec.hwq[0].step >= 0 ? out->codec.hwq[0].step :
			rt_amp_to_step(impl->out, out->codec.hwq[0].value);

		/* No usable scale, so no direct write: the ordinary route
		 * stays in charge, which is also the only route a device
		 * without a control ever has. */
		if (step >= 0 && rt_timer_ensure(impl->out) == 0) {
			/* Armed here, on the codec loop, from the queue: the
			 * request reached this thread from the mixer's own data
			 * loop, so no control loop hop sits between the cycle
			 * that decided the instant and the timer that meets it. */
			out->codec.deadline = out->codec.hwq[0].deadline;
			out->codec.step = step;
			out->codec.value = out->codec.hwq[0].value;
			out->codec.raise = out->codec.hwq[0].raise;
			out->codec.pending = true;
			{
				uint64_t now = now_ns();
				rt_arm_at(impl->out, rt_next_wake(out->codec.deadline, now));
				pw_log_debug("rt arm: step %ld, deadline +%.3f ms",
						step,
						(double) (out->codec.deadline > now ?
							(int64_t) (out->codec.deadline - now) / 1e6 : 0.0));
			}
			return;
		}
	}

	due = out->codec.hwq[0].deadline;
	ts.tv_sec = (time_t) (due / 1000000000u);
	ts.tv_nsec = (long) (due % 1000000000u);
	if (pw_loop_update_timer(pw_context_get_main_loop(impl->ctx),
			out->codec.hw_timer, &ts, NULL, true) < 0)
		pw_log_error("could not arm the codec write at %" PRIu64, due);
}
/* Runs on the control loop, handed over from the data loop by the mixer.
 *
 * The mixer supplies the instant and the direction only. The value is
 * resolved when the timer fires, not here: a slider move during absolute
 * playback changes the rung that the release must land on, and freezing the
 * number at decision time would restore a level the user has since moved
 * away from. */
/* The bump/dip harness still speaks the older payload shape; adapt it to
 * the same queue so the test exercises the real path. */
int swstep_request_cb(struct spa_loop *loop, bool async, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	const struct maemo_hw_change *ch = data;
	struct impl *impl = user_data;

	(void) loop; (void) async; (void) seq;
	if (size != sizeof(*ch))
		return -EINVAL;
	/* The harness has no per-output identity, so it drives whatever is
	 * current; the real path arms per output and never comes through
	 * here. */
	if (impl->out == NULL)
		return 0;
	return codec_request(impl->out, ch->deadline_ns, ch->value >= 1.0f);
}
int codec_request(void *user_data, uint64_t deadline_ns, bool raise)
{
	struct output *out = user_data;
	struct impl *impl = out->impl;

	/* With a substituted control the release cannot use the remembered
	 * rung: that number belongs to the announced control, which this
	 * window never touched. What has to go back is what the substituted
	 * control held before the raise, so read it here, at the opening
	 * edge only -- an overlapping window finds the control already at
	 * the raise level, and remembering that would strand it there. */
	if (raise && out->boost.sub && out->boost.restore_step < 0) {
		long step;
		if (rt_ctl_read(impl->out, &step))
			out->boost.restore_step = step;
	}

	/* On a substituted port both edges are raw steps, the one from
	 * configuration and the one read back, and the queue wants a scalar
	 * to order requests by. A step is monotone with loudness, so the step
	 * is used as that scalar and written as it stands: no conversion in
	 * either direction, and nothing from the ladder, which describes
	 * another control entirely. */
	long sub_step = -1;
	if (out->boost.sub) {
		sub_step = raise ? out->boost.step : out->boost.restore_step;
		if (sub_step < 0) {
			/* Nothing was read at the opening edge, so no level
			 * belongs to this control: leaving it where it is beats
			 * inventing one. */
			pw_log_info("codec release skipped: '%s' could not be read "
					"when the window opened", out->boost.elem);
			return 0;
		}
	}

	float desired;	int64_t margin;
	float written;
	size_t i, len;
	bool found = false;

	/* Runs on the codec loop when the direct control is live, which is
	 * the only thing that makes it a realtime path: the request reached
	 * this thread through the loop's own invoke ring, so there is no
	 * lock here and nothing that can be held by the control loop. The
	 * level bookkeeping below is read as atomics for the same reason. */

	if (out->codec.hwq_len >= HWQ) {
		/* Nokia prunes superseded entries rather than letting the
		 * queue fill. Dropping the oldest pending request is the
		 * conservative failure: the newest one reflects what the
		 * user is doing now. */
		pw_log_warn("codec queue full (%u); dropping the oldest", out->codec.hwq_len);
		out->codec.hwq_len--;
	}

	/* The value is resolved now rather than at arm time, and the release
	 * prefers what the control actually held when the window opened.
	 * Read at the opening edge only: with overlapping absolute streams
	 * the control is already at MAX when the second one starts, and a
	 * readback taken there would make MAX the release target and leave
	 * the codec stuck at full volume with nothing pending. */
	desired = sub_step >= 0 ? (float) sub_step :
			(raise ? vol_loadf(&out->boost.max) :
				(vol_loadb(&out->boost.slider_during) ||
					vol_loadf(&out->boost.restore) < 0.0f ?
					vol_loadf(&out->codec.amp) : vol_loadf(&out->boost.restore)));

	/* The deadline is the one the mixer asked for, `now + clock.delay` of
	 * the cycle whose samples the change applies to. Nothing is added for
	 * a sink's declared distance, an ALSA sink declares none and its delay
	 * is already on the graph clock. */

	/* Signed so a value can be expressed in either direction, but the
	 * convention is the reference's: "raise late, lower early". A later
	 * release is therefore a negative lower margin, which reads backwards
	 * against the intention -- noted rather than changed, because the
	 * margin also serves the spacing rule below and the two are entangled
	 * more deeply than looks obvious. */
	margin = raise ? impl->raise_margin_ns : impl->lower_margin_ns;

	if (!out->sink.sync) {
		/* A sink whose hardware cannot be timed still has both edges to
		 * meet, it just has nothing to aim them at.
		 *
		 * The software duck does not take effect where it is applied. It
		 * lands on samples that are then written to the device, and those
		 * are heard one buffer-depth later, so the hardware half has to
		 * arrive with them rather than ahead of them. Where the sink
		 * declares a distance the two halves can be aimed at it; where it
		 * does not, and a Bluetooth output declares none because its level
		 * is an AVRCP command whose latency is a D-Bus call, an over-the-air
		 * round trip and whatever the headset decides, the only honest
		 * instant is now, and the raise and the release both take it.
		 *
		 * Absolute, because that is what the queue holds, the mixer hands
		 * an instant and not an interval. A relative value here reads as a
		 * deadline in 1970 and the entry sits for seventeen seconds. */
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		deadline_ns = (uint64_t) ts.tv_sec * 1000000000u + ts.tv_nsec;
	}

	/* Nokia's push, transcribed. Three things happen here that my earlier
	 * version got wrong, and each of them matters:
	 *
	 * 1. A request that would restate the level the hardware already
	 *    holds, with nothing in flight, is dropped outright. Without it
	 *    every click writes the codec twice for no change.
	 * 2. The margin is not a fixed offset on the direction. It is applied
	 *    only when the shifted instant still lands beyond the entry being
	 *    compared against, walking back from the newest. That is what
	 *    keeps a burst of close-together requests from each pushing the
	 *    next one further out without bound.
	 * 3. The new entry is inserted in time order and everything *behind*
	 *    it is dropped -- "we can ignore volume events that came earlier
	 *    but should happen later than this".
	 *
	 * The one thing deliberately not copied is the rewind: Nokia rewrites
	 * the sample timeline when it shifts a change, and our timing comes
	 * from the graph clock instead, so there is nothing to rewind. */

	/* 1: no change, nothing in flight. */
	written = vol_loadf(&out->codec.written);
	if (out->codec.hwq_len == 0 && written >= 0.0f &&
	    fabsf(desired - written) < 1e-6f) {
		maemo_vol_state_set_hold(&out->vstate, false);
		return 0;
	}

	/* 2: walk back from the newest entry to the first one this change may
	 *    have to be shifted past. Unsigned arithmetic is avoided on the
	 *    lower side: a margin that would push the instant before zero is
	 *    not a margin, it is a bug waiting to wrap. */
	uint64_t asked_ns = deadline_ns;
	int behind = -1;
	size_t qlen_before = out->codec.hwq_len;

	for (i = out->codec.hwq_len; i > 0; i--) {
		unsigned int k = i - 1;
		float seen = out->codec.hwq[k].value;

		found = false;
		if (desired > seen) {
			if ((int64_t) deadline_ns + margin > (int64_t) out->codec.hwq[k].deadline) {
				deadline_ns = (uint64_t) ((int64_t) deadline_ns + margin);
				behind = (int) k;
				found = true;
				break;
			}
		} else if ((int64_t) deadline_ns > (int64_t) out->codec.hwq[k].deadline + margin) {
			deadline_ns = (uint64_t) ((int64_t) deadline_ns - margin);
			behind = (int) k;
			found = true;
			break;
		}
	}

	/* found == false means the walk ran off the front: nothing to sit
	 * behind. Note this is not the same as i == 0, which is what an
	 * earlier version tested -- with an empty queue the counter had
	 * already wrapped, and the entry landed outside the array. */
	if (!found) {
		/* Nothing to sit behind: aim against the level the hardware
		 * actually holds, not against a queued entry. */
		if (desired > out->codec.written)
			deadline_ns = (uint64_t) ((int64_t) deadline_ns + margin);
		else
			deadline_ns = ((int64_t) deadline_ns - margin > 0)
				? (uint64_t) ((int64_t) deadline_ns - margin) : 0;
		len = 1;
	} else {
		/* The entry found above -- index i - 1 -- is the one this
		 * request must sit *behind*, so it survives: the reference
		 * inserts after it and frees only what follows the new entry,
		 * which is everything scheduled later than the new one.
		 *
		 * Writing at index i - 1 instead, which an earlier version did
		 * while the comment already said otherwise, overwrites the
		 * entry being preserved. The visible failure was a release
		 * deleted by the next attack: the duck had already been lifted
		 * on the promise of that release, so the codec sat at maximum
		 * under un-ducked music. It showed as a bump on every click in
		 * a burst, and as a queue that never exceeded one entry. */
		len = i + 1;
	}
	out->codec.hwq[len - 1].deadline = deadline_ns;
	out->codec.hwq[len - 1].value = desired;
	out->codec.hwq[len - 1].raise = raise;
	out->codec.hwq[len - 1].step = sub_step;
	out->codec.hwq_len = len;

	{
		struct timespec ts;
		uint64_t now;
		char want[32];

		/* A substituted request carries a raw step, and printing it
		 * where an amplitude goes reads like a level of eleven. */
		if (sub_step >= 0)
			snprintf(want, sizeof(want), "step %ld", sub_step);
		else
			snprintf(want, sizeof(want), "%.4f", (double) desired);

		clock_gettime(CLOCK_MONOTONIC, &ts);
		now = (uint64_t) ts.tv_sec * 1000000000u + ts.tv_nsec;
		pw_log_debug("codec %s -> %s armed for +%.3f ms (queue %u)"
				" | margin %+.3f ms"
				" | sync %d | hw %.4f"
				" | asked +%.3f ms | shift %+.3f ms | behind %d | qlen %u",
				raise ? "raise" : "release", want,
				deadline_ns > now ? (double) (deadline_ns - now) / 1e6
						  : 0.0, out->codec.hwq_len,
				(double) margin / 1e6,
				out->sink.sync, (double) vol_loadf(&out->codec.amp),
				asked_ns > now ? (double) (asked_ns - now) / 1e6 : 0.0,
				(double) ((int64_t) deadline_ns - (int64_t) asked_ns) / 1e6,
				behind, (unsigned int) qlen_before);
	}
	hwq_arm(impl->out);
	return 0;
}
void hw_timer_cb(void *data, uint64_t expirations)
{
	struct impl *impl = data;

	if (impl->out == NULL)
		return;
	uint64_t now, last_due = 0;
	struct timespec ts;
	float value = -1.0f;
	unsigned int done = 0;

	(void) expirations;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	now = (uint64_t) ts.tv_sec * 1000000000u + ts.tv_nsec;

	if (impl->out->codec.hwq_len > SPA_N_ELEMENTS(impl->out->codec.hwq)) {
		pw_log_error("hardware queue length out of range: %u", impl->out->codec.hwq_len);
		impl->out->codec.hwq_len = 0;
	}

	/* Everything that is due is collapsed into the newest of them: two
	 * requests in the same direction are the same request, and a later
	 * one in the other direction supersedes the earlier. That is
	 * Nokia's pruning -- "we can ignore volume events that came earlier
	 * but should happen later than this" -- and it is what took his
	 * write count from 8 to 4 on a burst of four clicks. */
	while (impl->out->codec.hwq_len > 0 && impl->out->codec.hwq[0].deadline <= now) {
		value = impl->out->codec.hwq[0].value;
		last_due = impl->out->codec.hwq[0].deadline;
		/* Every due entry is drained before a single write is issued:
		 * the codec jumps to the newest level, it does not visit the
		 * intermediates on the way.
		 *
		 * Shift, do not swap: the array is kept in deadline order and
		 * "the newest due entry wins" depends on walking it in that
		 * order. Swapping the tail to the front would leave the queue
		 * unordered, and the entry armed next would be whichever
		 * happened to be last rather than the one due soonest. */
		memmove(&impl->out->codec.hwq[0], &impl->out->codec.hwq[1],
				(impl->out->codec.hwq_len - 1) * sizeof(impl->out->codec.hwq[0]));
		impl->out->codec.hwq_len--;
		done++;
	}

	if (done == 0) {
		hwq_arm(impl->out);
		return;
	}

	if (now > last_due) {
		pw_log_warn("codec write %.3f ms late (%u collapsed)",
				(now - last_due) / 1e6, done);
	}

	pw_log_info("codec -> %.4f (%u due, %u still queued)",
			(double) value, done, impl->out->codec.hwq_len);
	impl->out->codec.written = value;

	/* Nothing in this queue is a user level: it is fed only by the
	 * mixer's absolute-volume arming, so the drain never persists.
	 *
	 * On a substituted port the mirror is worse than pointless: a route
	 * param always lands on the control the route declares, which is the
	 * one the substitution exists to keep still. */
	if (!impl->out->boost.sub)
		write_route_volume(impl->out, value, false);

	/* Nothing owed: the mixer may stop asking for the extra cycles it
	 * only needed to arm this write. */
	if (impl->out->codec.hwq_len == 0)
		hold_update(impl);
	hwq_arm(impl->out);
}
int write_route_volume(struct output *out, float amp, bool save)
{
	struct impl *impl = out->impl;
	uint8_t buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	struct spa_pod_frame f[2];
	struct spa_pod *param;
	float vols[8];
	uint32_t n = out->route.channels;

	if (!dev_usable(impl->out)) {
		pw_log_debug("no device resolved yet; level %.4f not written",
				(double) amp);
		return -ENODEV;
	}

	if (n == 0 || n > 8)
		n = 2;

	/* The release that undoes a notification writes back the level the
	 * target already wrote, and every write is a route param change that
	 * pipewire-pulse announces as a sink volume change, so one key press
	 * woke the applet half a dozen times to learn what it already knew.
	 * A transient repeat of the level already there is dropped; a write
	 * that asks to persist is never dropped, so nothing can be lost. */
	if (!save && amp == out->route.written) {
		pw_log_debug("route volume already %.4f; not written", (double) amp);
		return 0;
	}

	for (uint32_t i = 0; i < n; i++)
		vols[i] = amp;

	spa_pod_builder_push_object(&b, &f[0],
			SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route);
	spa_pod_builder_add(&b,
			SPA_PARAM_ROUTE_index,  SPA_POD_Int(out->route.index),
			SPA_PARAM_ROUTE_device, SPA_POD_Int(out->route.dev_idx),
			0);
	spa_pod_builder_prop(&b, SPA_PARAM_ROUTE_props, 0);
	spa_pod_builder_push_object(&b, &f[1],
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_add(&b,
			SPA_PROP_channelVolumes,
			SPA_POD_Array(sizeof(float), SPA_TYPE_Float, n, vols), 0);
	spa_pod_builder_prop(&b, SPA_PROP_mute, 0);
	spa_pod_builder_bool(&b, false);	/* never muted from here */
	spa_pod_builder_pop(&b, &f[1]);

	/* Two independent questions, and conflating them is what let MAX
	 * into the per-device store: `save_requested` is whether this device
	 * participates in persistence at all, `save` is whether THIS value
	 * should be remembered. The absolute pair answers no to the second
	 * -- MAX is not a user level, and the release that follows it is
	 * only the undo of a transient, so neither belongs in the store.
	 * WirePlumber restores route volumes when a route becomes active
	 * again, so a device switched away from mid-notification comes back
	 * at the level the user set, provided MAX never got stored. */
	if (save && impl->save_requested) {
		spa_pod_builder_prop(&b, SPA_PARAM_ROUTE_save, 0);
		spa_pod_builder_bool(&b, true);
	}
	param = spa_pod_builder_pop(&b, &f[0]);

	/* `save` asks the store not to remember this level. It used to also
	 * pass a transient flag so the device would not announce the write,
	 * but this path is now the fallback -- when the module owns a control
	 * handle the write happens on the codec loop and never comes here --
	 * so there is nothing left to keep quiet and no reason to ask. */
	uint32_t flags = 0;
	int rc = pw_impl_device_set_param(out->route.dev, SPA_PARAM_Route, flags, param);
	if (rc < 0) {
		pw_log_error("route write failed dev=%p index=%u",
				out->route.dev, out->route.index);
		return -EIO;
	}

	out->route.written = amp;
	return 0;
}
