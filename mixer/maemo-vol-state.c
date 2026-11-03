#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <spa/support/loop.h>
#include <spa/utils/string.h>

#include "maemo-vol-state.h"

/* The two pointers the module handed over. Process-wide on purpose: there is
 * one volume state and it belongs to the module, not to any one node, and the
 * factory needs them before any node exists to carry them. */
static struct maemo_vol_state *attached_state;
static maemo_vol_port_cb attached_cb;
static void *attached_cb_data;

static inline uint32_t f2b(float f)
{
	uint32_t bits;
	memcpy(&bits, &f, sizeof(bits));
	return bits;
}

static inline uint64_t pack(float duck, bool abs_active, uint32_t generation)
{
	uint32_t bits;
	memcpy(&bits, &duck, sizeof(bits));

	return ((uint64_t) bits << 32) |
		(abs_active ? (1u << 31) : 0u) |
		(generation & MAEMO_VOL_GEN_MASK);
}

void maemo_vol_state_init(struct maemo_vol_state *s)
{
	/* Duck at unity, not absolute: this is the state of a mixer with
	 * nothing playing, and it must be a value the module then changes
	 * rather than a blank someone has to remember to fill in. */
	atomic_store_explicit(&s->pub, pack(1.0f, false, 0), memory_order_release);
	atomic_init(&s->observed, pack(1.0f, false, 0));
	atomic_init(&s->aux, f2b(1.0f));
}

void maemo_vol_state_set_aux(struct maemo_vol_state *s, float g)
{
	atomic_store_explicit(&s->aux, f2b(g), memory_order_release);
}

float maemo_vol_state_aux(const struct maemo_vol_state *s)
{
	uint32_t bits = atomic_load_explicit(&s->aux, memory_order_acquire);
	float f;

	memcpy(&f, &bits, sizeof(f));
	return f;
}

void maemo_vol_state_abs_data(struct maemo_vol_state *s)
{
	atomic_store_explicit(&s->abs_data, 1u, memory_order_relaxed);
}

bool maemo_vol_state_take_abs_data(struct maemo_vol_state *s)
{
	return atomic_exchange_explicit(&s->abs_data, 0u, memory_order_relaxed) != 0;
}

void maemo_vol_state_abs_peer_add(struct maemo_vol_state *s, uint32_t peer_id)
{
	uint32_t v = peer_id + 1;
	unsigned i, free_i = MAEMO_ABS_PEERS;

	if (peer_id == UINT32_MAX)
		return;

	for (i = 0; i < MAEMO_ABS_PEERS; i++) {
		uint32_t cur = atomic_load_explicit(&s->abs_peers[i], memory_order_relaxed);
		if (cur == v)
			return;
		if (cur == 0 && free_i == MAEMO_ABS_PEERS)
			free_i = i;
	}
	if (free_i < MAEMO_ABS_PEERS)
		atomic_store_explicit(&s->abs_peers[free_i], v, memory_order_relaxed);
	/* No slot: the fact is not recorded and that peer's port flag remains
	 * the only carrier, which is the older, racier path. Thirty two
	 * simultaneous absolute streams is not a real configuration. */
}

void maemo_vol_state_abs_peer_remove(struct maemo_vol_state *s, uint32_t peer_id)
{
	uint32_t v = peer_id + 1;
	unsigned i;

	if (peer_id == UINT32_MAX)
		return;

	for (i = 0; i < MAEMO_ABS_PEERS; i++)
		if (atomic_load_explicit(&s->abs_peers[i], memory_order_relaxed) == v)
			atomic_store_explicit(&s->abs_peers[i], 0, memory_order_relaxed);
}

bool maemo_vol_state_abs_peer_has(const struct maemo_vol_state *s, uint32_t peer_id)
{
	uint32_t v = peer_id + 1;
	unsigned i;

	if (peer_id == UINT32_MAX)
		return false;

	for (i = 0; i < MAEMO_ABS_PEERS; i++)
		if (atomic_load_explicit(&((struct maemo_vol_state *) s)->abs_peers[i],
				memory_order_relaxed) == v)
			return true;
	return false;
}

void maemo_vol_state_set_hold(struct maemo_vol_state *s, bool hold)
{
	atomic_store_explicit(&s->hold, hold ? 1u : 0u, memory_order_release);
}

bool maemo_vol_state_hold(const struct maemo_vol_state *s)
{
	return atomic_load_explicit(&s->hold, memory_order_acquire) != 0;
}

void maemo_vol_state_publish(struct maemo_vol_state *s, float duck, bool abs_active)
{
	uint64_t old = atomic_load_explicit(&s->pub, memory_order_relaxed);
	uint32_t gen = (uint32_t) (old & MAEMO_VOL_GEN_MASK) + 1;

	atomic_store_explicit(&s->pub, pack(duck, abs_active, gen),
			memory_order_release);
}

