/* One port: an input carrying one stream, or one channel of the output.
 *
 * Ports are created and destroyed by the core as links come and go, so
 * this is where the mixer's state is genuinely dynamic: the format
 * negotiation, the buffer ring, and the mix link a port joins when the
 * core wires a stream onto it.
 *
 * A port carries no level and no identity. The mixer has one level for
 * everything and no opinion about which stream a sample came from, so
 * there is nothing per-port to set and nothing per-port to keep in step
 * with anything else.
 */

#include "mixer-impl.h"

#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic
SPA_LOG_TOPIC_DEFINE_STATIC(log_topic, "spa.maemo.mixer");

void mixer_port_emit_info(struct impl *this, struct port *port, bool full)
{
	uint64_t old = full ? port->info.change_mask : 0;
	if (full)
		port->info.change_mask = port->info_all;
	if (port->info.change_mask) {
		spa_node_emit_port_info(&this->hooks,
				port->direction, port->id, &port->info);
		port->info.change_mask = old;
	}
}

struct port *mixer_port_alloc(struct impl *this)
{
	struct port *port;
	if (!spa_list_is_empty(&this->free_list)) {
		port = spa_list_first(&this->free_list, struct port, link);
		spa_list_remove(&port->link);
	} else {
		port = calloc(1, sizeof(struct port));
	}
	return port;
}

int mixer_node_add_port(void *object, enum spa_direction direction, uint32_t port_id,
		const struct spa_dict *props)
{
	struct impl *this = object;
	struct port *port;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(CHECK_FREE_IN_PORT(this, direction, port_id), -EINVAL);

	if ((port = mixer_port_alloc(this)) == NULL)
		return -errno;

	port->direction = direction;
	port->id = port_id;
	port->diag_gain = -1.0f;

	spa_list_init(&port->queue);
	port->info_all = SPA_PORT_CHANGE_MASK_FLAGS |
			SPA_PORT_CHANGE_MASK_PARAMS;
	port->info = SPA_PORT_INFO_INIT();
	port->info.flags = SPA_PORT_FLAG_NO_REF |
			   SPA_PORT_FLAG_DYNAMIC_DATA |
			   SPA_PORT_FLAG_REMOVABLE |
			   SPA_PORT_FLAG_OPTIONAL;
	port->params[0] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
	port->params[1] = SPA_PARAM_INFO(SPA_PARAM_Meta, SPA_PARAM_INFO_READ);
	port->params[2] = SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
	port->params[3] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
	port->params[4] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
	port->info.params = port->params;
	port->info.n_params = 5;

	this->in_ports[port_id] = port;
	spa_list_append(&this->port_list, &port->link);

	spa_log_info(this->log, "PORT add    %p dir=%d id=%u", this,
			direction, port_id);
	mixer_port_emit_info(this, port, true);

	return 0;
}

int
mixer_node_remove_port(void *object, enum spa_direction direction, uint32_t port_id)
{
	struct impl *this = object;
	struct port *port;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(CHECK_IN_PORT(this, direction, port_id), -EINVAL);

	port = GET_IN_PORT (this, port_id);
	this->in_ports[port_id] = NULL;
	spa_list_remove(&port->link);

	if (port->have_format && this->have_format) {
		if (--this->n_formats == 0)
			this->have_format = false;
	}

	spa_log_info(this->log, "PORT remove %p dir=%d id=%u",
			this, direction, port_id);

	/* Release the association before the structure is wiped. The core
	 * does not send a mix-info release with an invalid peer when it
	 * tears a port down -- it just removes it -- so this is the only
	 * place that can tell the module the stream is gone. Skipping it
	 * leaked both the reference and the module's peer slot: every link
	 * that came and go permanently consumed one, and once the table was
	 * full a new stream was no longer recognised as absolute, so music
	 * played unducked and every notification bumped. */
	if (port->vol != NULL) {
		maemo_vol_port_cb cb = maemo_vol_port_cb_current();
		if (cb != NULL)
			cb(maemo_vol_port_cb_data(), port->vol->peer_id, NULL);
		maemo_port_data_unref(port->vol);
		port->vol = NULL;
	}

	spa_memzero(port, sizeof(struct port));
	spa_list_append(&this->free_list, &port->link);

	spa_node_emit_port_info(&this->hooks, direction, port_id, NULL);

	return 0;
}

