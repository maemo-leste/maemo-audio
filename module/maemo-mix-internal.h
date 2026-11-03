/* maemo mixer module, shared internals.
 *
 * struct impl and the keys it speaks, shared by the files the module is
 * split across. Nothing here is public: the module is one .so and these
 * symbols never leave it.
 *
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <math.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <pipewire/pipewire.h>
#include <pipewire/global.h>
#include <pipewire/impl.h>
#include <pipewire/module.h>
#include <pipewire/data-loop.h>
#include <pipewire/loop.h>
#include <pipewire/thread.h>
#include <pipewire/impl-factory.h>
#include <pipewire/extensions/metadata.h>
#include <spa/support/loop.h>
#include <spa/support/thread.h>
#include <spa/node/node.h>
#include <spa/utils/string.h>
#include <spa/param/props.h>
#include <spa/param/route.h>
#include <spa/param/param.h>
#include <spa/utils/result.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>
#include <spa/pod/vararg.h>
#include <sys/timerfd.h>
#include <alsa/asoundlib.h>
#include "maemo-mixer-ext.h"
#include "maemo-vol-state.h"
#include "maemo-vol-map.h"
#include "maemo-ladder-cache.h"

/* Exported by the mixer library, which this module links against. The
 * mixer calls back into the function registered here; handing the pointer
 * over is the only way round, because the link direction is module ->
 * mixer and a module symbol cannot be referenced from the mixer without a
 * circular dependency. */
extern void maemo_swstep_arm(struct spa_loop *loop, spa_invoke_func_t cb,
		void *user_data);


/* The level bookkeeping is written on the control loop and read on the
 * codec loop. Each is a single aligned word, so a read cannot tear; what
 * the atomics buy is that neither side can be left holding a compiler
 * register across the thread boundary and that the ordering is defined
 * rather than incidental. A stale value costs one redundant codec write
 * and never a wrong level, because the queue holds the amplitude that was
 * resolved when the request was made. */
static inline float vol_loadf(const float *p)
{ float v; __atomic_load(p, &v, __ATOMIC_RELAXED); return v; }
static inline void vol_storef(float *p, float v)
{ __atomic_store(p, &v, __ATOMIC_RELAXED); }
static inline bool vol_loadb(const bool *p)
{ bool v; __atomic_load(p, &v, __ATOMIC_RELAXED); return v; }
static inline void vol_storeb(bool *p, bool v)
{ __atomic_store(p, &v, __ATOMIC_RELAXED); }

/* The target the applet writes, on the metadata object it binds. */
#define APP_TARGET_KEY		"x-maemo.volume.target"

/* The metadata object the applet binds and writes its target to. The
 * daemon has several; this is the one that matters. */
#define APP_METADATA_NAME	"default"

/* The ladder, published on a node by the monitor rules for that device.
 * The same property the applet reads to draw its slider, so the curve
 * lives in exactly one place and cannot drift from the thing it describes. */
#define TUNING_KEY		"x-maemo.volume.tuning"
#define TUNING_INCALL_KEY	"x-maemo.volume.tuning.incall"

/* An optional pair announced with the ladder, on the same node and with the
 * same lifetime: a control for absolute playback to move instead of the one
 * the route declares, and the rung to put on it, in centibels.
 *
 * It exists for a cascaded output. During a call the route declares the post
 * mix control, which carries the modem path, while a local stream reaches the
 * same output through the pre mix control: moving the post mix for a click
 * would move the call with it. */
#define ABS_ELEMENT_KEY		"x-maemo.volume.abs-element"
#define ABS_VALUE_KEY		"x-maemo.volume.abs-value"

/* The metadata namespace this module owns.
 *
 * The routing policy binds it and refuses to do anything until it is
 * there: refresh() returns early when the namespace is missing, so an
 * absent namespace means no relay link, no default sink and no route
 * announcement -- a silent stack that looks healthy. Creating it is
 * therefore not a formality, and the module has to own it rather than
 * wait for someone else to. */
#define ROUTE_NS		"x-maemo-mix"
#define ROUTE_KEY		"x-maemo.route"

