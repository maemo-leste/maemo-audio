/* Maemo Flat Volume Mixer: SSE volume kernel.
 *
 * SPDX-License-Identifier: MIT
 *
 * 4 floats per register. The volume is broadcast once per source, so a
 * stream sitting at full gain costs a multiply by one rather than a
 * separate pass.
 *
 * Unaligned loads throughout: the source pointer is a byte offset into a
 * buffer and the chunk offset is not guaranteed to keep 16-byte
 * alignment, so aligned loads would fault on a valid input. On modern
 * x86 they cost nothing.
 *
 * The sample block is the outer loop and the sources are accumulated in
 * registers, so dst is written once per block. Walking sources in the
 * outer loop instead makes every source load and store dst again, and
 * that output traffic -- not the arithmetic -- is what the kernel spends
 * its time on.
 */
#include <spa/utils/defs.h>

#include <xmmintrin.h>

#include "mix-volume.h"

void
mix_volume_f32_sse(void * SPA_RESTRICT dst_,
		const void * SPA_RESTRICT src[], const float *vols,
		uint32_t n_src, uint32_t n_samples)
{
	float *SPA_RESTRICT dst = dst_;
	const __m128 zero = _mm_setzero_ps();
	uint32_t i, j;
	uint32_t n_vec8 = n_samples & ~7u;
	uint32_t n_vec = n_samples & ~3u;

	if (n_src == 1) {
		const float *SPA_RESTRICT s = src[0];

		if (SPA_LIKELY(vols[0] == 1.0f)) {
			for (i = 0; i < n_vec; i += 4)
				_mm_storeu_ps(&dst[i], _mm_loadu_ps(&s[i]));
		} else {
			const __m128 v = _mm_set1_ps(vols[0]);

			for (i = 0; i < n_vec; i += 4)
				_mm_storeu_ps(&dst[i], _mm_mul_ps(_mm_loadu_ps(&s[i]), v));
		}
		for (; i < n_samples; i++)
			dst[i] = s[i] * vols[0];
		return;
	}

	for (i = 0; i < n_vec8; i += 8) {
		__m128 a0 = zero, a1 = zero;

		for (j = 0; j < n_src; j++) {
			const float *s = src[j];

			/* Flat volume means most streams arrive at unity, and
			 * without a fused multiply-add here that is worth a
			 * branch: the sum is one instruction instead of two.
			 * The compare is per source per block and perfectly
			 * predicted. */
			if (SPA_LIKELY(vols[j] == 1.0f)) {
				a0 = _mm_add_ps(a0, _mm_loadu_ps(&s[i]));
				a1 = _mm_add_ps(a1, _mm_loadu_ps(&s[i + 4]));
			} else {
				const __m128 v = _mm_set1_ps(vols[j]);

				a0 = _mm_add_ps(a0, _mm_mul_ps(_mm_loadu_ps(&s[i]), v));
				a1 = _mm_add_ps(a1, _mm_mul_ps(_mm_loadu_ps(&s[i + 4]), v));
			}
		}

		_mm_storeu_ps(&dst[i], a0);
		_mm_storeu_ps(&dst[i + 4], a1);
	}

	for (; i < n_vec; i += 4) {
		__m128 a0 = zero;

		for (j = 0; j < n_src; j++)
			a0 = _mm_add_ps(a0,
					_mm_mul_ps(_mm_loadu_ps(((const float *) src[j]) + i),
							_mm_set1_ps(vols[j])));

		_mm_storeu_ps(&dst[i], a0);
	}

	/* tail */
	for (; i < n_samples; i++) {
		float acc = 0.0f;
		for (j = 0; j < n_src; j++)
			acc += ((const float *) src[j])[i] * vols[j];
		dst[i] = acc;
	}
}
