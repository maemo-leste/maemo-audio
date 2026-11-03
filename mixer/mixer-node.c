/* The node as the core sees it.
 *
 * Everything here answers a question about the mixer as a whole: what
 * parameters it has, where its io vectors go, what it does with a
 * command, who is listening. Anything about one port lives in
 * mixer-port.c; anything that runs every quantum lives in
 * mixer-process.c.
 *
 * mixer_node_methods at the bottom is the whole outward face of the
 * plugin -- the core reaches everything else through it.
 */

#include <stdio.h>
#include "mixer-impl.h"

#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic
SPA_LOG_TOPIC_DEFINE_STATIC(log_topic, "spa.maemo.mixer");

static int mixer_node_enum_params(void *object, int seq,
				 uint32_t id, uint32_t start, uint32_t num,
				 const struct spa_pod *filter)
{
	struct impl *this = object;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_result_node_params result;
	uint32_t count = 0;
	/* The EnumFormat answer is built here rather than inside the block
	 * that composes it, because the pod we hand on to spa_pod_filter and
	 * to the emit is a pointer into this storage: building it in the
	 * block left the pointer pointing at a stack frame that had already
	 * gone, and the filter then copied whatever the stack had taken to
	 * be a pod header, size included, into the 1k buffer above. */
	uint8_t pb[512];
	struct spa_pod_builder bb = SPA_POD_BUILDER_INIT(pb, sizeof(pb));
	struct spa_pod_frame f;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	/* Node-level EnumFormat.  A mix handle is never asked -- the core
	 * drives its format from the linked peer -- so answering
	 * -ENOTSUP was harmless there.  As a sink WirePlumber must
	 * manage, the node has to answer, and answer with what the
	 * adapter accepts: media.subtype = raw carrying rate, channels
	 * and a channel map.  Answering nothing made
	 * si_audio_adapter_configure() fail with "no usable format
	 * found", so no session item was ever created and the node
	 * could not be linked through the policy at all.
	 *
	 * This is the outward-facing geometry only.  The port-level
	 * format stays dsp F32P mono, which is what the mixer actually
	 * sums -- the same split module-filter-chain makes between its
	 * node-level raw declaration and its internal planar form. */
	/* Answer, do not refuse. The factory declares Format readable and
	 * writable, so WirePlumber enumerates it for its param cache, and a
	 * -ENOTSUP there makes the node's registration transition finish with
	 * an error: the node is never announced to the rest of the policy, so
	 * no session item, no link, and the mixer left with the dummy driver
	 * as its only neighbour. Whether that cache pass lands before the
	 * node is registered is a matter of timing, which is why it looked
	 * intermittent.
	 *
	 * Nothing is invented to answer with either. The geometry is the one
	 * the factory was handed, F32 at the channel count the module declared
	 * for this output, and Format and EnumFormat carry the same one because
	 * for a node driven by its peer what it can do and what it is doing are
	 * the same thing. The empty answer is only for a factory that was never
	 * told, which the module always does today; it is a guard, not a state
	 * the node is expected to sit in. */
	if (id != SPA_PARAM_EnumFormat && id != SPA_PARAM_Format &&
	    id != SPA_PARAM_Props)
		return -ENOTSUP;
	if ((id == SPA_PARAM_EnumFormat || id == SPA_PARAM_Format) &&
	    this->sink_channels == 0)
		return 0;
	/* Nothing has been negotiated yet, so there is no configured format
	 * to report, and inventing one is worse than saying nothing: this
	 * board runs 44100, and a Format of 48000 before the peer has spoken
	 * is a contradiction waiting to be reconciled. EnumFormat still
	 * answers, with the rate as a range, so 44100 is offered and the peer
	 * picks it. */
	if (id == SPA_PARAM_Format && this->sink_rate == 0)
		return 0;

	result.id = id;
	result.next = start;
next:
	result.index = result.next++;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	if (result.index > 0)
		return 0;

	if (id == SPA_PARAM_Props) {
		/* The mixer's own level. Reported in double because that is
		 * the type SPA_PROP_volume carries on the wire; it is kept
		 * as a float inside the node, which is the one place the
		 * width matters (see node_props). */
		param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
				SPA_PROP_volume,	SPA_POD_Double(this->props.volume),
				SPA_PROP_mute,	SPA_POD_Bool(this->props.mute != 0));
		goto emit;
	}

	{
		static const uint32_t map[] = {
			SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR,
			SPA_AUDIO_CHANNEL_FC, SPA_AUDIO_CHANNEL_LFE,
			SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR,
		};
		uint32_t pos[SPA_N_ELEMENTS(map)];
		uint32_t nch = this->sink_channels;
		uint32_t i;

		for (i = 0; i < nch && i < SPA_N_ELEMENTS(map); i++)
			pos[i] = map[i];

		/* A per-port gain is the same arithmetic at every rate, so the
		 * rate is enumerated as any and the peer's decides it. Pinning a
		 * number here makes the core reconcile two rates that were never
		 * in conflict, and on a 44100 device that reconciliation keeps
		 * tearing down and rebuilding our input ports. sink_rate stays
		 * as the rate actually adopted at set-param time. */
		spa_pod_builder_push_object(&bb, &f, SPA_TYPE_OBJECT_Format, id);
		spa_pod_builder_add(&bb,
			SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_audio),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
		if (id == SPA_PARAM_Format) {
			/* The configured format, so a single value per field:
			 * a choice range here is read as "no format yet", and
			 * a reader that parses it gets zero rate and unknown
			 * sample format, which is what pw-top was showing. */
			spa_pod_builder_add(&bb,
				SPA_FORMAT_AUDIO_format,   SPA_POD_Id(SPA_AUDIO_FORMAT_F32),
				SPA_FORMAT_AUDIO_rate,
					SPA_POD_Int(this->sink_rate),
				SPA_FORMAT_AUDIO_channels, SPA_POD_Int(nch),
				SPA_FORMAT_AUDIO_position,
					SPA_POD_Array(SPA_TYPE_Id, sizeof(uint32_t), nch, pos), 0);
		} else {
			spa_pod_builder_add(&bb,
				SPA_FORMAT_AUDIO_format,   SPA_POD_Id(SPA_AUDIO_FORMAT_F32),
				SPA_FORMAT_AUDIO_rate,
					SPA_POD_CHOICE_RANGE_Int(48000, 8000, 192000),
				SPA_FORMAT_AUDIO_channels, SPA_POD_Int(nch),
				SPA_FORMAT_AUDIO_position,
					SPA_POD_Array(SPA_TYPE_Id, sizeof(uint32_t), nch, pos), 0);
		}
		param = spa_pod_builder_pop(&bb, &f);
	}

