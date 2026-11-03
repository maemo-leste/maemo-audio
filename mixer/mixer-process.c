/* The audio thread.
 *
 * Runs every quantum, under the data-loop lock, and must not allocate,
 * block, or take a lock it does not already hold. All it does is produce
 * the output buffer for one cycle: pull each input, sum it, and scale the
 * sum by the mixer's level.
 *
 * The level is read once per cycle, before any input is touched, so every
 * sample in a buffer is mixed against one value. Reading it per port, or
 * per segment, would let a change arriving mid-buffer split the buffer
 * across two levels -- which is not a smoother transition but a click at
 * the seam, and one whose position depends on when the write happened to
 * land.
 */

#include <inttypes.h>
#include <fcntl.h>
#include <unistd.h>
#include <spa/buffer/meta.h>
#include <time.h>

#include "mixer-impl.h"

#include <stdlib.h>
#include <time.h>


#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic
SPA_LOG_TOPIC_DEFINE_STATIC(log_topic, "spa.maemo.mixer");

int mixer_clear_buffers(struct impl *this, struct port *port)
{
	uint32_t i, j;

	spa_log_debug(this->log, "%p: clear buffers %p %d", this, port, port->n_buffers);
	for (i = 0; i < port->n_buffers; i++) {
		struct buffer *b = &port->buffers[i];
		if (SPA_FLAG_IS_SET(b->flags, BUFFER_FLAG_MAPPED)) {
			for (j = 0; j < b->buffer->n_datas; j++) {
				if (b->datas[j]) {
					spa_log_debug(this->log, "%p: unmap buffer %d data %d %p",
							this, i, j, b->datas[j]);
					munmap(b->datas[j], b->buffer->datas[j].maxsize);
					b->datas[j] = NULL;
				}
			}
			SPA_FLAG_CLEAR(b->flags, BUFFER_FLAG_MAPPED);
		}
	}
	port->n_buffers = 0;
	spa_list_init(&port->queue);
	return 0;
}

int mixer_queue_buffer(struct impl *this, struct port *port, struct buffer *b)
{
	if (SPA_FLAG_IS_SET(b->flags, BUFFER_FLAG_QUEUED))
		return -EINVAL;

	spa_list_append(&port->queue, &b->link);
	SPA_FLAG_SET(b->flags, BUFFER_FLAG_QUEUED);
	spa_log_trace_fp(this->log, "%p: queue buffer %d", this, b->id);
	return 0;
}

static struct buffer *mixer_dequeue_buffer(struct impl *this, struct port *port)
{
	struct buffer *b;

	if (spa_list_is_empty(&port->queue))
		return NULL;

	b = spa_list_first(&port->queue, struct buffer, link);
	spa_list_remove(&b->link);
	SPA_FLAG_CLEAR(b->flags, BUFFER_FLAG_QUEUED);
	spa_log_trace_fp(this->log, "%p: dequeue buffer %d", this, b->id);
	return b;
}

/* The level this cycle is mixed at.
 *
 * One value for the whole node: the mixer has no opinion about which
 * stream a sample came from. Mute wins over volume rather than
 * multiplying into it so that unmuting returns to whatever level was set
 * while muted, which is what a mute is for. */
static inline float node_gain(const struct impl *this, uint32_t chan)
{
	float g;

	if (this->props.mute)
		return 0.0f;
	g = this->props.volume;

	/* The module's private gain, applied in the sink role only: a mix
	 * handle would multiply it once per active link. chan is a channel
	 * index here and SPA_ID_INVALID in the mix-handle role. */
	if (chan != SPA_ID_INVALID && this->vstate != NULL)
		g *= maemo_vol_state_aux(this->vstate);

	return g;
}

/* The duck, applied per input.
 *
 * Only the mix-handle role scales: it owns exactly one input and is the only
 * node that can tell an absolute stream from a normal one. The sink applies
 * unity, which for one input per channel falls into the passthrough path and
 * so costs nothing. Applying the duck at the sink as well would multiply it
 * once per active link -- measured at four handles plus the sink on a stereo
 * stream, which is five applications of the same number.
 *
 * The absolute stream itself is never ducked: it sits at unity so it reaches
 * the converter at its calibrated level while the codec is at MAX. */