/* Where the generic fallback ladder comes from.
 *
 * Module args, not pw_context_get_conf_section(). That API reads the
 * merged config but only exposes the MAIN file's top-level sections
 * unprefixed: conf.c add_override() stores every conf.d fragment under
 * "override.<level>.<index>.<key>", so a key placed in a fragment is
 * reachable only by guessing a scandir index. Module args come through
 * module_init intact from our own fragment. */
#define DEFAULT_TUNING_KEY	"tuning.default"
#define DEFAULT_TUNING_INCALL_KEY "tuning.default.incall"

/* Not in the installed impl-node.h, but exported by libpipewire. It is
 * what pw's own code uses to set a param on a node it owns in-process,
 * which is exactly the situation here. */
int pw_impl_node_set_param(struct pw_impl_node *node, uint32_t id,
		uint32_t flags, const struct spa_pod *param);

struct pw_impl_device;
int pw_impl_device_set_param(struct pw_impl_device *device, uint32_t id,
		uint32_t flags, const struct spa_pod *param);

#define HWQ 16

struct codec {
	/* One card's hardware path, the queue of level changes the mixer asked
	 * for, the thread that delivers them and the control it writes. The
	 * queue and every decision about it are control loop; the fields below
	 * the deadline are touched only on the codec thread, and the hand back
	 * after a write is what keeps that true.
 */
	/* Everything below belongs to one output: its codec queue and control,
	 * its ladder, the device and route it is bound to, and the mixer node
	 * that carries it.
 */

	/* The hardware write the mixer asked for, and the timer that
	 * delivers it. The deadline is absolute and was computed on the
	 * data loop; nothing here re-derives it from "now", because the
	 * control loop may be busy with anything at the moment the mixer's
	 * hand-off lands. */
	struct spa_source *hw_timer;

	struct {
		uint64_t deadline;
		float value;
		bool raise;
		/* A raw control step, when the caller already knows one, which
		 * is what a substituted control always does: its level comes
		 * from configuration or from the hardware, never from the
		 * ladder, so it must not be snapped onto a rung. `value` still
		 * carries the ordering scalar for these entries, the step
		 * itself, because the margin walk below only ever asks which
		 * of two levels is louder. -1 means "no step, use the
		 * ladder". */
		long step;
	} hwq[HWQ];

	unsigned int hwq_head, hwq_len;

	/* ---- realtime control write (proof of concept) ----
	 *
	 * The queue and every decision about it stay on the control loop.
	 * What moves to the mixer's data loop is the delivery: a timer armed
	 * on the data loop fires on the realtime thread, and the control
	 * write -- measured at a few microseconds on this codec -- happens
	 * there instead of being marshalled through the route parameter into
	 * whichever process owns the card. That removes both unbounded hops
	 * the timing used to depend on.
	 *
	 * The element is named in the module arguments for now. It belongs
	 * to the ALSA device, which is the only party that knows which
	 * element carries a route's volume, so the real version of this is
	 * the device announcing it, not the operator configuring it.
	 *
	 * rt_* fields below the deadline and value are touched only from the
	 * data loop; everything else stays control-loop-only, and the hand
	 * back after a write is what keeps that true. */
	struct {
		snd_mixer_t *mixer;
		snd_mixer_elem_t *elem;
		long lo, hi;
		long db_lo, db_hi;	/* element's own scale, 0.01 dB */
		bool has_db;
		bool ok;
	} ctl;

	/* What rt_ctl currently holds open, so a route announcement can tell
	 * "same control" from "different control" instead of reopening on
	 * every announcement. */
	char card[64];

	char elem[128];

	struct spa_loop *loop;		/* our own codec loop, see rt_thread_fn */
	struct pw_loop *pwloop;		/* its pw_loop, so we can lock and destroy it */
	struct spa_thread *thr;		/* the thread running it */
	struct spa_thread_utils *tu;	/* who made it: module-rt's, so RTKit applies */
	bool running;

	struct spa_source timer;

	bool timer_added;

	bool pending;

	bool raise;

