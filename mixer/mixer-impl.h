#ifndef SPA_MAEMO_MIXER_IMPL_H
#define SPA_MAEMO_MIXER_IMPL_H

/* The mixer node, seen from the inside.
 *
 * This is the private header shared by the four files that make up the
 * plugin. Nothing here is reachable from outside the .so: the only symbol
 * the loader looks at is the handle factory at the bottom of
 * mixer-factory.c, and everything else is compiled hidden.
 *
 * NAMING. mixer_* is the node implementation -- what the core calls
 * through spa_node_methods, and the helpers that work divides into. If a
 * name starts with mixer_ it belongs to one of these four files and
 * nowhere else.
 *
 *   mixer-node.c      the node as the core sees it: parameters, io vectors,
 *                     commands, listeners, and the method table itself.
 *   mixer-port.c      one input or output port: adding, removing, formats,
 *                     buffers and properties.
 *   mixer-process.c   the audio thread: what runs every quantum.
 *   mixer-factory.c   creating and destroying the instance.
 *
 * WHAT THIS IS. A mixer with a volume. It sums its inputs and applies one
 * level to the result. That is the whole contract, and it is deliberately
 * small: there is no per-input gain, no duck, and no notion of any
 * hardware downstream. Anything that wants those has to be built on top
 * of this rather than woven through it.
 */

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>

#include <spa/support/plugin.h>
#include <spa/support/log.h>
#include <spa/support/cpu.h>
#include <spa/support/loop.h>
#include <spa/utils/list.h>
#include <spa/utils/names.h>
#include <spa/utils/string.h>
#include <spa/node/node.h>
#include <spa/node/utils.h>
#include <spa/node/io.h>
#include <spa/utils/ratelimit.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/param.h>
#include <spa/param/props.h>
#include <spa/pod/filter.h>
#include <spa/pod/parser.h>

#include "mix-volume.h"
#include "maemo-swstep.h"
#include "maemo-mixer-ext.h"
#include "maemo-vol-state.h"

#define MAX_BUFFERS	64
#define MAX_DATAS	SPA_AUDIO_MAX_CHANNELS

#define MAX_PORTS	512
/* Output ports are one per channel, so this bounds the sink layout. */
#define MAX_CH		4
/* The widest alignment any kernel here asks for: the AVX2 volume kernel
 * uses 32-byte loads and NEON 16. Buffers are aligned to the smaller of
 * this and what the CPU actually offers, and each kernel re-checks before
 * taking an aligned path, so this is a preference rather than a promise. */
#define MAX_ALIGN	32u

#define NODE_DEFAULT_VOLUME	1.0f
#define NODE_DEFAULT_MUTE	false

/* The level of the whole mixer.
 *
 * The audio thread reads this every quantum while a writer on another
 * loop sets it, so the gain has to land as a single aligned 4-byte store.
 * A double is two 32-bit stores on armhf: the RT thread can catch the
 * halves apart, which is not a slightly wrong level but a torn exponent
 * -- garbage. The VM cannot show this; the device will. Keep it float.
 *
 * Mute stays a separate field, so volume and mute do not change together.
 * The worst case across a mute transition is one quantum at the old gain,
 * which is benign next to a torn read. */
struct node_props {
	float volume;
	int32_t mute;
};

/* Enforce the layout the lock-free read depends on.  If either field
 * grows, the RT thread's unsynchronised read stops being a single
 * access. */
SPA_STATIC_ASSERT(sizeof(struct node_props) == 2 * sizeof(float),
		"node_props must stay two 4-byte words: the audio thread reads volume with no lock");

struct buffer {
	uint32_t id;
#define BUFFER_FLAG_QUEUED	(1 << 0)
#define BUFFER_FLAG_MAPPED	(1 << 1)
	uint32_t flags;

	struct spa_list link;
	struct spa_buffer *buffer;
	struct spa_meta_header *h;
	struct spa_buffer buf;

	void *datas[MAX_DATAS];
};

struct port {
	struct spa_list link;

	uint32_t direction;
	uint32_t id;

	struct spa_io_buffers *io[2];

	uint64_t info_all;
	struct spa_port_info info;
	struct spa_param_info params[8];

	/* Persistent storage for the properties published on a declared
	 * sink input port.  The core copies them into the pw_impl_port it
	 * materialises from spa_port_info.props, but the dict and the
	 * strings it points at have to stay valid across the call, so
	 * they live here rather than on the stack. */
	char chan_name[8];
	char port_name[32];
	struct spa_dict_item prop_items[4];
	struct spa_dict props_dict;

	unsigned int have_format:1;

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;

	struct spa_list queue;

	struct spa_list mix_link;
	bool active:1;

