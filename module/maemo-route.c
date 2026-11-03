/* maemo mixer module, the route and the tuning.
 *
 * What the announced device says about its volume control and its tuning,
 * what we write back to it, and the metadata that carries both.
 *
 * SPDX-License-Identifier: MIT
 */

#include "maemo-mix-internal.h"

void apply_tuning(struct output *out, const char *csv)
{
	struct impl *impl = out->impl;
	const struct maemo_vol_map *map;

	if (csv == NULL || *csv == '\0')
		return;

	/* Parsed once per distinct curve. The ladders are static config,
	 * so this is not a cache that can go stale -- it only avoids
	 * re-doing work that can never produce a different answer. */
	map = maemo_ladder_cache_get(out->ladder_cache, csv);
	if (map == NULL) {
		pw_log_error("could not parse the announced tuning: %s", csv);
		return;
	}

	/* Borrowed from the cache, which outlives the module's use of it. */
	out->map = map;

	/* The ladder is ascending, so its last hardware step is the loudest
	 * level the slider can ask for, and that is the ceiling for an
	 * absolute sound as well. */
	vol_storef(&out->boost.max, maemo_vol_map_hw(map, map->n_ladder - 1));
	if (out->boost.max <= 0.0f)
		vol_storef(&out->boost.max, 1.0f);

	if (impl->debug)
		pw_log_info("tuning: %u rungs, top rung %.4f (%.1f dB) -- the ceiling "
			"for absolute volume, not the codec's own maximum",
			map->n_slider, (double) out->boost.max,
			out->boost.max > 0.0f ? 20.0 * log10(out->boost.max) : -999.0);
}
const char *tuning_for(struct output *out)
{
	struct impl *impl = out->impl;
	const struct pw_properties *p;
	const char *csv;
	struct pw_impl_node *sn;

	/* The ladder belongs to the output, not to us. The policy script
	 * announces on the sink which rungs this hardware can reach, and
	 * that is the only copy that can be right for the board in use:
	 * the d4 stops at -9 dB, and a generic curve that runs to 0 dB
	 * both overshoots the top and invents steps between its rungs.
	 * tuning.default is a fallback for a board with no policy, not a
	 * setting that outranks an announcement. */
	if ((sn = find_sink_node(out)) != NULL &&
	    (p = pw_impl_node_get_properties(sn)) != NULL &&
	    (csv = pw_properties_get(p, TUNING_KEY)) != NULL && *csv != '\0')
		return csv;

	/* Our own mixer is not asked. It carries a curve of its own, the
	 * module default, and on a board whose amplifier stops at -9 dB that
	 * curve reaches above the ceiling: a raise resolved from it writes
	 * full scale to the control and the release has nothing below it to
	 * come back to. The sink the policy announced is the only place a
	 * curve can be right, and the module default is the fallback for a
	 * board with no policy at all, not a second opinion. */
	return impl->default_tuning;
}
/* The optional pair announced with the ladder. Read on every route
 * announcement rather than once at startup: the pair belongs to the output,
 * and the output changes under the sink when a headset appears or a profile
 * is switched -- the same reason the element is read per announcement. */
void read_abs_pair(struct output *out)
{
	struct pw_impl_node *sn;
	const struct pw_properties *p;
	const char *elem, *val;

	out->boost.elem[0] = '\0';
	out->boost.step = -1;

	sn = find_sink_node(out);
	if (sn == NULL)
		return;

	p = pw_impl_node_get_properties(sn);
	if (p == NULL)
		return;

	elem = pw_properties_get(p, ABS_ELEMENT_KEY);
	if (elem == NULL || *elem == '\0')
		return;

	snprintf(out->boost.elem, sizeof(out->boost.elem), "%s", elem);

	val = pw_properties_get(p, ABS_VALUE_KEY);
	if (val != NULL && *val != '\0') {
		char *end = NULL;
		long step = strtol(val, &end, 10);

		if (end != val && end != NULL && *end == '\0')
			out->boost.step = step;
		else
			pw_log_warn("absolute control: '%s' is not a control step: %s",
					ABS_VALUE_KEY, val);
	}
}
struct pw_impl_device *find_device(struct pw_context *ctx, uint32_t id)
{
	struct pw_global *g = pw_context_find_global(ctx, id);

	if (g == NULL)
		return NULL;
	if (!pw_global_is_type(g, PW_TYPE_INTERFACE_Device))
		return NULL;
	return (struct pw_impl_device *) pw_global_get_object(g);
}
/* Is the device we hold still something we can talk to?
 *
 * A pw_impl_device can outlive the plugin object behind it. The global
 * stays findable while the spa_device it wraps is already destroyed,
 * and pw_impl_device_for_each_param enumerates params through that
 * pointer, so a device torn down by a headset disconnect turns the next
 * stream start into a null dereference inside the core. Test the
 * implementation rather than the pointer: the pointer keeps its value
 * and lies. */