	uint64_t deadline;

	long step;

	float value;

	/* The level we last wrote, or -1.0f when we have written nothing to
	 * the device currently in hand. Negative is not an amplitude, so it
	 * cannot be confused with one, and the no-change comparison can never
	 * match it -- which is the point: "the control already holds this"
	 * is not a question this field can answer until a write has happened.
	 * A route change invalidates it for exactly that reason; we have
	 * written nothing to the device that just became the output. */
	float written;

	float amp;			/* current rung amplitude: the codec
					 * target when idle and the duck when
					 * an absolute stream plays -- one
					 * variable, deliberately */
};

struct boost {
	/* The temporary hardware level for an absolute stream, raise the
	 * control, duck the bed by exactly what the raise added, and put back
	 * what was there when it opened.
 */
	/* The top rung of the ladder, which is what "MAX" means here. It is
	 * deliberately not the hardware ceiling: a ladder stops below the
	 * codec's maximum on purpose -- droid4's control reaches +12 dB and
	 * its ladder tops out below that -- so a notification is as loud as
	 * the loudest the slider can ask for and not louder. Writing a
	 * literal 1.0 would hand the difference to the codec as extra gain,
	 * which on that hardware is nine decibels of clipping on a signal
	 * that was already full scale. */
	float max;

	/* The per-port override, see ABS_ELEMENT_KEY. `abs_elem` is what the
	 * port asked for, which is not necessarily what is in force: when the
	 * name matches the announced control, or cannot be opened, the
	 * override is off, and `abs_sub` is what the rest of the module asks
	 * rather than `abs_elem` being non-empty. */
	char elem[128];


	/* The control's own level when the current absolute window opened,
	 * or -1.0f when no window is open or nothing could be read. The
	 * release restores this rather than recomputing, so a level somebody
	 * else put on the codec survives the window instead of being
	 * overwritten by our idea of what the slider should be. */
	float restore;

	/* Set when the slider moved while the window was open. The readback
	 * is then stale -- the user has since decided the level -- and the
	 * release must follow the slider instead. */
	bool slider_during;

	/* The same idea for a substituted control, and it has to be a
	 * hardware read because the module never writes that control
	 * otherwise: UCM put whatever it put there at profile enable, and
	 * the only honest release is to put it back. A raw step, -1 when no
	 * window is open or the read failed. */
	long restore_step;

	/* The duck for a substituted window, resolved when the control is
	 * bound, or -1 when there is none.
	 *
	 * The substituted raise moves a stage every local stream passes
	 * through, so the others have to give back exactly what it adds:
	 * duck x P_abs == P_ucm, which keeps the amplitude arriving at the
	 * hardware mixer what it was before the window. Nothing downstream
	 * enters, the post stage is not moved and multiplies everything
	 * equally, and the slider lives there, so the duck does not follow
	 * the slider either: it is a constant for the window. */
	float duck;

	bool active;

	long step;		/* raw step for the configured rung, -1 if none */
	bool sub;		/* absolute playback moves another control */
};

struct route_bind {
	/* The device and route this output drives, as announced by the policy.
	 * A route change invalidates what we believe the hardware level is.
 */
	/* The hardware the slider drives, as announced by the routing
	 * policy: which device, which route device on it, which route index
	 * on that. All three are needed -- a card numbers its routes across
	 * both directions, so an index alone names the wrong thing. */
	struct pw_impl_device *dev;

	struct spa_hook dev_hook;

	uint32_t dev_idx;

	uint32_t index;

	uint32_t channels;

	/* The level last written to the route, or -1 when the route changed
	 * and nothing is known: see write_route_volume(). */
	float written;

	uint32_t announced_id;

	/* Set once an announcement has been answered, so the first one is not
	 * mistaken for a change of path. */
	bool announced_set;

	char last[256];	/* the last announcement we answered with */
	struct spa_hook dev_info_hook;	/* watching the announced device's params */
};

struct sink_ref {
	/* Our mixer node and the output sink it is linked to, remembered by
	 * name because both are recreated under us.
 */
	/* Our own sink. Created once, lives as long as the module. */
	struct pw_impl_node *mixer;

