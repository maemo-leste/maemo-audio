/* Maemo Flat Volume Mixer: volume-aware accumulate kernels.
 *
 * SPDX-License-Identifier: MIT
 *
 * The kernel inherited from the stock audiomixer sums sources with a
 * single uniform gain -- there
 * is nowhere to put a per-stream volume, which is the whole reason this
 * mixer exists.  These kernels take one volume per source.
 *
 * The DSP format is pinned at SPA_AUDIO_FORMAT_DSP_F32 with stride
 * sizeof(float) and one channel per port, so there is no
 * format axis here.  Only the CPU axis remains.
 *
 * Three paths per source, selected per source rather than per sample:
 *
 *   unity   -- add without multiplying
 *   scaled  -- multiply while adding
 *
 * Muted and zero-gain sources are dropped by the caller and never reach a
 * kernel at all, so they cost nothing.
 */
#ifndef SPA_MIX_VOLUME_H
#define SPA_MIX_VOLUME_H

#include <spa/utils/defs.h>

typedef void (*mix_volume_func_t) (void * SPA_RESTRICT dst,
		const void * SPA_RESTRICT src[], const float *vols,
		uint32_t n_src, uint32_t n_samples);

struct mix_volume_ops {
	uint32_t cpu_flags;
	mix_volume_func_t process;
};

/* Portable C. Also the tail handler for the SIMD kernels. */
void mix_volume_f32_c(void * SPA_RESTRICT dst,
		const void * SPA_RESTRICT src[], const float *vols,
		uint32_t n_src, uint32_t n_samples);

#if defined(HAVE_SSE)
void mix_volume_f32_sse(void * SPA_RESTRICT dst,
		const void * SPA_RESTRICT src[], const float *vols,
		uint32_t n_src, uint32_t n_samples);
#endif

#if defined(HAVE_AVX2)
void mix_volume_f32_avx2(void * SPA_RESTRICT dst,
		const void * SPA_RESTRICT src[], const float *vols,
		uint32_t n_src, uint32_t n_samples);
#endif

#if defined(HAVE_NEON)
void mix_volume_f32_neon(void * SPA_RESTRICT dst,
		const void * SPA_RESTRICT src[], const float *vols,
		uint32_t n_src, uint32_t n_samples);
#endif

/* Picks the widest kernel the given CPU flags allow. */
int mix_volume_init(struct mix_volume_ops *ops, uint32_t cpu_flags);

/* The selected kernel by name, for logging. Worth having because "which
 * kernel did the runtime detection land on" is the first question when
 * a device behaves differently from the build host, and it should be
 * answerable from the log rather than with a debugger. */
const char *mix_volume_kernel_name(const struct mix_volume_ops *ops);

#endif /* SPA_MIX_VOLUME_H */