	/* Refcounted per-port state the module writes into. Owned here
	 * (the mixer allocates and frees it) but referenced by the module,
	 * which is what lets it write without holding a pointer that might
	 * dangle. */
	struct maemo_port_data *vol;
	/* The gain last reported for this input, or -1 when nothing has
	 * been reported yet. Gains are never negative, so it doubles as the
	 * "never" marker. Diagnostics only. */
	float diag_gain;
};

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	struct spa_log *log;
	struct spa_cpu *cpu;
	uint32_t cpu_flags;
	uint32_t max_align;

	struct spa_loop *data_loop;

	/* The mixer's own level. Written through node SPA_PARAM_Props,
	 * read once per cycle by the audio thread. See node_props. */
	struct node_props props;

	uint32_t quantum_limit;

	struct mix_volume_ops vol_ops;

	uint64_t info_all;
	struct spa_node_info info;
	struct spa_param_info params[8];

	struct spa_io_position *position;

	struct spa_hook_list hooks;

	struct port *in_ports[MAX_PORTS];
	/* One output port per channel in the standalone sink role.  The
	 * mix-handle role keeps a single unchannelled output, which is
	 * correct there because the core made us the mixer for one port
	 * and wires our output into it directly. */
	struct port out_ports[MAX_CH];
	uint32_t n_out_ports;

	/* Measurement only; inert unless MAEMO_PROBE=1. */
	struct maemo_swstep swstep;

	/* The module's published word: duck, abs_active, generation. NULL
	 * until the module attaches, which is the correct degraded state:
	 * no absolute streams, everything at unity. */
	/* How many links hang off our output ports. Zero means the graph
	 * ends here, which is what it looks like while the policy is still
	 * building the relay, and a codec deadline taken from a driver that
	 * leads nowhere is a number that means nothing. */
	unsigned n_downstream;

	struct maemo_vol_state *vstate;
	uint64_t vstate_last;

	/* node.always-process, published on this node while a codec change is
	 * owed. Only the sink instance has a graph to hold up. */
	bool abs_armed;		/* abs_active the last codec change was armed on */
	struct spa_dict_item node_items[1];
	struct spa_dict node_props;
	uint32_t gain_apps;	/* measurement: how often this instance applies a gain */
	struct spa_list port_list;
	struct spa_list free_list;

	/* Declared input ports for the standalone sink role.  Zero means
	 * the mix-handle role, where the core adds one input per link. */
	uint32_t sink_channels;
	uint32_t sink_rate;

	struct buffer *mix_buffers[MAX_PORTS];
	const void *mix_datas[MAX_PORTS];
	float mix_vols[MAX_PORTS];

	int n_formats;
	struct spa_audio_info format;
	uint32_t stride;

	unsigned int have_format:1;
	unsigned int started:1;

	struct spa_list mix_list;

	struct spa_ratelimit rate_limit;
};

#define CHECK_ANY_IN(this,d,p)       ((d) == SPA_DIRECTION_INPUT && (p) == SPA_ID_INVALID)
#define CHECK_FREE_IN_PORT(this,d,p) ((d) == SPA_DIRECTION_INPUT && (p) < MAX_PORTS && this->in_ports[(p)] == NULL)
#define CHECK_IN_PORT(this,d,p)      ((d) == SPA_DIRECTION_INPUT && (p) < MAX_PORTS && this->in_ports[(p)] != NULL)
#define CHECK_OUT_PORT(this,d,p)     ((d) == SPA_DIRECTION_OUTPUT && (p) < (this)->n_out_ports)
#define CHECK_PORT(this,d,p)         (CHECK_OUT_PORT(this,d,p) || CHECK_IN_PORT (this,d,p))
#define CHECK_PORT_ANY(this,d,p)     (CHECK_ANY_IN(this,d,p) || CHECK_PORT(this,d,p))
#define GET_IN_PORT(this,p)          (this->in_ports[p])
#define GET_OUT_PORT(this,p)         (&this->out_ports[p])
#define GET_PORT(this,d,p)           (d == SPA_DIRECTION_INPUT ? GET_IN_PORT(this,p) : GET_OUT_PORT(this,p))
#define GET_PORT_ANY(this,d,p)       (CHECK_ANY_IN(this,d,p) ? NULL : GET_PORT(this,d,p))



/* Node-level param slots.  Named because the serial flag has to be
 * toggled on the right one, and that is invisible otherwise. */
#define IDX_EnumFormat	0
#define IDX_Format	1
#define IDX_Props	2

/* The node method table, defined in mixer-node.c and attached to the
 * handle by the factory. */
extern const struct spa_node_methods mixer_node_methods;

/* ---- the pieces, and which file each one lives in ----------------------
 *
 * Only what crosses a file boundary is declared here; everything else
 * stays static in the file that owns it. The method table in
 * mixer-node.c is what forces most of these to be visible: it names
 * functions defined in mixer-port.c and mixer-process.c.
 */

/* mixer-node.c */
void mixer_node_emit_info(struct impl *this, bool full);

/* mixer-port.c */
int mixer_node_add_port(void *object, enum spa_direction direction, uint32_t port_id, const struct spa_dict *props);
int mixer_node_remove_port(void *object, enum spa_direction direction, uint32_t port_id);
struct port *mixer_port_alloc(struct impl *this);
void mixer_port_emit_info(struct impl *this, struct port *port, bool full);
int mixer_port_enum_formats(void *object, struct port *port, uint32_t index, struct spa_pod **param, struct spa_pod_builder *builder);
int mixer_port_enum_params(void *object, int seq, enum spa_direction direction, uint32_t port_id, uint32_t id, uint32_t start, uint32_t num, const struct spa_pod *filter);
int mixer_port_reuse_buffer(void *object, uint32_t port_id, uint32_t buffer_id);
int mixer_port_set_format(void *object, enum spa_direction direction, uint32_t port_id, uint32_t flags, const struct spa_pod *format);
int mixer_port_set_io(void *object, enum spa_direction direction, uint32_t port_id, uint32_t id, void *data, size_t size);
int mixer_port_set_param(void *object, enum spa_direction direction, uint32_t port_id, uint32_t id, uint32_t flags, const struct spa_pod *param);
int mixer_port_use_buffers(void *object, enum spa_direction direction, uint32_t port_id, uint32_t flags, struct spa_buffer **buffers, uint32_t n_buffers);

/* mixer-process.c */
int mixer_clear_buffers(struct impl *this, struct port *port);
int mixer_node_process(void *object);
int mixer_queue_buffer(struct impl *this, struct port *port, struct buffer *b);
#endif /* SPA_MAEMO_MIXER_IMPL_H */