bool dev_usable(struct output *out)
{
	return out->route.dev != NULL &&
		pw_impl_device_get_implementation(out->route.dev) != NULL;
}
/* The device going away is not an error, it is Tuesday: the headset
 * walks out of range and its device is destroyed under us. Drop the
 * reference so the next route announcement resolves a fresh one instead
 * of us holding on to a corpse. */
void on_dev_destroy(void *data)
{
	struct impl *impl = data;

	impl->out->route.dev = NULL;
	spa_hook_remove(&impl->out->route.dev_hook);
	spa_hook_remove(&impl->out->route.dev_info_hook);
	/* Whatever the route said before is now about a device that is gone,
	 * so the next announcement has to be answered even if it reads the
	 * same as the last one. */
	impl->out->route.last[0] = '\0';
}
/* Pull `key=<number>` out of the routing announcement, which is a flat
 * "sink=63 device=56 devidx=3 index=1".
 *
 * Matched on a space or the start of the string so "device=" cannot be
 * satisfied by the tail of "devidx=". */
bool payload_get_long(const char *p, const char *key, long *out)
{
	size_t klen = strlen(key);
	const char *v = p;

	while (v != NULL && *v != '\0') {
		if (spa_strneq(v, key, klen) && v[klen] == '=') {
			char *end = NULL;
			long n = strtol(v + klen + 1, &end, 10);
			if (end != v + klen + 1) {
				*out = n;
				return true;
			}
		}
		v = strchr(v, ' ');
		while (v != NULL && *v == ' ')
			v++;
	}
	return false;
}
struct route_vol {
	uint32_t want;
	bool found;
	float amp;
};
int route_param_cb(void *data, int seq, uint32_t id, uint32_t index,
		uint32_t next, struct spa_pod *param)
{
	struct route_vol *r = data;
	struct spa_pod_object *obj;
	/* The find_prop macro resumes from its iterator argument, so each
	 * one has to start at NULL or it reads garbage as a position. */
	const struct spa_pod_prop *iprop = NULL, *dprop = NULL, *pprop = NULL,
		*vprop = NULL;
	int32_t route_index = -1;
	uint32_t direction = 0;
	float ch[1] = { 0.0f };

	(void) seq; (void) index; (void) next;

	if (id != SPA_PARAM_Route || param == NULL || r->found)
		return 0;

	obj = (struct spa_pod_object *) param;

	iprop = spa_pod_object_find_prop(obj, iprop, SPA_PARAM_ROUTE_index);
	if (iprop == NULL ||
	    spa_pod_get_int(&iprop->value, &route_index) != 0 ||
	    (uint32_t) route_index != r->want)
		return 0;

	/* A card numbers its routes across both directions, so the index on
	 * its own does not identify the one we drive: the ALSA card's route
	 * 0 is "analog-input-mic" while its output is route 5. Only the
	 * output route carries the level we care about.
	 *
	 * Direction is carried as SPA_POD_Id -- alsa-acp-device.c and
	 * bluez5-device.c both build it that way -- so it has to be read
	 * with the id accessor; spa_pod_get_int on an Id pod fails the type
	 * check rather than coercing. */
	dprop = spa_pod_object_find_prop(obj, dprop, SPA_PARAM_ROUTE_direction);
	if (dprop == NULL ||
	    spa_pod_get_id(&dprop->value, &direction) != 0 ||
	    direction != (uint32_t) SPA_DIRECTION_OUTPUT)
		return 0;

	pprop = spa_pod_object_find_prop(obj, pprop, SPA_PARAM_ROUTE_props);
	if (pprop == NULL || !spa_pod_is_object(&pprop->value))
		return 0;

	vprop = spa_pod_object_find_prop((struct spa_pod_object *) &pprop->value,
			vprop, SPA_PROP_channelVolumes);
	if (vprop == NULL)
		return 0;

	/* A route carries one value per channel. spa_pod_copy_array checks
	 * the element type and width against what is asked for and copies
	 * the raw values, so a route reporting something other than floats
	 * is refused instead of being reinterpreted. */
	if (spa_pod_copy_array(&vprop->value, SPA_TYPE_Float, ch, 1) < 1)
		return 0;

	r->amp = ch[0];
	r->found = true;
	return 0;
}
/* The hardware level currently in force on the route we drive.
 *
 * This is what WirePlumber's route store restored at startup, or what the
 * codec has been holding since before this module loaded. Read rather
 * than assumed, so the level on screen can be made to agree with the
 * level in the amplifier instead of both of them guessing. */
float read_route_volume(struct output *out)
{
	struct impl *impl = out->impl;
	struct route_vol r = { .want = out->route.index, .amp = -1.0f };

	/* Negative means "unknown", and callers must treat it as such.
	 *
	 * It used to answer unity, which reads as a level and therefore gets
	 * adopted as one: on a headset appearing or disappearing the new
	 * route has no stored volume yet, WirePlumber applies it a moment
	 * later, and the adoption marched the slider to the top rung on the
	 * strength of a default. Not knowing is not the same as full volume. */
	if (!dev_usable(impl->out))
		return -1.0f;

	pw_impl_device_for_each_param(out->route.dev, 0, SPA_PARAM_Route, 0,
			UINT32_MAX, NULL, route_param_cb, &r);

	if (!r.found && impl->debug)
		pw_log_info("no volume on route %u yet, leaving the level alone",
				out->route.index);

	return r.amp;
}