static inline float input_gain(const struct impl *this, struct port *inport,
		uint32_t chan, uint64_t pub)
{
	float g;

	if (chan != SPA_ID_INVALID)
		g = 1.0f;			/* sink role: unity, passthrough */
	else if (!maemo_vol_state_abs_active(pub))
		g = 1.0f;			/* nothing absolute: the codec carries it */
	else if (inport->vol == NULL ||
		 !maemo_port_data_test(inport->vol, MAEMO_PORT_STREAM))
		g = 1.0f;			/* not a stream input: never ducked */
	else if (maemo_port_data_test(inport->vol, MAEMO_PORT_ABSOLUTE))
		g = 1.0f;
	else
		g = maemo_vol_state_duck(pub);

	/* Reported when it changes rather than once per port: the defect this
	 * is chasing is a stream that is not ducked while the codec is up,
	 * and a single early line cannot show that -- the mark can arrive
	 * after the first cycle, or land on one port object of a peer that
	 * owns several. The published word is printed alongside so a gain of
	 * unity is distinguishable from a duck of unity, which look the same
	 * in the output and are not the same thing. */
	if (g != inport->diag_gain) {
		inport->diag_gain = g;
		spa_log_debug(this->log,
			"gain: port %u vol=%p peer %u stream=%d abs=%d -> %.4f "
			"(word abs=%d duck=%.4f)",
			inport->id, (void *) inport->vol,
			inport->vol ? inport->vol->peer_id : 0u,
			inport->vol ? !!maemo_port_data_test(inport->vol,
					MAEMO_PORT_STREAM) : -1,
			inport->vol ? !!maemo_port_data_test(inport->vol,
					MAEMO_PORT_ABSOLUTE) : -1,
			(double) g,
			maemo_vol_state_abs_active(pub) ? 1 : 0,
			(double) maemo_vol_state_duck(pub));
	}
	return g;
}

/* Fold the eligible inputs into one output port.
 *
 * chan == SPA_ID_INVALID means "every input": that is the mix-handle
 * role, where the core made this instance the mixer for a single port
 * and there is exactly one destination.  In the standalone sink role
 * chan is the channel index, and only inputs whose port id matches are
 * folded in -- which is the whole reason left stops landing in the right
 * ear.
 *
 * Returns SPA_STATUS_HAVE_DATA when this output produced a buffer. */