int mixer_port_enum_formats(void *object, struct port *port,
			     uint32_t index,
			     struct spa_pod **param,
			     struct spa_pod_builder *builder)
{
	struct impl *this = object;

	switch (index) {
	case 0:
		if (this->have_format) {
			*param = spa_format_audio_dsp_build(builder, SPA_PARAM_EnumFormat,
					&this->format.info.dsp);
		} else {
			*param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
				SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_audio),
				SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_dsp),
				SPA_FORMAT_AUDIO_format,   SPA_POD_Id(SPA_AUDIO_FORMAT_DSP_F32));
		}
		break;
	default:
		return 0;
	}
	return 1;
}

int
mixer_port_enum_params(void *object, int seq,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t id, uint32_t start, uint32_t num,
			   const struct spa_pod *filter)
{
	struct impl *this = object;
	struct port *port;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_result_node_params result;
	uint32_t count = 0;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);
	spa_return_val_if_fail(CHECK_PORT_ANY(this, direction, port_id), -EINVAL);

	port = GET_PORT_ANY(this, direction, port_id);

	result.id = id;
	result.next = start;
next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_EnumFormat:
		if ((res = mixer_port_enum_formats(this, port, result.index, &param, &b)) <= 0)
			return res;
		break;

	case SPA_PARAM_Format:
		if (result.index > 0)
			return 0;

		/* Ours from the beginning: the port sums planar float and that
		 * is not a fact the negotiation changes. Refusing until
		 * have_format left readers showing an unknown format for a
		 * node that has always known what it carries. */
		param = spa_format_audio_dsp_build(&b, id, &this->format.info.dsp);
		break;

	case SPA_PARAM_Buffers:
		if (port == NULL || !port->have_format)
			return -EIO;
		if (result.index > 0)
			return 0;

		param = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamBuffers, id,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(1, 1, MAX_BUFFERS),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
			SPA_PARAM_BUFFERS_size,    SPA_POD_CHOICE_RANGE_Int(
								this->quantum_limit * this->stride,
								16 * this->stride,
								INT32_MAX),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(this->stride));
		break;

	case SPA_PARAM_Meta:
		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamMeta, id,
				SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
				SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
			break;
		default:
			return 0;
		}
		break;

	case SPA_PARAM_IO:
		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_Buffers),
				SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_buffers)));
			break;
		case 1:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_AsyncBuffers),
				SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_async_buffers)));
			break;
		default:
			return 0;
		}
		break;

	default:
		return -ENOENT;
	}

	if (spa_pod_filter(&b, &result.param, param, filter) < 0)
		goto next;

	spa_node_emit_result(&this->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);

	if (++count != num)
		goto next;

	return 0;
}