emit:
	if (spa_pod_filter(&b, &result.param, param, filter) < 0)
		goto next;

	spa_node_emit_result(&this->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);

	if (++count != num)
		goto next;

	return 0;
}

void mixer_node_emit_info(struct impl *this, bool full);

static int mixer_node_set_param(void *object, uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	struct impl *this = object;

	/* The adapter sets the negotiated format on us.  Refusing it
	 * here is what stalled the async node/create-item hook: the
	 * session item never finished activating, and because hooks run
	 * as a chain, everything queued behind it -- including the
	 * default-node selection -- never ran either. */
	if (id == SPA_PARAM_Format && this->sink_channels > 0) {
		struct spa_audio_info_raw info;
		if (spa_format_audio_raw_parse(param, &info) < 0) {
			spa_log_error(this->log, "%p: sink got a format it cannot "
					"carry: not raw audio", this);
			return -EINVAL;
		}
		if (info.channels > this->sink_channels) {
			spa_log_error(this->log, "%p: sink asked for %u channels but "
					"declares %u", this, info.channels,
					this->sink_channels);
			return -EINVAL;
		}
		/* A Format with no rate fixed is a request to pick one, not a
		 * rate: storing it would put 0 into every frames-to-time
		 * conversion downstream. */
		if (info.rate != 0)
			this->sink_rate = info.rate;
		this->sink_channels = info.channels;
		spa_log_info(this->log, "%p: sink format set: %u Hz, %u ch",
				this, info.rate, info.channels);

		/* Announce that our Props changed.  This is not cosmetic.
		 * si-audio-adapter sets our Format and then starts a task
		 * that is completed only by a Props param-changed report on
		 * the proxy (or a ports-changed with ports present).  In dsp
		 * mode it does not even watch the port, and our ports are
		 * static, so Props is the only thing that can close it.
		 * Without this the async node/create-item hook never
		 * finishes, and because WirePlumber runs its hooks as one
		 * sequential chain, every event queued behind it -- our
		 * own included -- stops forever.  Pattern taken from
		 * bluez5 media-sink.c. */
		this->params[IDX_Props].flags ^= SPA_PARAM_INFO_SERIAL;
		this->info.change_mask |= SPA_NODE_CHANGE_MASK_PARAMS;
		mixer_node_emit_info(this, false);
		return 0;
	}

	if (id == SPA_PARAM_Props) {
		double volume = this->props.volume;
		bool mute = this->props.mute != 0;

		/* Seed with the current values: the OPT_ macros only
		 * overwrite what the caller actually sent, so a partial
		 * update leaves the rest alone. */
		if (spa_pod_parse_object(param, SPA_TYPE_OBJECT_Props, NULL,
				SPA_PROP_volume, SPA_POD_OPT_Double(&volume),
				SPA_PROP_mute,   SPA_POD_OPT_Bool(&mute)) < 0)
			return -EINVAL;

		if (volume < 0.0)
			volume = 0.0;

		/* A single aligned float store. The audio thread reads this
		 * every quantum without a lock, so the write has to be one
		 * access; see node_props. */
		this->props.volume = (float) volume;
		this->props.mute = mute ? 1 : 0;

		spa_log_info(this->log, "%p: level set: vol:%f mute:%d",
				this, this->props.volume, this->props.mute);

		/* Say the param changed by flipping its serial bit and
		 * announcing the parameter list, which is how the rest of
		 * this node reports a param going stale -- see the Format
		 * path above. Not CHANGE_MASK_PROPS: that announces node
		 * *properties*, and this node has none to hand out, so the
		 * core would read a NULL dict. */
		this->params[IDX_Props].flags ^= SPA_PARAM_INFO_SERIAL;
		this->info.change_mask |= SPA_NODE_CHANGE_MASK_PARAMS;
		mixer_node_emit_info(this, false);
		return 0;
	}

	return -ENOTSUP;
}