/* Ask WirePlumber to remember this route's level.
 *
 * The route already carries the level: ACP watches the element and mirrors
 * it into the route's channelVolumes, which is where read_route_volume()
 * finds it. What it does not do is remember it. state-routes.lua stores a
 * route's props only when the route param says `save`, and a level the
 * module puts on the element itself never says so, so a port torn down and
 * re-enabled came back at the driver default with nothing to restore from.
 *
 * Naming the route that is already active with `save` and no props is the
 * cheap way to set that flag: acp_device_set_port() marks the port and
 * returns before it re-applies anything, and alsa-acp-device only applies
 * props when there are any, so the sequence UCM declared for this port
 * runs no again and no volume is written. The flag stays until the port
 * changes, so every later move of the element is stored as it happens. */
void mark_route_saveable(struct output *out)
{
	uint8_t buf[256];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	struct spa_pod *param;

	if (!dev_usable(out))
		return;

	param = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route,
			SPA_PARAM_ROUTE_index,  SPA_POD_Int(out->route.index),
			SPA_PARAM_ROUTE_device, SPA_POD_Int(out->route.dev_idx),
			SPA_PARAM_ROUTE_save,   SPA_POD_Bool(true));

	if (param == NULL)
		return;

	if (pw_impl_device_set_param(out->route.dev, SPA_PARAM_Route, 0, param) < 0)
		pw_log_debug("route %u on device %p refused the save flag",
				out->route.index, (void *) out->route.dev);
}
/* The device's params moved, which is how WirePlumber telling the route to
 * a new volume reaches us.
 *
 * The route store keeps a level per output and applies it when the output
 * becomes active, and that apply lands a moment after the policy mentions
 * the route, so reading the level at the announcement reads the previous
 * output's value out of a register both outputs share. This is the event
 * that says the number has actually moved, and re-running the adoption
 * then takes the level WirePlumber put there rather than the one we
 * carried.
 *
 * Two cases stay out of it. While an absolute stream plays the codec holds
 * MAX because of our own duck, so there is nothing honest to read. And
 * once the applet has named a target the user has spoken, so a route
 * volume that disagrees with it is news about somebody else's output. */
void on_dev_info_changed(void *data, const struct pw_device_info *info)
{
	struct impl *impl = data;

	if (info == NULL || !(info->change_mask & PW_DEVICE_CHANGE_MASK_PARAMS))
		return;
	if (vol_loadb(&impl->out->boost.active) || impl->app_target != NULL)
		return;

	reread_target(impl->out);
}
/* Make the level a route carries into the slider's level.
 *
 * The route is where the hardware's own level shows up: ACP watches the
 * element and mirrors it into the route's channelVolumes, WirePlumber puts
 * a stored level back onto the route when it becomes active, and the
 * element is the thing the user is hearing, so this is the one number that
 * asks the same question on this output and on the one a switch moves to.
 *
 * Two cases stay out of it. Once the applet has named a target the user has
 * spoken, and a level that disagrees with it is news about somebody else's
 * output. And while an absolute stream plays the codec holds MAX because of
 * our own duck, so there is nothing honest to adopt.
 *
 * A negative amplitude means the route has no level yet, which is a
 * statement about ordering and not a level: the device's params move again
 * when it has one, and this returns without touching anything. */
void adopt_route_level(struct output *out, float amp)
{
	struct impl *impl = out->impl;
	int step, hw_step;

	if (amp < 0.0f) {
		if (impl->debug)
			pw_log_info("no level on route %u yet; waiting for the params",
				    out->route.index);
		return;
	}
	if (impl->app_target != NULL || vol_loadb(&out->boost.active))
		return;
	if (out->map == NULL)
		return;

	step = maemo_vol_map_step_for_amp(out->map, amp);
	if (step < 0)
		return;

	hw_step = maemo_vol_map_hw_step(out->map, (unsigned) step);
	impl->slider_step = step;

	/* The rung is not enough. The absolute release restores hw_amp and the
	 * no-change guard compares it against hw_written, so an adoption that
	 * records only the rung leaves both at their initialisation value -- 1.0
	 * -- and the first notification then finds the release to be a no-change
	 * and drops it, stranding the codec at MAX until something writes a real
	 * target. Measured: every guard firing in a cold-start log read
	 * hw_amp 1.0000.
	 *
	 * Taking the amplitude from the ladder rather than storing `amp` keeps
	 * the invariant that the duck is the rung's amplitude, so duck x MAX is
	 * the level the slider shows.
	 *
	 * hw_written is only true while the control actually holds that level.
	 * During an absolute episode it holds MAX because of our own duck, and
	 * adopting it as "what we wrote" would be the same lie by another route. */
	vol_storef(&out->codec.amp, maemo_vol_map_hw(out->map, hw_step));
	if (!vol_loadb(&out->boost.active))
		vol_storef(&out->codec.written, out->codec.amp);

	if (impl->debug)
		pw_log_info("adopted %.4f from '%s' -> rung %d (duck %.4f)",
			    (double) amp, out->sink.name, step,
			    (double) out->codec.amp);

	/* The level and the curve that gives it meaning go up together: the
	 * applet greys the slider until a ladder arrives, and a ladder without
	 * a level would be a control ready to move the hardware to a rung
	 * nobody chose. */
	out->level_known = true;
	publish_sink_props(out);
}