	uint32_t bound_id;

	/* The output sink the applet is reading, remembered by node.name
	 * rather than by pointer. A device sink is recreated whenever the
	 * profile changes, the port switches or the ALSA device is
	 * re-enumerated, and each recreation has its own global id -- so an
	 * id or a pointer goes stale exactly when the value matters, and a
	 * stale pointer is worse than no pointer. The name survives the
	 * recreation; the lookup is redone every time it is needed. */
	char name[128];

	/* Our own mixer's node.name, so the fallback publisher can tell it
	 * apart from a real output. */
	char mixer_name[128];

	struct spa_hook hook;	/* the sink's declared distance */
	struct pw_impl_node *node;	/* the node that hook is linked into */
	bool hold;

	/* The output sink advertises a hardware gain stage that can be timed
	 * against the audio (x-maemo.volume.sync). Absent means it does not,
	 * which is PulseAudio's default for PA_SINK_SYNC_VOLUME too. */
	bool sync;			/* node.always-process as published */
};

struct output {
	struct spa_list link;		/* on impl->outputs */
	char mixer_name[200];		/* the node this output gets when it is
					 * adopted, <prefix>.<card> */
	struct impl *impl;		/* back pointer, module-wide config lives there */
	/* Published to the mixer: the duck amplitude and whether any
	 * absolute stream is running. See mixer/maemo-vol-state.h. */
	struct maemo_vol_state vstate;

	/* What we know about each stream. Main loop only -- the mixer never
	 * reads this list, it reads the per-port word we write into it. */
	struct spa_list streams;

	unsigned int n_peers;

	/* The ladder. Parsed once per distinct curve and owned by the
	 * cache, which outlives every use of it. */
	struct maemo_ladder_cache *ladder_cache;

	const struct maemo_vol_map *map;

	/* The card this output belongs to, when it was made by
	 * enumeration rather than by an announcement. Not hooked, only a
	 * identity: route.dev is the device an announcement bound and has
	 * hooks that belong to it, this one has none. */
	struct pw_impl_device *card_dev;
	bool enumerated;		/* made by outputs_sync, so it is ours to free */
	struct spa_hook card_dev_hook;

	struct codec codec;
	struct boost boost;
	struct route_bind route;
	struct sink_ref sink;

	/* Whether a level for this output is known: adopted from the node, or
	 * named by the applet. Until it is, the ladder stays off the node --
	 * see publish_sink_props(). */
	bool level_known;

	unsigned n_running;		/* streams currently in the running state */
};


struct impl {
	struct spa_hook mod_hook;

	struct spa_hook ctx_hook;

	struct pw_context *ctx;

	/* The codec requests the mixer armed, as a queue of {value,
	 * deadline} drained by a timer armed for the head. A single slot
	 * cannot work: overlapping absolute streams each have a release
	 * armed, the slider staircase produces a transition every ~100 ms,
	 * and an overwritten or reordered entry leaves the codec matching no
	 * current state. See DESIGN.md. */

	/* Direction margins, in microseconds, from module arguments. They
	 * are NOT a hedge against late delivery -- a timer cannot fire
	 * early, so lateness cannot make the codec move too soon. They
	 * insure against a published clock.delay that underestimates the
	 * real pipeline, which is the one way the codec can raise before
	 * the duck has taken effect and produce the loud transient. Raise
	 * late, lower early: every residual lands on the quiet side.
	 *
	 * Default zero rather than Nokia's 5 ms because no measured number
	 * belongs in the code (docs/timing.md). Measure on the target and
	 * set them in the module args. */
	int64_t raise_margin_ns, lower_margin_ns;

	/* What the module arguments asked for, kept so a device that declares
	 * nothing falls back to them instead of to zero: "no per-device value"
	 * and "no margin" are different statements. */
	int64_t raise_margin_default, lower_margin_default;

