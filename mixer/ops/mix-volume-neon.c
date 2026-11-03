/* Maemo Flat Volume Mixer: ARM NEON volume kernel.
 *
 * SPDX-License-Identifier: MIT
 *
 * 4 floats per register. Compiled only when HAVE_NEON is set, which
 * happens on the ARM build; the dispatch table simply omits this entry
 * elsewhere, so the x86 build carries no dead code.
 *
 * vmlaq/vfmaq give a single fused multiply-accumulate per lane, which is
 * the whole point of doing this in NEON rather than leaning on
 * auto-vectorisation of the C loop.
 *
 * The sample block is the outer loop and the sources are accumulated in
 * registers, so dst is written once per block. Walking sources in the
 * outer loop instead makes every source load and store dst again, and
 * that output traffic -- not the arithmetic -- is what the kernel spends
 * its time on. Two blocks are kept live to give the FMA pipeline a
 * independent chain to fill.
 */
#include <spa/utils/defs.h>

#include <arm_neon.h>

#include "mix-volume.h"

#if defined(__ARM_FEATURE_FMA)
#define MACL(acc, x, v)	vfmaq_f32((acc), (x), (v))
#else
#define MACL(acc, x, v)	vmlaq_f32((acc), (x), (v))
#endif

void
mix_volume_f32_neon(void * SPA_RESTRICT dst_,
		const void * SPA_RESTRICT src[], const float *vols,
		uint32_t n_src, uint32_t n_samples)
{
	float *SPA_RESTRICT dst = dst_;
	const float32x4_t zero = vdupq_n_f32(0.0f);
	uint32_t i, j;
	uint32_t n_vec8 = n_samples & ~7u;
	uint32_t n_vec = n_samples & ~3u;

	if (n_src == 1) {
		const float *SPA_RESTRICT s = src[0];
		const float32x4_t v = vdupq_n_f32(vols[0]);
		const int unity = vols[0] == 1.0f;

		for (i = 0; i < n_vec; i += 4) {
			const float32x4_t x = vld1q_f32(&s[i]);
			vst1q_f32(&dst[i], unity ? x : vmulq_f32(x, v));
		}
		for (; i < n_samples; i++)
			dst[i] = s[i] * vols[0];
		return;
	}

	for (i = 0; i < n_vec8; i += 8) {
		float32x4_t a0 = zero, a1 = zero;

		for (j = 0; j < n_src; j++) {
			const float *s = src[j];
			const float32x4_t v = vdupq_n_f32(vols[j]);

			a0 = MACL(a0, vld1q_f32(&s[i]), v);
			a1 = MACL(a1, vld1q_f32(&s[i + 4]), v);
		}

		vst1q_f32(&dst[i], a0);
		vst1q_f32(&dst[i + 4], a1);
	}

	for (; i < n_vec; i += 4) {
		float32x4_t a0 = zero;

		for (j = 0; j < n_src; j++)
			a0 = MACL(a0, vld1q_f32(((const float *) src[j]) + i),
					vdupq_n_f32(vols[j]));

		vst1q_f32(&dst[i], a0);
	}

	/* tail */
	for (; i < n_samples; i++) {
		float acc = 0.0f;
		for (j = 0; j < n_src; j++)
			acc += ((const float *) src[j])[i] * vols[j];
		dst[i] = acc;
	}
}