void reread_target(struct output *out)
{
	struct impl *impl;
	const char *value;
	long raw;
	float n, amp, hw_amp;
	int step, hw_step;

	if (out == NULL)
		return;

	impl = out->impl;

	apply_tuning(out, tuning_for(out));

	if (out->map == NULL)
		return;

	/* Nothing has been asked for yet -- a fresh session, or an output just
	 * switched to. Adopt whatever the route underneath carries; see
	 * adopt_route_level() for what stays out of it and why a missing level
	 * is a wait rather than a default. */
	if (impl->app_target == NULL) {
		adopt_route_level(out, read_route_volume(out));
		return;
	}

	value = impl->app_target;
	if (sscanf(value, "%ld", &raw) != 1) {
		pw_log_warn("ignoring a target that is not a number: '%s'", value);
		return;
	}

	n = (float) raw / 65536.0f;
	if (n < 0.0f)
		n = 0.0f;
	if (n > 1.0f)
		n = 1.0f;
	amp = n * n * n;

	step = maemo_vol_map_step_for_amp(out->map, amp);
	if (step < 0)
		return;

	impl->slider_step = step;

	apply_silence(impl->out);

	/* The rung picks the hardware step, and the hardware step's own
	 * amplitude is what gets written. Going through the ladder rather
	 * than writing `amp` directly is the difference between a slider
	 * with rungs and a fader: the user's position, the level the
	 * amplifier is asked for and the level any later reader reports
	 * are then all the same rung, and nothing has to be reconciled
	 * afterwards.
	 *
	 * The software gain is deliberately not touched here. It stays at
	 * unity, so what the codec is asked for is what is heard. */
	hw_step = maemo_vol_map_hw_step(out->map, (unsigned) step);
	hw_amp = maemo_vol_map_hw(out->map, hw_step);

	/* While an absolute stream plays the codec is pinned at MAX and the
	 * slider moves this number instead -- the software duck takes the
	 * codec out of the product and puts it back in when the last one
	 * stops. So the duck and the codec target are the same variable,
	 * which is the only way the invariant "duck x MAX == slider level"
	 * is true by construction rather than by care. */
	vol_storef(&out->codec.amp, hw_amp);
	/* On a substituted port the duck is a constant resolved when the
	 * control was bound, and the slider does not belong to that stage at
	 * all: the move goes straight to the route, which is the control the
	 * user is actually talking to. */
	if (vol_loadb(&out->boost.active) && !out->boost.sub) {
		/* At the silence rung the ducked streams are silent, not
		 * merely at the bottom of the ladder: the notification keeps
		 * playing at MAX and everything else goes to zero. */
		float duck = maemo_vol_map_is_silence(out->map, step) ?
			0.0f : hw_amp / vol_loadf(&out->boost.max);
		vol_storeb(&out->boost.slider_during, true);
		if (duck > 1.0f)
			duck = 1.0f;
		maemo_vol_state_publish(&out->vstate, duck, true);
	}
	else {
		vol_storef(&out->codec.written, hw_amp);
		write_route_volume(impl->out, hw_amp, true);
	}

	/* The rung we landed on, not the number the applet asked with: the
	 * slider and the codec should agree on the rung, not converge on it
	 * by accident. */
	publish_target(impl->out);

	if (impl->debug)
		pw_log_info("target %ld -> amp %.4f -> rung %d -> hw step %d -> codec %.4f",
				raw, (double) amp, step, hw_step, (double) hw_amp);
}
/* Put the ladder on the node that carries the volume.
 *
 * The applet reads the curve off the sink node's properties and disables
 * its slider when there is none -- there is nothing to map a slider
 * position to otherwise, and a write made from that state would land on a
 * level derived from nothing. So the node that owns the level has to
 * publish the curve, or the control for it goes grey.
 *
 * Only filled where the node is silent: a per-device rule that gave this
 * sink a curve of its own wins, and overwriting it would throw away the
 * one thing a device-specific ladder is for. */
