/* The state shared between the module that knows things and the mixer that
 * knows when.
 *
 * Two pieces, deliberately different shapes because their lifetimes are
 * different:
 *
 *   struct maemo_vol_state  -- one 64-bit word, module-owned, lives as long as
 *                              the module. The mixer holds a pointer to it and
 *                              never frees it.
 *   struct maemo_port_data  -- one per input port, mixer-owned, refcounted, so
 *                              the module can hold a reference and write at
 *                              leisure without a pointer that might dangle.
 *
 * The direction of every pointer in here is the same: the mixer points at
 * module memory, and the module points at refcounted mixer memory. Nothing
 * points at something with a shorter lifetime than its own holder, which is
 * the whole of the safety argument.
 */
#ifndef MAEMO_VOL_STATE_H
#define MAEMO_VOL_STATE_H

#include <stdbool.h>
#include <spa/support/loop.h>
#include <stdint.h>

#include "maemo-export.h"

/* A toolchain without LDREXD/STREXD silently routes 8-byte atomics through
 * libatomic, which may be a spinlock. That is still correct and would still
 * pass every test we can run here, and it would put a lock inside a data loop
 * on the device -- so the assumption is asserted rather than trusted. */
#if !defined(__GCC_ATOMIC_LLONG_LOCK_FREE) || __GCC_ATOMIC_LLONG_LOCK_FREE != 2
#error "maemo volume state requires always-lock-free 64-bit atomics"
#endif

/* The published word.
 *
 * duck and abs_active share one word because they must be *seen* together: an
 * absolute stream starting changes both, and a mixer that read abs_active=1
 * with a stale duck would arm the codec against a level that had not landed.
 * A slider move during absolute playback changes duck only, and the bit rides
 * along unchanged, so the mixer sees a duck change with no edge and arms
 * nothing -- which is the correct behaviour and not a special case.
 *
 *   [63:32] duck, IEEE-754 float bits
 *   [31]    abs_active
 *   [30:0]  generation, bumped on every publish
 *
 * The generation travels in the same atomic word, so a reader can tell that the
 * pair it observed belongs to one publish rather than to two.
 */
#define MAEMO_VOL_GEN_MASK	0x7FFFFFFFu

struct maemo_vol_state {
	/* The way back to the module that owns this state. The codec thread,
	 * the control it writes and the deadline it meets belong to one
	 * output, so the arm belongs to the state: file-scope statics can
	 * name only one output, which is right while there is one mixer in
	 * the process and wrong the moment a second card is heard on. */
	struct spa_loop *arm_loop;
	int (*arm_fn)(void *user_data, uint64_t deadline_ns, bool raise);
	void *arm_data;

	/* Explicit alignment: armhf does not guarantee 8-byte alignment for a
	 * member buried in a struct, and a misaligned 64-bit atomic is a
	 * SIGBUS waiting for a layout change rather than a bug in the code. */
	_Atomic uint64_t pub __attribute__((aligned(8)));

	/* Raised while a codec change is owed, so the mixer node can hold its
	 * graph up and be certain of a cycle in which to arm it. Separate from
	 * the published word because it is not part of the pair that must be
	 * seen together: it says "somebody still needs me", nothing more.
	 *
	 * Without this an absolute stream can be the last thing keeping the
	 * graph alive, and its stop both triggers the release and removes the
	 * cycle that would have armed it -- the codec is left at MAX with
	 * nobody left to lower it. */
	_Atomic uint32_t hold __attribute__((aligned(8)));

	/* The last word some mixer instance acted on. Shared, because the
	 * sink and every mix handle observe the same publication and only
	 * one of them should treat it as a transition: five instances each
	 * arming a codec write for one event is five queue entries for one
	 * decision. Whoever wins the compare-exchange arms; the others see
	 * no edge. That is exclusivity from the state rather than from a
	 * role test, so it does not depend on knowing which node is the
	 * sink -- and if two instances on different data loops ever both
	 * win, the queue collapses the duplicate the way it already does. */
	_Atomic uint64_t observed __attribute__((aligned(8)));

	/* The mixer's timing numbers, so a decision made on the main loop
	 * can state when the hardware write should land. */
	_Atomic uint64_t timing __attribute__((aligned(8)));


	/* A gain the module owns and Pulse cannot see.
	 *
	 * The node's volume param is what pipewire-pulse exports as the sink
	 * volume, so writing it announces a change the user never made. The
	 * silence at the bottom of the slider belongs here instead: same
	 * number, same effect on the audio, no notification. Applied by the
	 * sink node only, multiplied with its own volume. */
	_Atomic uint32_t aux __attribute__((aligned(4)));

