/* Creating and destroying the instance.
 *
 * The handle factory is the only thing the SPA loader looks at. It reads
 * the settings handed to it, decides which of the two roles the mixer is
 * being asked for -- a standalone sink with declared channels, or a mix
 * handle the core made for one link -- and builds the node around that.
 */

#include <spa/node/keys.h>
#include <spa/utils/dict.h>
#include "mixer-impl.h"

#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic
SPA_LOG_TOPIC_DEFINE_STATIC(log_topic, "spa.maemo.mixer");

static int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;


	if (spa_streq(type, SPA_TYPE_INTERFACE_Node))
		*interface = &this->node;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	struct impl *this;
	struct port *port;

	spa_return_val_if_fail(handle != NULL, -EINVAL);

	this = (struct impl *) handle;

	spa_list_insert_list(&this->free_list, &this->port_list);
	spa_list_consume(port, &this->free_list, link) {
		spa_list_remove(&port->link);
		free(port);
	}
	return 0;
}

static size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	return sizeof(struct impl);
}

static int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	struct impl *this;
	struct port *port;
	uint32_t i;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	this->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	spa_log_topic_init(this->log, &log_topic);

	/* The software gain starts at unity. The module used to state this
	 * through the node's volume param, but that param is what
	 * pipewire-pulse exports as the sink volume and is no longer ours to
	 * write, and a zeroed struct here is silence rather than a default. */
	this->props.volume = 1.0f;

	this->data_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataLoop);
	if (this->data_loop == NULL) {
		spa_log_error(this->log, "a data loop is needed");
		return -EINVAL;
	}
	this->cpu = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_CPU);
	if (this->cpu) {
		this->cpu_flags = spa_cpu_get_flags(this->cpu);
		this->max_align = SPA_MIN(MAX_ALIGN, spa_cpu_get_max_align(this->cpu));
	}
	mix_volume_init(&this->vol_ops, this->cpu_flags);

	/* The module's published word, if it has attached. Absent is
	 * a valid state, not a failure, it means no absolute volume policy is
	 * in play and every input stays at unity.
	 *
	 * This is the whole of the mixer's connection to the volume policy, so
	 * it is not instrumentation and nothing here may be dropped with the
	 * measurement harness, without it the mixer scales nothing by the
	 * user's number and the slider appears dead. */
	{
		const char *vkey = info ? spa_dict_lookup(info, "maemo.vstate") : NULL;
		this->vstate = maemo_vol_state_lookup(vkey);
	}
	if (this->vstate != NULL)
		this->vstate_last = maemo_vol_state_load(this->vstate);
	else
		spa_log_info(this->log, "no volume state attached: "
				"absolute volume inactive");

	/* The software step, taken from the graph position rather than a timer.
	 * Armed from the environment because it exists to answer one question,
	 * what a step sounds like when the main loop cannot jitter, and it is
	 * only ever armed on the sink the module created, see the marker test
	 * below. */
	{
		const char *at = getenv("MAEMO_SWSTEP_AT");
		const char *to = getenv("MAEMO_SWSTEP_TO");
		const char *hw = getenv("MAEMO_HWSTEP_TO");
		const char *csv = getenv("MAEMO_SWSTEP_CSV");

		/* Only the sink the module created takes the step. The core
		 * instantiates this factory once per link for the mix-handle
		 * role, and those instances get no creation dict entries of
		 * ours, so the key the module passes is what tells them apart.
		 * Without this test every handle would run its own copy of the
		 * step and scale the same signal again, making the level depend
		 * on how many links are open. */
		const char *main_marker = info ?
			spa_dict_lookup(info, "maemo.sink.channels") : NULL;

		if (at != NULL && *at != '\0' && to != NULL && *to != '\0' &&
				main_marker != NULL) {
			if (maemo_swstep_open(&this->swstep, csv,
						strtoull(at, NULL, 10),
						(float) atof(to),
						hw ? (float) atof(hw) : -1.0f) == 0)
				spa_log_info(this->log,
						"software gain step armed: -> %s at position %s -> %s",
						to, at, csv ? csv : "(no log)");
		}
	}

	/* Stated once at startup. The CPU this builds on and the CPU a
	 * phone has are not the same machine, and a kernel chosen wrongly
	 * shows up as audio trouble rather than as a missing instruction,
	 * so the log should say what was picked and why. */
	spa_log_info(this->log,
			"volume kernel: %s (cpu flags 0x%08x%s)",
			mix_volume_kernel_name(&this->vol_ops), this->cpu_flags,
			this->cpu ? "" : ", no CPU interface: assumed baseline");

	for (i = 0; info && i < info->n_items; i++) {
		const char *k = info->items[i].key;
		const char *s = info->items[i].value;
		if (spa_streq(k, "clock.quantum-limit"))
			spa_atou32(s, &this->quantum_limit, 0);
		else if (spa_streq(k, "maemo.sink.channels"))
			spa_atou32(s, &this->sink_channels, 0);
		else if (spa_streq(k, "maemo.sink.rate"))
			spa_atou32(s, &this->sink_rate, 0);
	}

	/* A sink that cannot state a rate is a sink nothing can open. */
	if (this->sink_rate == 0)
		this->sink_rate = 48000;

	spa_hook_list_init(&this->hooks);
	spa_list_init(&this->port_list);
	spa_list_init(&this->free_list);
	spa_list_init(&this->mix_list);

	this->props.volume = NODE_DEFAULT_VOLUME;
	this->props.mute = NODE_DEFAULT_MUTE;

	this->rate_limit.interval = 2 * SPA_NSEC_PER_SEC;
	this->rate_limit.burst = 1;

	this->node.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE,
			&mixer_node_methods, this);
	this->info = SPA_NODE_INFO_INIT();

	/* The sink node alone can hold the graph up, and it only does so while
	 * a codec change is owed. Declared false here so the property exists
	 * from the start and a later change is a change of value rather than
	 * the appearance of a key nobody saw before.
	 *
	 * The core merges this dict into the node's properties on a props
	 * change and, since "core: walk the graph when a node's
	 * always-process changes", raising it triggers the graph walk that
	 * makes it take effect. Without that walk the property would be set
	 * and silently ignored until something else happened to recalc. */
	if (this->sink_channels > 0) {
		this->node_items[0] = SPA_DICT_ITEM_INIT(SPA_KEY_NODE_ALWAYS_PROCESS,
				"false");
		this->node_props = SPA_DICT_INIT(this->node_items, 1);
		this->info.props = &this->node_props;
		this->info.change_mask |= SPA_NODE_CHANGE_MASK_PROPS;
	}

	this->info.max_input_ports = MAX_PORTS;
	this->info.max_output_ports = MAX_CH;
	this->info.change_mask |= SPA_NODE_CHANGE_MASK_FLAGS;
	this->info.flags = SPA_NODE_FLAG_RT | SPA_NODE_FLAG_IN_DYNAMIC_PORTS;

	/* The node must advertise EnumFormat for anything to be able to
	 * ask it.  Without SPA_NODE_CHANGE_MASK_PARAMS the core answers
	 * -ENOENT before the implementation is ever called, so a node
	 * that implements the callback and a node that does not are
	 * indistinguishable from outside. */
	if (this->sink_channels > 0) {
		this->info.change_mask |= SPA_NODE_CHANGE_MASK_PARAMS;
		this->params[IDX_EnumFormat] =
			SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
		/* EnumFormat alone is not enough: the adapter must also be
		 * able to SET the chosen format, so it has to be
		 * advertised as writable or the set is rejected before it
		 * reaches us. */
		this->params[IDX_Format] =
			SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_READWRITE);
		/* The mixer's software gain. Readable so a reader can see
		 * what is being applied; deliberately NOT writable from
		 * outside.
		 *
		 * WirePlumber sets a volume on every sink it manages. If
		 * this param were advertised read-write, its policy and the
		 * device's own hardware level would both be driving
		 * loudness, and the two would multiply -- a slider at full
		 * with the mixer quietly at 0.37. The level the user sets
		 * belongs to the hardware; this gain is internal, and the
		 * module that owns it writes it in-process, which does not
		 * go through the advertisement. */
		this->params[IDX_Props] =
			SPA_PARAM_INFO(SPA_PARAM_Props, SPA_PARAM_INFO_READ);
		this->info.params = this->params;
		this->info.n_params = 3;
	}

	this->info_all = this->info.change_mask;

	/* Output ports, one per channel.
	 *
	 * A single unchannelled output can only ever be linked to one
	 * destination channel, which leaves the other side of the world
	 * unfed.  Declaring a port per channel with audio.channel set is
	 * what lets the linker match our left onto the sink's left and
	 * our right onto its right. */
	{
		static const char *const out_ch[] = { "FL", "FR", "FC", "LFE" };
		uint32_t n_out = (this->sink_channels > 0)
			? SPA_MIN(this->sink_channels, (uint32_t) MAX_CH)
			: 1u;
		uint32_t i;

		for (i = 0; i < n_out; i++) {
			port = GET_OUT_PORT(this, i);
			port->direction = SPA_DIRECTION_OUTPUT;
			port->id = i;

			if (this->sink_channels > 0) {
				spa_scnprintf(port->chan_name, sizeof(port->chan_name),
					    "%s", out_ch[i]);
				spa_scnprintf(port->port_name, sizeof(port->port_name),
					    "output_%s", out_ch[i]);

				port->prop_items[0] = SPA_DICT_ITEM_INIT("audio.channel",
							    port->chan_name);
				port->prop_items[1] = SPA_DICT_ITEM_INIT("port.name",
							    port->port_name);
				port->prop_items[2] = SPA_DICT_ITEM_INIT("port.physical", "true");
				port->prop_items[3] = SPA_DICT_ITEM_INIT("port.terminal", "true");
				port->props_dict = SPA_DICT_INIT(port->prop_items, 4);
			}

			port->info = SPA_PORT_INFO_INIT();
			port->info.change_mask |= SPA_PORT_CHANGE_MASK_FLAGS;
			port->info.flags = SPA_PORT_FLAG_DYNAMIC_DATA;
			port->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
			if (this->sink_channels > 0)
				port->info.change_mask |= SPA_PORT_CHANGE_MASK_PROPS;
			port->info_all = port->info.change_mask;
			port->params[0] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
			port->params[1] = SPA_PARAM_INFO(SPA_PARAM_Meta, SPA_PARAM_INFO_READ);
			port->params[2] = SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
			port->params[3] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
			port->params[4] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
			port->info.params = port->params;
			port->info.n_params = 5;
			if (this->sink_channels > 0) {
				port->info.props = &port->props_dict;
				port->info_all |= SPA_PORT_CHANGE_MASK_PROPS;
			}

			spa_list_init(&port->queue);
			this->n_out_ports = i + 1;
		}
	}

	/* Standalone sink role: declare the input ports so streams have
	 * something to link to.  The core materialises a pw_impl_port
	 * from each emitted port info and reads the channel layout out of
	 * spa_port_info.props -- without those the port carries no
	 * audio.channel and nothing can be matched onto it.
	 *
	 * Emission is left to add_listener, which walks in_ports the same
	 * way it walks the output port. */
	if (this->sink_channels > 0) {
		static const char *const ch_names[] = { "FL", "FR", "FC", "LFE" };
		uint32_t n = SPA_MIN(this->sink_channels, 4u);
		uint32_t i;

		for (i = 0; i < n; i++) {
			struct port *p = mixer_port_alloc(this);
			if (p == NULL)
				return -errno;

			spa_scnprintf(p->chan_name, sizeof(p->chan_name),
				    "%s", ch_names[i]);
			spa_scnprintf(p->port_name, sizeof(p->port_name),
				    "playback_%s", ch_names[i]);

			p->prop_items[0] = SPA_DICT_ITEM_INIT("audio.channel",
							    p->chan_name);
			p->prop_items[1] = SPA_DICT_ITEM_INIT("port.name",
							    p->port_name);
			p->prop_items[2] = SPA_DICT_ITEM_INIT("port.physical", "true");
			p->prop_items[3] = SPA_DICT_ITEM_INIT("port.terminal", "true");
			p->props_dict = SPA_DICT_INIT(p->prop_items, 4);

			p->direction = SPA_DIRECTION_INPUT;
			p->id = i;
			spa_list_init(&p->queue);
			p->info_all = SPA_PORT_CHANGE_MASK_FLAGS |
				    SPA_PORT_CHANGE_MASK_PARAMS |
				    SPA_PORT_CHANGE_MASK_PROPS;
			p->info = SPA_PORT_INFO_INIT();
			p->info.flags = SPA_PORT_FLAG_DYNAMIC_DATA;
			p->info.props = &p->props_dict;
			p->params[0] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
			p->params[1] = SPA_PARAM_INFO(SPA_PARAM_Meta, SPA_PARAM_INFO_READ);
			p->params[2] = SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
			p->params[3] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
			p->params[4] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
			p->info.params = p->params;
			p->info.n_params = 5;

			this->in_ports[i] = p;
			spa_list_append(&this->port_list, &p->link);
		}
	}

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_Node,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info,
			 uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*info = &impl_interfaces[*index];
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}

/* The SPA loader looks these two up by name out of the .so, so they are
 * the other thing that has to survive -fvisibility=hidden. */
SPA_EXPORT const struct spa_handle_factory spa_maemo_flat_volume_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_AUDIO_MIXER_DSP,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};

/* The same mixer under its own name, so it can be loaded explicitly --
 * and created as a node by name -- rather than only through the spa-libs
 * redirect that makes it the core's per-port mix handle. */
SPA_EXPORT const struct spa_handle_factory spa_maemo_flat_volume_named_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	"audio.maemo.mixer",
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