void publish_tuning_on(struct output *out, struct pw_impl_node *node)
{
	struct impl *impl = out->impl;
	struct pw_properties *upd;
	const struct pw_properties *have;

	if (node == NULL)
		return;

	upd = pw_properties_new(NULL, NULL);
	if (upd == NULL)
		return;
	have = pw_impl_node_get_properties(node);

	if (impl->default_tuning != NULL &&
	    pw_properties_get(have, TUNING_KEY) == NULL)
		pw_properties_set(upd, TUNING_KEY, impl->default_tuning);

	if (impl->default_tuning_incall != NULL &&
	    pw_properties_get(have, TUNING_INCALL_KEY) == NULL)
		pw_properties_set(upd, TUNING_INCALL_KEY,
				impl->default_tuning_incall);

	if (upd->dict.n_items > 0)
		pw_impl_node_update_properties(node, &upd->dict);

	pw_properties_free(upd);
}
/* The routing policy's announcement of where the audio goes.
 *
 * Logged, not acted on: a mixer with a volume does not care which device
 * is underneath it. It is watched because the announcement is the proof
 * that the routing side is alive, and a silent stack is otherwise hard to
 * tell apart from a misconfigured one. */
/* The routing policy's announcement of where the audio goes, in the form
 *   sink=<node id> device=<id> devidx=<n> index=<n>
 *
 * Two things come from it, and only one of them is about routing.
 *
 * The sink id names the node the applet is looking at. The default sink
 * deliberately stays the physical device -- it has to remain a truthful
 * statement of what WirePlumber resolved, and streams reach the mixer by
 * their own target.node instead -- so the node the applet reads its
 * ladder from is this one, not our mixer. Fill the curve onto it as
 * well, or the slider has no ladder and goes grey.
 *
 * The announcement itself is logged because it is the proof the routing
 * side is alive; a silent stack is otherwise hard to tell apart from a
 * misconfigured one. Nothing about the mix depends on which device is
 * underneath. */
/* Which output an announced device belongs to.
 *
 * One output today, so this always answers the same one whatever is
 * announced. It exists as the seam, with per-output mixers there is one
 * struct output per card and the announced device id picks which of them the
 * announcement belongs to, which is the only new decision enumeration adds
 * to this path.
 */
struct output *output_for_device(struct impl *impl, uint32_t dev_id)
{
	struct output *o;
	struct pw_impl_device *dev = find_device(impl->ctx, dev_id);

	if (dev == NULL)
		return impl->out;

	spa_list_for_each(o, &impl->outputs, link) {
		if (o->card_dev == dev)
			return o;
	}
	return impl->out;
}