int mixer_port_set_format(void *object,
			   enum spa_direction direction,
			   uint32_t port_id,
			   uint32_t flags,
			   const struct spa_pod *format)
{
	struct impl *this = object;
	struct port *port;
	int res;

	port = GET_PORT(this, direction, port_id);

	/* Intent: refuse to re-format a port that is already wired to a
	 * buffer.  port->io is `struct spa_io_buffers *[2]`, so
	 * comparing the array itself against NULL is always false and the
	 * test degenerates into "never while started".  Upstream's mix
	 * handle gets away with that because it is never asked to
	 * re-format while running.  We are a standalone sink: adding a
	 * stream to a running mixer is the ordinary case, so test the
	 * pointers that actually say whether this port is wired. */
	spa_return_val_if_fail(!this->started ||
			(port->io[0] == NULL && port->io[1] == NULL), -EIO);

	if (format == NULL) {
		if (port->have_format) {
			port->have_format = false;
			if (--this->n_formats == 0)
				this->have_format = false;
			mixer_clear_buffers(this, port);
		}
	} else {
		struct spa_audio_info info = { 0 };

		if ((res = spa_format_parse(format, &info.media_type, &info.media_subtype)) < 0)
			return res;

		if (info.media_type != SPA_MEDIA_TYPE_audio ||
		    info.media_subtype != SPA_MEDIA_SUBTYPE_dsp)
			return -EINVAL;

		if (spa_format_audio_dsp_parse(format, &info.info.dsp) < 0)
			return -EINVAL;

		if (info.info.dsp.format != SPA_AUDIO_FORMAT_DSP_F32)
			return -EINVAL;

		if (!this->have_format) {
			this->stride = sizeof(float);
			this->have_format = true;
			this->format = info;
		}
		if (!port->have_format) {
			this->n_formats++;
			port->have_format = true;
			spa_log_info(this->log, "FMTSET dir=%d port=%u ACCEPTED dsp=%u",
				direction, port_id, (unsigned) info.info.dsp.format);
			spa_log_debug(this->log, "%p: set format on port %d:%d",
					this, direction, port_id);
		}
	}
	port->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	if (port->have_format) {
		port->params[3] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_READWRITE);
		port->params[4] = SPA_PARAM_INFO(SPA_PARAM_Buffers, SPA_PARAM_INFO_READ);
	} else {
		port->params[3] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
		port->params[4] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
	}
	mixer_port_emit_info(this, port, false);

	return 0;
}

int
mixer_port_set_param(void *object,
			 enum spa_direction direction, uint32_t port_id,
			 uint32_t id, uint32_t flags,
			 const struct spa_pod *param)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	if (id == SPA_PARAM_Format)
		return mixer_port_set_format(this, direction, port_id, flags, param);

	/* No SPA_PARAM_Props on a port: the mixer has one level, and it is
	 * set on the node. A per-port write here would be silently ignored,
	 * which is worse than a refusal because it looks like it worked. */
	return -ENOENT;
}

int
mixer_port_use_buffers(void *object,
			   enum spa_direction direction,
			   uint32_t port_id,
			   uint32_t flags,
			   struct spa_buffer **buffers,
			   uint32_t n_buffers)
{
	struct impl *this = object;
	struct port *port;
	uint32_t i, j;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_log_debug(this->log, "%p: use %d buffers on port %d:%d",
			this, n_buffers, direction, port_id);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	spa_return_val_if_fail(!this->started ||
			(port->io[0] == NULL && port->io[1] == NULL), -EIO);

	if (n_buffers > 0 && !port->have_format) {
		res = -EIO;
		goto error;
	}
	if (n_buffers > MAX_BUFFERS) {
		res = -ENOSPC;
		goto error;
	}

	mixer_clear_buffers(this, port);

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b;
		uint32_t n_datas = buffers[i]->n_datas;
		struct spa_data *d = buffers[i]->datas;

		if (n_datas > MAX_DATAS) {
			res = -ENOSPC;
			goto error;
		}

		b = &port->buffers[i];
		b->buffer = buffers[i];
		b->flags = 0;
		b->id = i;
		b->h = spa_buffer_find_meta_data(buffers[i], SPA_META_Header, sizeof(*b->h));
		b->buf = *buffers[i];

		for (j = 0; j < n_datas; j++) {
			void *data = d[j].data;
			if (data == NULL && SPA_FLAG_IS_SET(d[j].flags, SPA_DATA_FLAG_MAPPABLE)) {
				int prot = 0;
				if (SPA_FLAG_IS_SET(d[j].flags, SPA_DATA_FLAG_READABLE))
					prot |= PROT_READ;
				if (SPA_FLAG_IS_SET(d[j].flags, SPA_DATA_FLAG_WRITABLE))
					prot |= PROT_WRITE;
				data = mmap(NULL, d[j].maxsize,
					prot, MAP_SHARED, d[j].fd, d[j].mapoffset);
				if (data == MAP_FAILED) {
					spa_log_error(this->log, "%p: mmap failed %d on buffer %d %d %p: %m",
							this, j, i, d[j].type, data);
					res = -EINVAL;
					goto error;
				}
				SPA_FLAG_SET(b->flags, BUFFER_FLAG_MAPPED);
				spa_log_debug(this->log, "%p: mmap %d on buffer %d %d %p %p",
							this, j, i, d[j].type, data, b);
			}
			if (data == NULL) {
				spa_log_error(this->log, "%p: invalid memory %d on buffer %d %d %p",
						this, j, i, d[j].type, data);
				res = -EINVAL;
				goto error;
			} else if (!SPA_IS_ALIGNED(data, this->max_align)) {
				spa_log_warn(this->log, "%p: memory %d on buffer %d not aligned",
						this, j, i);
			}

			d[j].data = b->datas[j] = data;
		}
		if (direction == SPA_DIRECTION_OUTPUT)
			mixer_queue_buffer(this, port, b);

		port->n_buffers++;
		spa_log_debug(this->log, "%p: port %d:%d buffer:%d n_data:%d data:%p maxsize:%d",
				this, direction, port_id, i,
				buffers[i]->n_datas, d[0].data, d[0].maxsize);
	}

	return 0;