uint64_t maemo_vol_state_load(const struct maemo_vol_state *s)
{
	return atomic_load_explicit(&s->pub, memory_order_acquire);
}

bool maemo_vol_state_edge(struct maemo_vol_state *s, uint64_t *pub,
		bool *now_abs, bool *was_abs)
{
	uint64_t cur = atomic_load_explicit(&s->pub, memory_order_acquire);
	uint64_t seen = atomic_load_explicit(&s->observed, memory_order_relaxed);

	if (cur == seen)
		return false;
	/* Only the winner of this exchange reports the edge. The loser
	 * returns false with the current word still available for the gain
	 * path, which is not a transition and must be seen by everyone. */
	if (!atomic_compare_exchange_strong_explicit(&s->observed, &seen, cur,
			memory_order_relaxed, memory_order_relaxed))
		return false;

	*pub = cur;
	*now_abs = maemo_vol_state_abs_active(cur);
	*was_abs = maemo_vol_state_abs_active(seen);
	return *now_abs != *was_abs;
}

float maemo_vol_state_duck(uint64_t pub)
{
	uint32_t bits = (uint32_t) (pub >> 32);
	float duck;
	memcpy(&duck, &bits, sizeof(duck));
	return duck;
}

bool maemo_vol_state_abs_active(uint64_t pub)
{
	return (pub & (1u << 31)) != 0;
}

uint32_t maemo_vol_state_generation(uint64_t pub)
{
	return (uint32_t) (pub & MAEMO_VOL_GEN_MASK);
}

/* ---- the codec request ---- */

static struct spa_loop *arm_loop;
static maemo_vol_codec_fn arm_fn;
static void *arm_data;

void maemo_vol_arm(struct spa_loop *control_loop,
		maemo_vol_codec_fn fn, void *user_data)
{
	arm_loop = control_loop;
	arm_fn = fn;
	arm_data = user_data;
}

void maemo_vol_state_arm(struct maemo_vol_state *state,
		struct spa_loop *control_loop, maemo_vol_codec_fn fn,
		void *user_data)
{
	if (state == NULL)
		return;
	state->arm_loop = control_loop;
	state->arm_fn = fn;
	state->arm_data = user_data;
}

/* Runs on the control loop. All it does is hand the pair to the module: the
 * deadline was fixed on the data loop, and re-deriving it here would inherit
 * every millisecond of whatever else the control loop was doing. */
static int arm_invoke(struct spa_loop *loop, bool async, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	const struct maemo_vol_request *r = data;

	(void) loop; (void) async; (void) seq; (void) user_data;
	if (size != sizeof(*r) || r->fn == NULL)
		return -EINVAL;
	return r->fn(r->data, r->deadline_ns, r->raise != 0);
}

MAEMO_EXPORT void maemo_vol_state_publish_timing(struct maemo_vol_state *st,
		uint64_t delay_frames, uint32_t rate)
{
	uint64_t packed = ((delay_frames & 0xffffffffu) << 32) | rate;

	atomic_store_explicit(&st->timing, packed, memory_order_release);
}

MAEMO_EXPORT void maemo_vol_state_timing(const struct maemo_vol_state *st,
		uint64_t *delay_frames, uint32_t *rate)
{
	uint64_t packed = atomic_load_explicit(
			(__typeof__ (st->timing) *) &st->timing,
			memory_order_acquire);

	*delay_frames = packed >> 32;
	*rate = (uint32_t) packed;
}


void maemo_vol_request_codec_at(struct maemo_vol_state *state,
		bool raise, uint64_t delay_frames, uint32_t rate, uint64_t base_ns)
{
	struct maemo_vol_request r;
	struct timespec ts;
	struct spa_loop *loop = state && state->arm_loop ? state->arm_loop : arm_loop;
	int (*fn)(void *, uint64_t, bool) = state && state->arm_fn ?
		state->arm_fn : arm_fn;

	if (loop == NULL || fn == NULL)
		return;

	if (base_ns == 0)
		clock_gettime(CLOCK_MONOTONIC, &ts);
	else
		/* The driver's instant, not ours: the distance and this stamp
		 * were taken from the same hardware read, so their sum is the
		 * arrival of the frame at the write position and contains no
		 * moment of ours at all. */
		(void) (ts.tv_sec = (time_t)(base_ns / 1000000000ull),
			ts.tv_nsec = (long)(base_ns % 1000000000ull));

	r.deadline_ns = (uint64_t) ts.tv_sec * 1000000000u + ts.tv_nsec +
		(rate ? delay_frames * 1000000000ull / rate : 0);
	r.raise = raise ? 1 : 0;
	r.fn = fn;
	r.data = state && state->arm_fn ? state->arm_data : arm_data;

	/* Copied by value into the invoke queue: nothing is allocated on the
	 * data loop and nothing has to outlive the call. */
	spa_loop_invoke(loop, arm_invoke, 0, &r, sizeof(r), false, NULL);
}