	/* Which peers carry absolute audio, by peer id.
	 *
	 * The port flag says the same thing, but the port object is created with
	 * the link and the module learns of it on the main loop, so the flag can
	 * arrive a moment after the first buffer and the raise ends up behind
	 * the sound. This table is keyed by the peer id the stream's ports were
	 * published with, which exists when the node appears, long before
	 * anything links to it, so the answer is here before the audio that
	 * needs it. Zero is an empty slot, so ids are stored offset by one.
	 *
	 * A lookup that misses is not an error: it means the mixer has not been
	 * told about that peer, and the port flag is still consulted. */
#define MAEMO_ABS_PEERS	32
	_Atomic uint32_t abs_peers[MAEMO_ABS_PEERS];

	/* "An absolute stream contributed samples to this mix in this cycle."
	 *
	 * Set by a mix handle in the cycle it mixes them and taken by the sink
	 * in that same cycle, because the server runs a node's input mixes
	 * before the node itself. It cannot ride on the buffer instead: the
	 * sink consumes the handle's output a cycle after the handle produced
	 * it, and a raise armed one cycle after the first chunk of a ten
	 * millisecond sound is a raise that arrives after the sound. */
	_Atomic uint32_t abs_data __attribute__((aligned(4)));
};

MAEMO_EXPORT void maemo_vol_state_init(struct maemo_vol_state *s);

/* Module side. */
MAEMO_EXPORT void maemo_vol_state_publish(struct maemo_vol_state *s, float duck, bool abs_active);

/* The graph hold. The module raises it when it decides a codec change is due
 * and lowers it once the write has landed; the mixer reads it to decide
 * whether to ask for a cycle. Both sides only ever set it to a state they
 * know they need, so a lost clear is a node that keeps running, not a codec
 * stuck at MAX -- the conservative direction. */
/* The cycle's absolute data. A mix handle raises it when an absolute port
 * contributed; the sink takes it, which reads and clears in one step so the
 * next cycle starts clean even if the sink skipped a cycle. */
MAEMO_EXPORT void maemo_vol_state_abs_data(struct maemo_vol_state *s);
MAEMO_EXPORT bool maemo_vol_state_take_abs_data(struct maemo_vol_state *s);

/* The per peer absolute table. The module owns the contents; the mixer reads
 * it in the data loop. Ids are stored offset by one so zero stays empty. */
MAEMO_EXPORT void maemo_vol_state_abs_peer_add(struct maemo_vol_state *s,
		uint32_t peer_id);
MAEMO_EXPORT void maemo_vol_state_abs_peer_remove(struct maemo_vol_state *s,
		uint32_t peer_id);
MAEMO_EXPORT bool maemo_vol_state_abs_peer_has(const struct maemo_vol_state *s,
		uint32_t peer_id);

MAEMO_EXPORT void maemo_vol_state_set_hold(struct maemo_vol_state *s, bool hold);
MAEMO_EXPORT bool maemo_vol_state_hold(const struct maemo_vol_state *s);

/* Mixer side: the raw word, decoded with the helpers below. */
uint64_t maemo_vol_state_load(const struct maemo_vol_state *s);

/* Consume the edge. Returns true and fills *pub and *was_abs when the
 * published word has changed since anyone last consumed it; *now_abs tells
 * which way it went. Safe to call from every mixer instance: at most one
 * sees the transition. */
bool maemo_vol_state_edge(struct maemo_vol_state *s, uint64_t *pub,
		bool *now_abs, bool *was_abs);

/* The private gain. Defaults to unity; the module sets it, the sink node
 * multiplies it into its output. */
MAEMO_EXPORT void maemo_vol_state_set_aux(struct maemo_vol_state *s, float g);
MAEMO_EXPORT float maemo_vol_state_aux(const struct maemo_vol_state *s);

float maemo_vol_state_duck(uint64_t pub);
bool maemo_vol_state_abs_active(uint64_t pub);
uint32_t maemo_vol_state_generation(uint64_t pub);

/* ---- per-port data, refcounted ----
 *
 * The reference is the identity. While the module holds one the memory cannot
 * be freed, so it may write whenever it likes, and a recycled global id cannot
 * make it write into a different stream's port. That is strictly better than
 * the generation-counter-plus-scan the first attempt needed, and it is why
 * "never cache the pointer" is no longer a rule anyone has to remember.
 */
#define MAEMO_PORT_ABSOLUTE	(1u << 0)
/* The port was identified as belonging to a playback stream.
 *
 * Ducking is opt-in, not opt-out. The core instantiates the mixer factory on
 * the *output* side as well -- one mix handle per input port of the device
 * sink, whose peer is a device port rather than a stream -- and those handles
 * write the already-mixed sum. Ducking one of those ducks the absolute stream
 * too, which shows up as the click following the slider. Anything the module
 * has not named as a stream is left alone: unknown peers are then silent about
 * the duck rather than wrongly attenuated. */
#define MAEMO_PORT_STREAM	(1u << 1)