static int mixer_node_set_io(void *object, uint32_t id, void *data, size_t size)
{
	struct impl *this = object;

	switch (id) {
	case SPA_IO_Position:
		this->position = data;
		break;
	default:
		return -ENOTSUP;
	}
	return 0;
}

static int mixer_node_send_command(void *object, const struct spa_command *command)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		this->started = true;
		break;
	case SPA_NODE_COMMAND_Pause:
		this->started = false;
		break;
	default:
		return -ENOTSUP;
	}
	return 0;
}

void mixer_node_emit_info(struct impl *this, bool full)
{
	uint64_t old = full ? this->info.change_mask : 0;
	if (full)
		this->info.change_mask = this->info_all;
	if (this->info.change_mask) {
		spa_node_emit_info(&this->hooks, &this->info);
		this->info.change_mask = old;
	}
}

static int mixer_node_add_listener(void *object,
		struct spa_hook *listener,
		const struct spa_node_events *events,
		void *data)
{
	struct impl *this = object;
	struct spa_hook_list save;
	struct port *port;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	mixer_node_emit_info(this, true);
	/* Every output port must be announced; the output ports are not on
	 * port_list (that list carries the input ports), so they are
	 * walked separately.  Emitting only index 0 is what left the right
	 * channel with nothing to link to. */
	{
		uint32_t i;
		for (i = 0; i < this->n_out_ports; i++)
			mixer_port_emit_info(this, GET_OUT_PORT(this, i), true);
	}
	spa_list_for_each(port, &this->port_list, link)
		mixer_port_emit_info(this, port, true);

	spa_hook_list_join(&this->hooks, &save);

	return 0;
}

static int
mixer_node_set_callbacks(void *object,
			const struct spa_node_callbacks *callbacks,
			void *user_data)
{
	return 0;
}

/* The core tells a mix node who it is mixing for, and again with
 * SPA_ID_INVALID when that goes away. This is the only place the mixer learns
 * a peer identity, so it is where the per-port state is created and where the
 * module is told it exists.
 *
 * The entry is owned here -- the mixer allocates it and frees it when the port
 * goes -- but the module is handed a reference, which is what lets it write
 * the absolute bit at leisure without holding a pointer that might dangle.
 * The reference is the identity: a recycled global id cannot make the module
 * write into a different stream's port.
 */
static int mixer_node_port_set_mix_info(void *object,
		enum spa_direction direction, uint32_t port_id,
		uint32_t peer_id, const struct spa_dict *props)
{
	struct impl *this = object;
	struct port *port;

	(void) props;

	if (direction != SPA_DIRECTION_INPUT || port_id >= MAX_PORTS)
		return 0;
	port = this->in_ports[port_id];
	if (port == NULL)
		return -ENOENT;

	if (peer_id == SPA_ID_INVALID) {
		spa_log_debug(this->log, "mix info released: port %u", port_id);
		if (port->vol != NULL) {
			maemo_vol_port_cb cb = maemo_vol_port_cb_current();
			if (cb != NULL)
				cb(maemo_vol_port_cb_data(), port->vol->peer_id, NULL);
			maemo_port_data_unref(port->vol);
			port->vol = NULL;
		}
		return 0;
	}

	if (port->vol != NULL)
		return 0;			/* already associated */

	port->vol = maemo_port_data_new(peer_id);
	if (port->vol == NULL)
		return -ENOMEM;

	spa_log_debug(this->log, "mix info: port %u peer %u", port_id, peer_id);
	{
		maemo_vol_port_cb cb = maemo_vol_port_cb_current();
		if (cb != NULL)
			cb(maemo_vol_port_cb_data(), peer_id, port->vol);
	}
	return 0;
}

const struct spa_node_methods mixer_node_methods = {
	SPA_VERSION_NODE_METHODS,
	.add_listener = mixer_node_add_listener,
	.set_callbacks = mixer_node_set_callbacks,
	.port_set_mix_info = mixer_node_port_set_mix_info,
	.enum_params = mixer_node_enum_params,
	.set_param = mixer_node_set_param,
	.set_io = mixer_node_set_io,
	.send_command = mixer_node_send_command,
	.add_port = mixer_node_add_port,
	.remove_port = mixer_node_remove_port,
	.port_enum_params = mixer_port_enum_params,
	.port_set_param = mixer_port_set_param,
	.port_use_buffers = mixer_port_use_buffers,
	.port_set_io = mixer_port_set_io,
	.port_reuse_buffer = mixer_port_reuse_buffer,
	.process = mixer_node_process,
};