static int
mixer_process_output(struct impl *this, struct port *outport,
		uint32_t chan, uint32_t cycle)
{
	struct spa_io_buffers *outio, *inio;
	struct port *inport;
	struct buffer **buffers = this->mix_buffers;
	struct buffer *outb;
	const void **datas = this->mix_datas;
	float *vols = this->mix_vols;
	const struct spa_io_clock *swclk = this->position ? &this->position->clock : NULL;
	const float gain = maemo_swstep_apply(&this->swstep, node_gain(this, chan),
			swclk ? swclk->position : 0,
			swclk ? (uint64_t) (swclk->delay > 0 ? swclk->delay : 0) : 0,
			swclk ? swclk->rate.denom : 0);

	/* Log the ones that are not unity: the question is not how often
	 * the volume op runs (it always runs) but how many instances end
	 * up scaling by the user's number. */
	if (gain != 1.0f && this->gain_apps < 8) {
		this->gain_apps++;
		spa_log_info(this->log,
				"NON-UNITY gain applied: impl %p chan %u gain %.4f pos %" PRIu64,
				(void *) this, chan, (double) gain,
				swclk ? swclk->position : 0);
	}
	struct spa_data *d;
	uint32_t n_buffers = 0;
	uint32_t maxsize = UINT32_MAX;
	bool abs_contrib = false;

	/* The length of the cycle is the graph's, not the shortest input's.
	 * A chunk shorter than the quantum is the end of a sound, and the
	 * producer has already filled the rest of its buffer with silence,
	 * so mixing the whole quantum over that port contributes exactly
	 * what it should. Deriving the length from the inputs instead would
	 * shorten the cycle for every other port as well: one notification
	 * running out would cut the music. */
	if (this->position && this->position->clock.duration > 0)
		maxsize = (uint32_t) this->position->clock.duration * sizeof(float);

	if ((outio = outport->io[cycle]) == NULL)
		return -EIO;

	/* Already produced and not yet consumed downstream.  Skip this
	 * output; the others still have to run. */
	if (SPA_UNLIKELY(outio->status == SPA_STATUS_HAVE_DATA))
		return SPA_STATUS_HAVE_DATA;

	/* recycle */
	if (SPA_LIKELY(outio->buffer_id < outport->n_buffers)) {
		mixer_queue_buffer(this, outport, &outport->buffers[outio->buffer_id]);
		outio->buffer_id = SPA_ID_INVALID;
	}

	spa_list_for_each(inport, &this->mix_list, mix_link) {
		struct buffer *inb;
		struct spa_data *bd;
		uint32_t size, offs;

		if (chan != SPA_ID_INVALID && inport->id != chan)
			continue;

		inio = inport->io[cycle];

		if (inio->buffer_id >= inport->n_buffers ||
		    inio->status != SPA_STATUS_HAVE_DATA) {
			spa_log_trace_fp(this->log, "%p: skip input idx:%d "
					"io:%p status:%d buf_id:%d n_buffers:%d", this,
				inport->id, inio, inio->status, inio->buffer_id, inport->n_buffers);
			continue;
		}

		inb = &inport->buffers[inio->buffer_id];
		bd = &inb->buffer->datas[0];

		offs = SPA_MIN(bd->chunk->offset, bd->maxsize);
		size = SPA_MIN(bd->maxsize - offs, bd->chunk->size);

		spa_log_trace_fp(this->log, "%p: mix input %d %p->%p %d %d/%d %d:%d/%d %u", this,
				inport->id, inio, outio, inio->status, inio->buffer_id, inport->n_buffers,
				offs, size, (int)sizeof(float),
				bd->chunk->flags);

		/* A port with nothing to say must still consume its buffer
		 * or the stream upstream stalls; it just does not reach the
		 * mix. */
		if (!SPA_FLAG_IS_SET(bd->chunk->flags, SPA_CHUNK_FLAG_EMPTY) &&
		    gain > 0.0f) {
			if (chan == SPA_ID_INVALID) {
				/* handle role: this port's peer is the stream,
				 * so the kind is known here. The sink cannot be
				 * told it directly -- the core gives a handle its
				 * peer and gives the sink nothing -- so the flag
				 * goes through the shared state, where the sink
				 * reads it in this same cycle. */
				if (inport->vol != NULL &&
				    (maemo_port_data_test(inport->vol,
						MAEMO_PORT_ABSOLUTE) ||
				     (this->vstate != NULL &&
				      maemo_vol_state_abs_peer_has(this->vstate,
						inport->vol->peer_id)))) {
					abs_contrib = true;
				}
			}
			datas[n_buffers] = SPA_PTROFF(bd->data, offs, void);
			/* Per-channel mix, so one float is one frame. */
			vols[n_buffers] = gain *
				input_gain(this, inport, chan, this->vstate_last);
			buffers[n_buffers++] = inb;
		}

		inio->status = SPA_STATUS_NEED_DATA;
	}

	outb = mixer_dequeue_buffer(this, outport);
	if (SPA_UNLIKELY(outb == NULL)) {
		int suppressed;
		if (outport->n_buffers > 0 &&
		    (suppressed = spa_ratelimit_test(&this->rate_limit,
				this->position->clock.nsec)) >= 0)
			spa_log_warn(this->log, "%p: (%d suppressed) out of buffers (%d)", this,
					suppressed, outport->n_buffers);
		return -EPIPE;
	}

	d = outb->buf.datas;

	/* One input at unity is a copy, not a mix: hand the buffer
	 * downstream untouched rather than reading and rewriting every
	 * sample to multiply it by one. */
	if (n_buffers == 1 && vols[0] == 1.0f &&
	    SPA_FLAG_IS_SET(d[0].flags, SPA_DATA_FLAG_DYNAMIC)) {
		spa_log_trace_fp(this->log, "%p: %d passthrough", this, n_buffers);
		*outb->buffer = *buffers[0]->buffer;
	} else {
		*outb->buffer = outb->buf;

		maxsize = SPA_MIN(maxsize, d[0].maxsize);

		d[0].chunk->offset = 0;
		d[0].chunk->size = maxsize;
		d[0].chunk->stride = sizeof(float);
		SPA_FLAG_UPDATE(d[0].chunk->flags, SPA_CHUNK_FLAG_EMPTY, n_buffers == 0);

		spa_log_trace_fp(this->log, "%p: %d mix %d", this, n_buffers, maxsize);

		{
			const void *seg[MAX_PORTS];
			uint32_t i;

			for (i = 0; i < n_buffers; i++)
				seg[i] = datas[i];

			this->vol_ops.process(d[0].data, seg, vols, n_buffers,
					maxsize / sizeof(float));
		}

	}

	/* The sink runs after its input mixes in this cycle, so it will read
	 * this before the cycle ends. Not on the buffer: the sink consumes
	 * what the handle publishes a cycle later, and a cycle is longer than
	 * the sound that has to be audible. */
	if (abs_contrib && this->vstate != NULL) {
		maemo_vol_state_abs_data(this->vstate);
	}

	outio->buffer_id = outb->id;
	outio->status = SPA_STATUS_HAVE_DATA;

	return SPA_STATUS_HAVE_DATA;
}