	/* The mix ports the mixer has told us about, keyed by the peer id it
	 * named them with. This is the other half of the join: a Port global
	 * arriving in on_global_added tells us which stream a peer id
	 * belongs to, and this tells us the port object that id names.
	 * Neither half is enough alone, and neither needs a private header. */
#define PEERS 64
	struct {
		uint32_t peer_id;
		struct maemo_port_data *data;
	} peers[PEERS];

	char *default_tuning;

	char *default_tuning_incall;

	/* The user's slider, as written by the applet: a 0..65536 integer
	 * on the metadata bus, and the rung it lands on once quantised. */
	char *app_target;

	int slider_step;

	bool save_requested;

	/* Our own metadata namespace, created and registered so the
	 * routing policy has something to bind. */
	struct spa_hook md_hook;

	/* A client connection to our own daemon, used only to read the
	 * applet's metadata. */
	struct pw_core *self_core;

	struct pw_registry *self_registry;

	struct spa_hook self_registry_hook;

	struct pw_metadata *app_md;

	struct spa_hook app_md_hook;


	bool debug;

	/* One output for now. The enumeration work turns this into a list,
	 * one per audio device, and nothing else changes. */
	struct output *out;		/* the current output, NULL until a card
				 * exists, every per-output entry point has to
				 * survive that */
	bool vsink_enable;		/* create a mixer node per card */
	char vsink_prefix[128];		/* mixer names are <prefix>.<card> */
	uint32_t default_channels;	/* route channels until a route says otherwise */
	struct spa_list outputs;	/* one struct output per audio device */
};

/* What the device itself says about its hardware volume.
 *
 * The element that carries a route's volume belongs to the device and
 * moves with the route, so a name in a config file is a guess about
 * hardware the config cannot see, wrong on the first card that differs
 * from the one it was written on and wrong in the other direction on a
 * headset, whose gain is an AVRCP command rather than a register. The
 * device can say it, so it does, on the same Props param that carries
 * the hardware volume gate.
 *
 * Three answers, and they are not all the same. The device named an
 * element: write that one. The device publishes Props but names nothing:
 * there is nothing to write, which is a statement and not a gap. The
 * device published no Props at all, or is not an ALSA device that ever
 * heard of the key: that is silence, and silence keeps the configured
 * control rather than disabling the codec path on a misreading. */
struct hw_vol_read {
	bool saw_route;
	bool key_seen;
	uint32_t want_index;
	char elem[128];
};

/* The control that carries the volume for one output. It lives on the
 * route, not on the card: `Speaker` and `Headset` are different outputs
 * with different controls, and a card-wide answer has to guess which one
 * was meant -- and guessed wrong on a UCM card, where the first device
 * that answered belonged to a different output entirely. */
#define HW_VOLUME_ELEMENT_KEY "route.hw-volume-element"

#define RT_COARSE_MAX		(5ull * 1000 * 1000)

/* When to wake next for a deadline.
 *
 * A single long sleep is the accurate thing to ask for and the least
 * accurate thing to get: the deeper the state the idle governor picks
 * for a long nap, the later the wake, and on this hardware a twenty
 * millisecond sleep measures out at a median of 0.8 ms late with a
 * tail past 8. Short naps keep the core in the shallow states, where
 * the wake is worth a tenth of that, so the approach is stepped: one
 * ordinary sleep until the deadline is close, then short steps, and
 * the last interval asked for absolutely so it cannot round past.
 *
 * There is deliberately no notion of a final step stored anywhere.
 * This is a pure function of the deadline and the clock, and the write
 * happens when the clock has reached the deadline and at no other
 * time, so a wake can only ever be early and the response to being
 * early is to ask again. */
#define RT_COARSE_WINDOW	(20ull * 1000 * 1000)

#define RT_FINE_STEP		(200ull * 1000)