int on_route_property(void *data, uint32_t subject,
		const char *key, const char *type, const char *value)
{
	struct impl *impl = data;
	if (impl->out == NULL)
		return 0;
	long sink_id = -1;

	(void) subject; (void) type;

	if (key == NULL || strcmp(key, ROUTE_KEY) != 0)
		return 0;

	if (value == NULL) {
		pw_log_info("route announced: (cleared)");
		impl->out->route.last[0] = '\0';
		return 0;
	}

	/* The same answer twice is not a change. Policy re-announces on
	 * everything that could move the route and during a click burst
	 * nothing has moved, so this is the common case by a wide margin;
	 * answering it anyway means resolving the device, binding the
	 * control and writing the level to the codec, which is a hardware
	 * write and a device re-announce and a fresh pile of events. */
	if (impl->out->route.last[0] != '\0' && strcmp(impl->out->route.last, value) == 0)
		return 0;
	snprintf(impl->out->route.last, sizeof(impl->out->route.last), "%s", value);


	long dev = -1, devidx = -1, index = -1;

	if (!payload_get_long(value, "device", &dev) ||
	    !payload_get_long(value, "index", &index)) {
		pw_log_warn("routing announcement missing device/index: %s", value);
		return 0;
	}
	bool have_devidx = payload_get_long(value, "devidx", &devidx);

	struct output *o = output_for_device(impl, (uint32_t) dev);

	/* The path as it was before this announcement, taken from the output
	 * that was current and before the fields below are overwritten. The
	 * whole path identifies the level, not just the card: a card numbers its
	 * routes per port and per verb, so a 3.5 mm plug arriving moves the
	 * route under a sink name that did not change, and the level
	 * WirePlumber restored for the new route is as much a stranger as the
	 * level of another card.
	 */
	struct output *prev_out = impl->out;
	char prev_sink[sizeof(prev_out->sink.name)];
	uint32_t prev_index = prev_out->route.index;
	uint32_t prev_devidx = prev_out->route.dev_idx;
	uint32_t prev_dev = prev_out->route.announced_id;
	bool had_path = prev_out->route.announced_set;

	snprintf(prev_sink, sizeof(prev_sink), "%s", prev_out->sink.name);

	/* The announcement names the card the user is now heard on, which is
	 * also the card the applet's next write belongs to. Handing the
	 * current output over is what makes every module-wide path, the
	 * slider, the level publication, the duck, follow it. */
	output_adopt(impl, o);
	o->route.index = (uint32_t) index;
	/* The index of the device inside the route parameter belongs to the
	 * card that was announced, and it has to be stored after the adoption:
	 * before it, `impl->out` is still the card that used to be current,
	 * and writing there leaves the new output aiming its route writes at
	 * device index 0, which is a write that goes nowhere. */
	o->route.dev_idx = have_devidx ? (uint32_t) devidx : 0;
	o->route.written = -1.0f;	/* a new route knows nothing */
	o->route.announced_id = (uint32_t) dev;

	/* Unlink from the device we were watching before we forget it.
	 *
	 * spa_hook_remove is not idempotent on this codebase: spa_list_remove
	 * leaves the removed element's links pointing at the neighbours it
	 * unlinked from, so a second remove walks into the object that owned
	 * the list and, if that object has since been freed, writes a pointer
	 * pair into released memory. Every hook must therefore be removed
	 * exactly once, by whoever last held the object -- either the destroy
	 * handler, which nulls o->route.dev, or this path, which only sees a
	 * non-null dev when the device is still alive. */
	if (o->route.dev != NULL) {
		spa_hook_remove(&o->route.dev_hook);
		spa_hook_remove(&o->route.dev_info_hook);
	}
	impl->out->route.dev = find_device(impl->ctx, impl->out->route.announced_id);

	/* A global can outlive the object behind it during teardown: the id
	 * the policy announced a moment ago can resolve to a device that is
	 * on its way out, and arming a listener on that writes into memory
	 * the core is about to release, which is a pointer pair landing in
	 * whatever the allocator hands out next. Refuse the shell. */
	if (impl->out->route.dev != NULL &&
	    pw_impl_device_get_implementation(impl->out->route.dev) == NULL) {
		pw_log_warn("announced device %u has no implementation; ignoring",
				impl->out->route.announced_id);
		impl->out->route.dev = NULL;
	}

	/* Re-armed on every route change because the route can move to a
	 * different device, and the old one's destruction must not be able
	 * to clear a reference we no longer hold. */
	if (impl->out->route.dev != NULL)
		pw_impl_device_add_listener(impl->out->route.dev, &impl->out->route.dev_hook,
				&dev_events, impl);

	/* The route is what decides which hardware carries this stream, so
	 * it is also what decides which control the codec path has to
	 * write; ask the device rather than trusting the config line. */
	bind_announced_control(impl->out);

	/* Watch the level WirePlumber puts on this route. Re-armed because
	 * the announcement can name a different device, and the hook belongs
	 * to the one we are now listening to. */
	if (impl->out->route.dev != NULL)
		pw_impl_device_add_listener(impl->out->route.dev, &impl->out->route.dev_info_hook,
				&dev_info_events, impl);

	pw_log_info("route announced: %s (device %s)", value,
			impl->out->route.dev ? "resolved" : "NOT FOUND");

	/* Measurement hook: put the hardware at a named level once, through
	 * the route -- which is the only channel that survives. An external
	 * amixer write gets re-asserted away within seconds by the very
	 * mechanism that makes the route the right channel, so a test that
	 * sets the codec from outside silently tests nothing. */
	{
		static bool test_route_done;
		const char *tr = getenv("MAEMO_TEST_ROUTE");
		if (!test_route_done && tr != NULL && *tr != '\0') {
			test_route_done = true;
			pw_log_info("test: route forced to %s once", tr);
			write_route_volume(impl->out, (float) atof(tr), true);
		}
	}

	/* Remember which output this is, by name. */
	if (payload_get_long(value, "sink", &sink_id) && sink_id > 0) {
		struct pw_global *g = pw_context_find_global(impl->ctx,
				(uint32_t) sink_id);
		if (g != NULL && pw_global_is_type(g, PW_TYPE_INTERFACE_Node)) {
			const struct pw_properties *np = pw_impl_node_get_properties(
					(struct pw_impl_node *) pw_global_get_object(g));
			const char *nm = np ? pw_properties_get(np, PW_KEY_NODE_NAME) : NULL;

			if (nm != NULL) {
				snprintf(impl->out->sink.name, sizeof(impl->out->sink.name),
						"%s", nm);
			}
		}
	}

	/* A different output is not the same question as a slider move.
	 *
	 * PipeWire and WirePlumber keep a level per route and put it back when
	 * the route comes active, so the control we have just been handed
	 * already holds the level that route was last left at, and the target we
	 * are carrying belongs to the route that just went away. Imposing it is
	 * what made a headset arrive at the speaker's loudness, and a 3.5 mm
	 * plug arriving at the speaker's was no better. Drop it and let the
	 * adoption path below read the truth off the hardware and publish it,
	 * which moves the slider to meet the new route instead of the other way
	 * round.
	 *
	 * The route counts as changed when any part of the path moved, the card,
	 * the device index inside the route parameter, the route index on it, or
	 * the sink the policy named, because any of them can be the only thing
	 * that differs -- a port swap keeps the sink and the card and moves only
	 * the index, a call keeps the port and moves only the verb.
	 */
	if (had_path && !vol_loadb(&impl->out->boost.active) &&
	    (prev_out != o ||
	     !spa_streq(prev_sink, impl->out->sink.name) ||
	     prev_index != o->route.index ||
	     prev_devidx != o->route.dev_idx ||
	     prev_dev != o->route.announced_id)) {
		pw_log_info("path changed '%s' -> '%s' (route %u -> %u); dropping "
				"the carried target and adopting what the hardware "
				"holds",
				prev_sink, impl->out->sink.name,
				prev_index, o->route.index);
		if (impl->app_target != NULL) {
			free(impl->app_target);
			impl->app_target = NULL;
		}
		/* Whatever level was known belonged to the output that went
		 * away, whether the applet ever named it or not. The control
		 * goes back to grey until this route says where it sits, so the
		 * applet is never shown a level from a port nobody is listening
		 * to, which is what made a plug move the slider twice. */
		o->level_known = false;
	}

	/* The curve and the level go onto the sink the applet is reading. */
	publish_sink_props(impl->out);


	/* And look at the codec control again, because the two announcements
	 * that describe a route are not ordered against each other. The route
	 * parameter arrives from the device and the sink name arrives from the
	 * policy, and on a call the device wins by a few milliseconds, which
	 * means the bind above read the pair -- the configured absolute
	 * control, see ABS_ELEMENT_KEY -- off the node the call just replaced.
	 * Re-binding is cheap and returns immediately when nothing moved. */
	bind_announced_control(impl->out);
	read_sync_flag(impl->out);

	/* Ask for this route's level to be remembered, once, so the next time
	 * the port comes up there is something to come back to. */
	mark_route_saveable(impl->out);

	/* Whatever we believed the control holds belonged to the device that
	 * just stopped being the output. Carrying it over would let the
	 * no-change guard compare a transition against a level this control
	 * has never been sent -- and a swallowed release leaves the codec at
	 * MAX with nothing pending, which is the worst possible outcome of a
	 * headset appearing mid-notification. reread_target() below re-establishes
	 * it if the device is reachable and no absolute stream is running. */
	impl->out->codec.written = -1.0f;

	/* A new route can bring a new ladder with it, and the user's target
	 * means a different rung on a different curve, so re-apply it now
	 * rather than waiting for the next slider tick. This is also the
	 * first moment the codec is reachable at all, so on a cold start it
	 * is what puts the hardware where the slider already is. */
	reread_target(impl->out);
	o->route.announced_set = true;
	/* The announced card is known now, which is what tells the
	 * enumeration which card is not this one. */
	outputs_sync(impl);
	return 0;
}