int mixer_node_process(void *object)
{
	struct impl *this = object;
	uint32_t cycle = this->position->clock.cycle & 1;
	uint32_t i;
	int res;
	/* Needed on both sides of the output loop: the word is read before it,
	 * the deadline is computed after it. */
	const struct spa_io_clock *c = &this->position->clock;
	uint32_t rate = (uint32_t) (c->rate.denom ?
		c->rate.num / c->rate.denom : 0);

	spa_return_val_if_fail(this != NULL, -EINVAL);

	/* Every output port is produced from the same quantum.  Returning
	 * early on the first one would leave the others with no data for
	 * this cycle, which downstream reads as a dropout on that channel
	 * rather than a scheduling decision. */
	/* One read of the module's word per cycle, and every decision below
	 * derives from that single value: reading it twice could arm against
	 * a state that never existed.
	 *
	 * Only abs_active arms anything. A duck change with the bit steady is
	 * a slider move during absolute playback -- the codec is already at
	 * MAX and belongs there, so the new level is simply applied to this
	 * cycle's samples and nothing is armed. */
	if (this->vstate != NULL) {
		/* Read the current word unconditionally. The gain path needs
		 * it in every instance, and only the winner of the edge
		 * comparison is told anything happened: taking the word from
		 * the edge call instead would leave every losing instance --
		 * which is every mix handle, since the sink gets there first
		 * -- holding a stale duck and mixing unducked audio at MAX,
		 * i.e. the music jumping a full rung louder on each click. */
		/* The mixer is not the graph driver, so its own clock can
		 * carry a denominator without a numerator -- a fraction that
		 * is "set" but says nothing. Test the value that comes out,
		 * not the field that goes in: testing the denominator here
		 * produced rate 0 and a deadline of "now", which dropped the
		 * codec while the tail of the sample was still in the pipe. */
		if (rate == 0)
			rate = this->sink_rate;

		/* Refresh the word every instance reads for the gain path.
		 * This is not optional: an instance that never refreshed would
		 * mix with a stale duck, which is the music jumping a full
		 * rung louder on each click. */
		this->vstate_last = maemo_vol_state_load(this->vstate);

		/* Hold the graph up while a codec change is owed.
		 *
		 * An absolute stream can be the last thing keeping this graph
		 * alive: its stop is both the event that makes a release due
		 * and the event that would pause the graph, so the release
		 * would have no cycle to be armed in and the codec would be
		 * left at MAX with nobody left to lower it. The module raises
		 * the hold when it decides a change is due and drops it once
		 * the write has landed; all this does is make the node ask for
		 * the cycles that decision needs.
		 *
		 * Only the sink instance has a graph to hold up -- a mix
		 * handle publishing this would be a node asking to be driven
		 * that has no say in what drives it. */
		/* node.always-process is NOT published here.
	 *
	 * Emitting a props change from the data loop makes the core walk the
	 * graph in that context, and a graph walk in the realtime thread
	 * ends with protocol-native calling impl_ext_end_resource from the
	 * wrong context -- "Device or resource busy", then the server. The
	 * walk has to be requested by whoever owns the decision, on the main
	 * loop, and the decision was never ours: the module raises the hold
	 * when it decides a codec change is due and drops it when the write
	 * lands. It now publishes the property itself.
	 *
	 * Nothing about the timing changes. The mixer still arms from its own
	 * cycle and still reads the hold to know whether it is owed a cycle;
	 * only the announcement moved, and it moved earlier -- the property
	 * used to change on the first cycle after the decision, up to a
	 * quantum late, and now changes when the decision is made. */


		if (rate > 0)
			maemo_vol_state_publish_timing(this->vstate,
					c->delay, rate);
	}

	for (i = 0; i < this->n_out_ports; i++) {
		res = mixer_process_output(this, GET_OUT_PORT(this, i),
				this->sink_channels > 0 ? i : SPA_ID_INVALID,
				cycle);
		if (res < 0)
			return res;
	}

	/* Arm the codec change after the outputs have been produced, so that
	 * "this cycle carried absolute samples" is already known when the
	 * deadline is computed from this cycle. */
	if (this->vstate != NULL) {
		/* The deadline is `now + clock.delay/rate` taken from this
		 * node's own clock in this cycle, and it is handed to the
		 * module as an absolute instant. It must be taken here and not
		 * derived from published numbers on the control loop, because
		 * the distance that matters is from the frame the gain was
		 * applied to, and only this cycle knows which frame that was.
		 *
		 * One instance arms, and it is this one: it is the node the
		 * graph drives and the node that can hold the graph up, so an
		 * edge cannot occur while it is starved of cycles.
		 *
		 * The gain is applied once per cycle, so the cycle position
		 * is the frame the change lands on and there is no finer
		 * clock here to account for. Reading the published word more
		 * often than once per cycle was tried and cannot observe an
		 * event any earlier: the mixer is asked to run once per
		 * quantum and the mix itself takes microseconds, so a
		 * mid-cycle read only splits one buffer across two levels. */
		if (this->sink_channels > 0 && rate > 0) {
			bool abs_now = maemo_vol_state_abs_active(this->vstate_last);
			bool abs_data = maemo_vol_state_take_abs_data(this->vstate);
			bool ready;

			/* The samples are the trigger, not the word. The word is
			 * published from the control loop when the stream goes
			 * running and can arrive a cycle after the audio it
			 * describes, and a raise armed a cycle after the first
			 * chunk of a ten millisecond sound is a raise that
			 * arrives after the sound. So absolute samples in this
			 * cycle, with the codec not yet up, are a raise; the word
			 * says what to aim at and when to come back down. */
			ready = abs_data ? !this->abs_armed :
				(!abs_now && abs_now != this->abs_armed);

			if (ready) {
				uint64_t off;

				/* Aim at the distance the backend reports
				 * for its own hardware, and at nothing if
				 * it reports none.
				 *
				 * A Bluetooth sink reports clock.delay 0:
				 * its route volume is an AVRCP command, not
				 * a register we can schedule against the
				 * samples, and the latency it declares is the
				 * distance to its speaker buffer rather than
				 * to a gain stage we can aim at. Aiming at
				 * that figure was tried and made the
				 * notification inaudible, because the gain
				 * lands after the samples it was meant to
				 * cover.
				 *
				 * This is Nokia's rule for a sink that has
				 * not set PA_SINK_SYNC_VOLUME: the hardware
				 * is written where it is written, from the
				 * main loop, unsynchronised, and the change
				 * is allowed to be slightly out of step with
				 * the audio. */
				uint64_t base = 0;

				/* Aim on the distance the driver publishes.
				 *
				 * There was a second candidate: the raw fill
				 * at the hardware instant, published as a
				 * pair with the driver's own stamp. It aimed
				 * differently and measured worse on the USB
				 * rig, so the switch between them is gone,
				 * and so is the pair that fed it. */
				{
					/* The distance was measured when the stamp
					 * was taken, not now, and the hardware has
					 * been consuming ever since. A cycle that
					 * runs late is not a cycle with a stale
					 * reading in it; it is a cycle whose reading
					 * has to be projected forward by its own
					 * age, or every edge in it lands late by
					 * exactly that age -- and a late release
					 * is an exposure. */
					uint64_t age_frames = 0;
					if (c->nsec != 0) {
						struct timespec tn;
						clock_gettime(CLOCK_MONOTONIC, &tn);
						{
							uint64_t now_ns = (uint64_t) tn.tv_sec *
								1000000000u + tn.tv_nsec;
							if (now_ns > c->nsec)
								age_frames = (uint64_t)
									((now_ns - c->nsec) *
									 (uint64_t) rate / 1000000000ull);
						}
					}
					{
						uint64_t base = (uint64_t) c->delay;
						off = base > age_frames ? base - age_frames : 0;
					}
				}


				/* Which way to go: samples in this cycle mean up no
				 * matter what the word says, because the word can
				 * be a cycle behind the audio it describes. */
				this->abs_armed = abs_data || abs_now;
				maemo_vol_request_codec_at(this->vstate, abs_now, off, rate, base);
			}
		}
	}


	return SPA_STATUS_HAVE_DATA | SPA_STATUS_NEED_DATA;
}