/* ---- hardware level path, maemo-hw-ctl.c ---- */
uint64_t now_ns(void);
int rt_ctl_open(struct output *out, const char *card, const char *elem);
void rt_ctl_close(struct output *out);
int rt_ctl_quiesce_cb(struct spa_loop *loop, bool async, uint32_t seq, const void *data, size_t size, void *user_data);
int rt_ctl_publish_cb(struct spa_loop *loop, bool async, uint32_t seq, const void *data, size_t size, void *user_data);
int rt_ctl_rebind(struct output *out, const char *card, const char *elem);
int device_route_cb(void *data, int seq, uint32_t id, uint32_t index, uint32_t next, struct spa_pod *param);
void bind_announced_control(struct output *out);
void bind_abs_ceiling(struct output *out, bool sub);
long rt_amp_to_step(struct output *out, float amp);
void rt_ctl_write(struct output *out, long step);
float rt_step_to_amp(struct output *out, long step);
bool rt_ctl_read(struct output *out, long *step);
void *rt_thread_fn(void *data);
void codec_thread_start(struct output *out);
int rt_wake_cb(struct spa_loop *loop, bool async, uint32_t seq, const void *data, size_t size, void *user_data);
uint64_t rt_next_wake(uint64_t deadline, uint64_t now);
void rt_arm_at(struct output *out, uint64_t when);
int rt_timer_ensure(struct output *out);
void rt_timer_cb(struct spa_source *source);
int rt_done_cb(struct spa_loop *loop, bool async, uint32_t seq, const void *data, size_t size, void *user_data);
void hwq_arm(struct output *out);
int swstep_request_cb(struct spa_loop *loop, bool async, uint32_t seq, const void *data, size_t size, void *user_data);
int codec_request(void *user_data, uint64_t deadline_ns, bool raise);
void hw_timer_cb(void *data, uint64_t expirations);
int write_route_volume(struct output *out, float amp, bool save);
void apply_tuning(struct output *out, const char *csv);
bool dev_usable(struct output *out);
void hold_update(struct impl *impl);

/* ---- the level ---- */

/* Hand a software gain to the mixer.
 *
 * Called with 1.0 and nothing else, for now. The level the user hears is
 * produced by the codec, not by multiplying samples here, so this stays
 * at unity -- but the call and the path through the mixer stay in place,
 * because that path is where the software residual goes when flat volume
 * returns. Removing it now would mean rebuilding it rather than repointing
 * it. */
void set_level(struct output *out, float volume);
void read_abs_pair(struct output *out);
const char *tuning_for(struct output *out);
struct pw_impl_device *find_device(struct pw_context *ctx, uint32_t id);
void on_dev_destroy(void *data);
float read_route_volume(struct output *out);

/* Take a level the route carries and make it the slider, if there is no
 * reason not to: see the adoption in reread_target(). */
void adopt_route_level(struct output *out, float amp);

/* Ask WirePlumber to remember the level this route carries, without
 * writing a level: see the comment on the definition. */
void mark_route_saveable(struct output *out);
void on_dev_info_changed(void *data, const struct pw_device_info *info);
void reread_target(struct output *out);
void publish_tuning_on(struct output *out, struct pw_impl_node *node);
int on_route_property(void *data, uint32_t subject, const char *key, const char *type, const char *value);
bool payload_get_long(const char *p, const char *key, long *out);
struct pw_impl_node *find_sink_node(struct output *out);    /* ---- the hardware ---- */;
void publish_target(struct output *out);
void apply_silence(struct output *out);

extern const struct pw_impl_device_events dev_events;
extern const struct pw_impl_device_events dev_info_events;
void publish_sink_props(struct output *out);
void read_sync_flag(struct output *out);
int create_virtual_sink(struct output *out, const char *name);

/* The name a mixer for this device should carry: the card name with its
 * api prefix gone, `alsa_card.platform-soundcard` becomes
 * `platform-soundcard`, and the mixer is `maemo_mixer.platform-soundcard`.
 * The same rule for built-in, HDMI, USB and Bluetooth, because the name is
 * what the policy matches on.
 */
void maemo_card_name(struct pw_impl_device *dev, char *name, size_t len);

struct output *output_for_device(struct impl *impl, uint32_t dev_id);

/* One output per audio card beyond the announced one. See module-maemo-mix.c. */
void outputs_sync(struct impl *impl);

void output_adopt(struct impl *impl, struct output *o);