struct maemo_port_data {
	_Atomic uint32_t flags;
	_Atomic uint32_t refs;
	uint32_t peer_id;		/* immutable after creation */
};

MAEMO_EXPORT struct maemo_port_data *maemo_port_data_new(uint32_t peer_id);
MAEMO_EXPORT struct maemo_port_data *maemo_port_data_ref(struct maemo_port_data *d);
MAEMO_EXPORT void maemo_port_data_unref(struct maemo_port_data *d);

MAEMO_EXPORT void maemo_port_data_set(struct maemo_port_data *d, uint32_t flag, bool on);
bool maemo_port_data_test(const struct maemo_port_data *d, uint32_t flag);

/* ---- hand-over ----
 *
 * The module calls this once, at init. The mixer cannot look the state up for
 * itself and the module cannot reach into a mix handle the core created, so the
 * factory carries both as far as every node it instantiates.
 *
 * `published` fires on the data loop when a mix port learns its peer, and
 * again with data == NULL when that port goes away. The module refs what it
 * wants to keep and marshals to its own loop; the callback must not block.
 */
typedef void (*maemo_vol_port_cb)(void *user_data, uint32_t peer_id,
		struct maemo_port_data *data);

MAEMO_EXPORT void maemo_vol_state_attach(struct maemo_vol_state *state,
		maemo_vol_port_cb published, void *user_data);

/* ---- asking for a codec change ----
 *
 * The mixer knows when; the module knows what. The payload is a deadline and a
 * direction because the value to write -- MAX on the way up, whatever rung the
 * slider is on now on the way down -- is something only the module can name,
 * and naming it at drain time is also what makes a slider move during absolute
 * playback land on the right rung without a second message.
 *
 * The deadline is absolute, CLOCK_MONOTONIC, and is computed on the data loop
 * from the cycle where the duck actually landed -- not from when the module
 * decided. That is what closes the loud window: the param path main -> data
 * costs milliseconds, and timing the codec from the module's intention would
 * turn those milliseconds into un-ducked audio at MAX.
 */
struct maemo_vol_request {
	uint64_t deadline_ns;
	uint32_t raise;
	int (*fn)(void *user_data, uint64_t deadline_ns, bool raise);
	void *data;
};

typedef int (*maemo_vol_codec_fn)(void *user_data, uint64_t deadline_ns, bool raise);

MAEMO_EXPORT void maemo_vol_arm(struct spa_loop *control_loop,
		maemo_vol_codec_fn fn, void *user_data);

/** Arm one state, which is to say one output: the request from a mixer
 * instance is delivered on the loop that output owns, to the callback
 * armed for it, and never to whatever the module considers current. */
MAEMO_EXPORT void maemo_vol_state_arm(struct maemo_vol_state *state,
		struct spa_loop *control_loop, maemo_vol_codec_fn fn,
		void *user_data);

void maemo_vol_request_codec(struct maemo_vol_state *state, bool raise,
		uint64_t delay_frames, uint32_t rate);

/** Arm against someone else's clock.
 *
 * Same as maemo_vol_request_codec() except that the distance is added to
 * base_ns rather than to the instant this call happens to run. Used when
 * the distance came from a hardware read: the driver measured it at the
 * hardware's own timestamp, and adding a clock reading taken later in the
 * cycle would sum two different instants. base_ns == 0 means "now", i.e.
 * the plain form. */
void maemo_vol_request_codec_at(struct maemo_vol_state *state,
		bool raise, uint64_t delay_frames, uint32_t rate,
		uint64_t base_ns);

/* The mixer's timing numbers, published for whoever decides a codec write is
 * due. delay_frames in the high half, rate in the low: both are fixed for a
 * running graph, so a single store per change is enough and the reader never
 * sees a torn pair. */
void maemo_vol_state_publish_timing(struct maemo_vol_state *st,
		uint64_t delay_frames, uint32_t rate);
void maemo_vol_state_timing(const struct maemo_vol_state *st,
		uint64_t *delay_frames, uint32_t *rate);


/* Mixer side: what the factory hands to each node it creates. */
struct maemo_vol_state *maemo_vol_state_current(void);

/* One state per mixer node, found by the name the module gave that node.
 * An unknown key, and no key at all, answer the attached state, which is
 * what the core's own per-port mix handles get and what every mixer got
 * before there was more than one output. */
MAEMO_EXPORT void maemo_vol_state_register(const char *key,
		struct maemo_vol_state *s);
MAEMO_EXPORT void maemo_vol_state_unregister(struct maemo_vol_state *s);
MAEMO_EXPORT struct maemo_vol_state *maemo_vol_state_lookup(const char *key);
maemo_vol_port_cb maemo_vol_port_cb_current(void);
void *maemo_vol_port_cb_data(void);

#endif /* MAEMO_VOL_STATE_H */