/* ---- our own sink ---- */

/* Create the mixer node in this process.
 *
 * Created with resource == NULL, so the node is locally owned rather than
 * made on behalf of a client and does not go away when a client
 * disconnects -- the point of a sink that is always there.
 */
int create_virtual_sink(struct output *out, const char *name)
{
	struct impl *impl = out->impl;
	struct pw_impl_factory *f;
	struct pw_impl_node *node;
	struct pw_properties *props;
	char dev_id[16] = "0";

	if (out->card_dev != NULL)
		snprintf(dev_id, sizeof(dev_id), "%u",
			pw_global_get_id(pw_impl_device_get_global(out->card_dev)));

	snprintf(out->sink.mixer_name, sizeof(out->sink.mixer_name), "%s", name);

	f = pw_context_find_factory(impl->ctx, "spa-node-factory");
	if (f == NULL) {
		pw_log_error("no spa-node-factory in this context; cannot create the virtual sink");
		return -ENOENT;
	}

	props = pw_properties_new(
		SPA_KEY_FACTORY_NAME, "audio.maemo.mixer",
		/* Which volume state this mixer reads: one per output, found by
		 * the name this node carries. See mixer/maemo-vol-state.h. */
		"maemo.vstate", name,
		PW_KEY_MEDIA_CLASS, "Audio/Sink",
		PW_KEY_MEDIA_TYPE, "Audio",
		PW_KEY_NODE_NAME, name,
		PW_KEY_NODE_DESCRIPTION, "Maemo Mixer",
		PW_KEY_NODE_NICK, "Maemo Mix",
		/* Not virtual. The name is a leftover from when this node was
		 * a probe's fake sink; claiming to be virtual tells
		 * WirePlumber this is a protocol-side sink with no device
		 * behind it, and whether it then builds a session item for
		 * the node is up to its rules, which is a coin toss we do not
		 * get to lose: no session item means no relay, the node is
		 * left with the dummy driver as its only neighbour, and every
		 * stream aimed at it plays into that. This node is the mixer
		 * for a real card, with a device.id, a route and a control. */
		PW_KEY_NODE_VIRTUAL, "true",
		/* Not a stream, and nothing to remember about it.
		 *
		 * WirePlumber's state-stream.lua takes any Audio class node
		 * without device.routes for a playback stream, which a
		 * mixer matches exactly, and so every Props change on this
		 * node was serialised into the state store: a slider touch
		 * and every absolute edge, measured at 231 saves in a
		 * twelve second click burst on the device.
		 *
		 * The script reads this key straight off the node's own
		 * properties. The documented stream.rules setting was tried
		 * first and did not reach the script from a drop-in at all,
		 * so the answer lives with the node rather than in a file
		 * that has to be installed everywhere. */
		"state.restore-props", "false",
		/* Must be able to go idle. A node created directly by a
		 * module never gets WirePlumber's session management, so
		 * without these it stays running forever: it holds the
		 * downstream ALSA sink running and the ring keeps filling
		 * with silence. */
		PW_KEY_NODE_PAUSE_ON_IDLE, "true",
		/* Runnability, not activeness, is what suspends a node. The
		 * ALSA sink is always active, and an active peer joined by
		 * an ordinary link keeps it runnable for as long as this
		 * node exists, so the PCM never closes. Making the output
		 * passive takes this node out of that propagation; a stream
		 * that really is playing still wakes the ALSA sink through
		 * the mixer's own driver. */
		PW_KEY_NODE_PASSIVE, "out",
		/* The mixer only gets input ports from the core in the
		 * mix-handle role. Declared here so streams have somewhere
		 * to link, with the channel layout the core matches on. */
		"maemo.sink.channels", "2",
		/* Which card this mixer belongs to, as the id the policy
		 * already uses for the device sinks, so a mixer can be found
		 * by the same test that finds a sink. */
		"device.id", dev_id,
		NULL);
	if (props == NULL)
		return -ENOMEM;

	node = pw_impl_factory_create_object(f, NULL,
			PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, props, 0);
	if (node == NULL) {
		pw_log_error("could not create the virtual sink from audio.maemo.mixer: %s",
			     spa_strerror(-errno));
		pw_properties_free(props);
		return -errno;
	}

	out->sink.mixer = node;
	{
		struct pw_global *g = pw_impl_node_get_global(node);
		if (g != NULL) {
			out->sink.bound_id = pw_global_get_id(g);
			pw_log_info("virtual sink bound id %u", out->sink.bound_id);
		}
	}
	pw_log_info("virtual sink created: %s (%p)", name, (void *) node);

	/* The crossing. From now on the mixer can ask for a codec write at
	 * an instant it computed itself; until this happens it switches its
	 * software gain and arms nothing. */
	/* Nothing is published on this node. The curve and the level belong
	 * to the output sink; see publish_sink_props(). All that can be done
	 * here is read a fallback, so a target arriving before the routing
	 * policy has named anything still has a curve to land on. */
	apply_tuning(out, tuning_for(out));

	/* State the unity explicitly rather than leaving it implicit in a
	 * zeroed struct: this is the software gain, and the fact that it is
	 * 1.0 is a decision, not an absence. */
	set_level(out, 1.0f);
	return 0;
}