error:
	mixer_clear_buffers(this, port);
	return res;
}

struct io_info {
	struct impl *impl;
	struct port *port;
	void *data;
	size_t size;
};

static int do_port_set_io(struct spa_loop *loop, bool async, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	struct io_info *info = user_data;
	struct port *port = info->port;

	if (info->data == NULL || info->size < sizeof(struct spa_io_buffers)) {
		port->io[0] = NULL;
		port->io[1] = NULL;
		if (port->active) {
			/* Unwiring the io vector takes the port out of the
			 * mix: there is nothing to read a buffer from any
			 * more, so leaving it listed would have process()
			 * follow a dangling io pointer on the next cycle.
			 *
			 * A stream that stops is unlinked rather than seen
			 * delivering nothing, which is why this is the
			 * normal path out of the mix and not an error one. */
			spa_list_remove(&port->mix_link);
			port->active = false;
		}
	} else {
		if (info->size >= sizeof(struct spa_io_async_buffers)) {
			struct spa_io_async_buffers *ab = info->data;
			port->io[0] = &ab->buffers[port->direction];
			port->io[1] = &ab->buffers[port->direction^1];
		} else {
			port->io[0] = info->data;
			port->io[1] = info->data;
		}
		if (port->direction == SPA_DIRECTION_INPUT && !port->active) {
			spa_list_append(&info->impl->mix_list, &port->mix_link);
			port->active = true;
		}
	}
	return 0;
}

int
mixer_port_set_io(void *object,
		      enum spa_direction direction, uint32_t port_id,
		      uint32_t id, void *data, size_t size)
{
	struct impl *this = object;
	struct port *port;
	struct io_info info;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_log_debug(this->log, "%p: port %d:%d io %d %p/%zd", this,
			direction, port_id, id, data, size);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);
	info.impl = this;
	info.port = port;
	info.data = data;
	info.size = size;

	switch (id) {
	case SPA_IO_Buffers:
	case SPA_IO_AsyncBuffers:
		spa_loop_locked(this->data_loop,
                               do_port_set_io, SPA_ID_INVALID, NULL, 0, &info);
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

int mixer_port_reuse_buffer(void *object, uint32_t port_id, uint32_t buffer_id)
{
	struct impl *this = object;
	struct port *port;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(CHECK_PORT(this, SPA_DIRECTION_OUTPUT, port_id), -EINVAL);
	port = GET_OUT_PORT(this, port_id);

	if (buffer_id >= port->n_buffers)
		return -EINVAL;

	return mixer_queue_buffer(this, port, &port->buffers[buffer_id]);
}