void maemo_vol_request_codec(struct maemo_vol_state *state, bool raise,
		uint64_t delay_frames, uint32_t rate)
{
	maemo_vol_request_codec_at(state, raise, delay_frames, rate, 0);
}

/* ---- per-port data ---- */

struct maemo_port_data *maemo_port_data_new(uint32_t peer_id)
{
	struct maemo_port_data *d = calloc(1, sizeof(*d));

	if (d == NULL)
		return NULL;
	atomic_init(&d->flags, 0);
	atomic_init(&d->refs, 1);
	d->peer_id = peer_id;
	return d;
}

struct maemo_port_data *maemo_port_data_ref(struct maemo_port_data *d)
{
	if (d != NULL)
		atomic_fetch_add_explicit(&d->refs, 1, memory_order_relaxed);
	return d;
}

void maemo_port_data_unref(struct maemo_port_data *d)
{
	if (d == NULL)
		return;
	if (atomic_fetch_sub_explicit(&d->refs, 1, memory_order_acq_rel) == 1)
		free(d);
}

void maemo_port_data_set(struct maemo_port_data *d, uint32_t flag, bool on)
{
	uint32_t want = on ? (atomic_load_explicit(&d->flags, memory_order_relaxed) | flag)
		: (atomic_load_explicit(&d->flags, memory_order_relaxed) & ~flag);

	/* Single writer per flag in practice -- only the module sets these --
	 * so a plain store of the recomputed word is enough. If a second
	 * writer ever appears this becomes a compare-exchange loop, not a
	 * lock. */
	atomic_store_explicit(&d->flags, want, memory_order_release);
}

bool maemo_port_data_test(const struct maemo_port_data *d, uint32_t flag)
{
	return (atomic_load_explicit(&d->flags, memory_order_acquire) & flag) != 0;
}

/* ---- hand-over ---- */

void maemo_vol_state_attach(struct maemo_vol_state *state,
		maemo_vol_port_cb published, void *user_data)
{
	attached_state = state;
	attached_cb = published;
	attached_cb_data = user_data;
}

struct maemo_vol_state *maemo_vol_state_current(void)
{
	return attached_state;
}

maemo_vol_port_cb maemo_vol_port_cb_current(void)
{
	return attached_cb;
}

void *maemo_vol_port_cb_data(void)
{
	return attached_cb_data;
}

/* Which volume state belongs to which mixer node.
 *
 * One mixer per output means one state per output, and the node is the only
 * thing the spa factory can see, so the module registers each state under the
 * name of the mixer it belongs to and the factory asks for it by that name.
 *
 * An unknown key, and no key at all, answer the attached state. That is not a
 * fallback out of laziness, the core instantiates this factory for its own
 * per-port mix handles with no creation dict entries of ours, and those must
 * keep following the state the module attached exactly as they did before
 * there was more than one.
 */
#define MAEMO_VOL_STATE_MAX 8

struct vol_state_slot {
	char key[128];
	struct maemo_vol_state *state;
};

static struct vol_state_slot vol_states[MAEMO_VOL_STATE_MAX];

MAEMO_EXPORT void
maemo_vol_state_register(const char *key, struct maemo_vol_state *s)
{
	uint32_t i;

	if (key == NULL || s == NULL)
		return;

	for (i = 0; i < MAEMO_VOL_STATE_MAX; i++) {
		if (vol_states[i].state == NULL) {
			snprintf(vol_states[i].key, sizeof(vol_states[i].key), "%s", key);
			vol_states[i].state = s;
			return;
		}
	}
}

MAEMO_EXPORT void
maemo_vol_state_unregister(struct maemo_vol_state *s)
{
	uint32_t i;

	for (i = 0; i < MAEMO_VOL_STATE_MAX; i++) {
		if (vol_states[i].state == s) {
			vol_states[i].state = NULL;
			vol_states[i].key[0] = '\0';
		}
	}
}

MAEMO_EXPORT struct maemo_vol_state *
maemo_vol_state_lookup(const char *key)
{
	uint32_t i;

	if (key == NULL)
		return attached_state;

	for (i = 0; i < MAEMO_VOL_STATE_MAX; i++) {
		if (vol_states[i].state != NULL && spa_streq(vol_states[i].key, key))
			return vol_states[i].state;
	}
	return attached_state;
}