const struct pw_impl_device_events dev_info_events = {
	PW_VERSION_IMPL_DEVICE_EVENTS,
	.info_changed = on_dev_info_changed,
};
const struct pw_impl_device_events dev_events = {
	PW_VERSION_IMPL_DEVICE_EVENTS,
	.destroy = on_dev_destroy,
};

void maemo_card_name(struct pw_impl_device *dev, char *name, size_t len)
{
	const struct pw_properties *p = dev ? pw_impl_device_get_properties(dev) : NULL;
	const char *nick = p ? pw_properties_get(p, "device.nick") : NULL;
	const char *dn = p ? pw_properties_get(p, "device.name") : NULL;
	char raw[128];
	size_t i = 0, j = 0;

	/* The nick is what the card calls itself, "Mapphone Audio" and "HDMI"
	 * here, which is the name a mixer should carry. The device name is the
	 * fallback, with the monitor prefix gone, because the suffix a monitor
	 * appends, `.3.auto`, says how the kernel found the card and nothing
	 * about what it is. */
	if (nick != NULL && nick[0] != '\0')
		snprintf(raw, sizeof(raw), "%s", nick);
	else if (dn != NULL && strchr(dn, '.') != NULL)
		snprintf(raw, sizeof(raw), "%s", strchr(dn, '.') + 1);
	else
		snprintf(raw, sizeof(raw), "default");

	/* A node name, so lower case and nothing a shell or a config file
	 * would have to quote. Spaces become underscores because that is the
	 * shape of the names the policy and the applet already match on. */
	for (; raw[i] != '\0' && j + 1 < len; i++) {
		char c = raw[i];

		if (c >= 'A' && c <= 'Z')
			c += 'a' - 'A';
		if (c == ' ' || c == '/' || c == '(' || c == ')')
			c = '_';
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
		      c == '_' || c == '.' || c == '-'))
			continue;
		name[j++] = c;
	}
	name[j] = '\0';

	if (j == 0)
		snprintf(name, len, "default");
}
