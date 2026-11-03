/* Maemo Flat Volume Mixer: AVX2 + FMA volume kernel.
 *
 * SPDX-License-Identifier: MIT
 *
 * 8 floats per register. Uses unaligned loads: the source pointer is
 * SPA_PTROFF(bd->data, offs, void) and the chunk offset is not guaranteed
 * to keep 32-byte alignment, so aligned loads would be a crash waiting to
 * happen. On modern x86 the penalty is negligible.
 *
 * The sample block is the outer loop and the sources are accumulated in
 * registers, so dst is written once per block. Walking sources in the
 * outer loop instead makes every source load and store dst again, and
 * that output traffic -- not the arithmetic -- is what the kernel spends
 * its time on. A unity gain needs no special case here: fmadd is one
 * instruction whichever operand is one.
 */
#include <spa/utils/defs.h>

#include <immintrin.h>

#include "mix-volume.h"

void
mix_volume_f32_avx2(void * SPA_RESTRICT dst_,
		const void * SPA_RESTRICT src[], const float *vols,
		uint32_t n_src, uint32_t n_samples)
{
	float *SPA_RESTRICT dst = dst_;
	const __m256 zero = _mm256_setzero_ps();
	uint32_t i, j;
	uint32_t n_vec16 = n_samples & ~15u;
	uint32_t n_vec = n_samples & ~7u;

	if (n_src == 1) {
		const float *SPA_RESTRICT s = src[0];
		const __m256 v = _mm256_set1_ps(vols[0]);

		for (i = 0; i < n_vec; i += 8)
			_mm256_storeu_ps(&dst[i],
					_mm256_mul_ps(_mm256_loadu_ps(&s[i]), v));
		for (; i < n_samples; i++)
			dst[i] = s[i] * vols[0];
		return;
	}

	for (i = 0; i < n_vec16; i += 16) {
		__m256 a0 = zero, a1 = zero;

		for (j = 0; j < n_src; j++) {
			const float *s = src[j];
			const __m256 v = _mm256_set1_ps(vols[j]);

			a0 = _mm256_fmadd_ps(_mm256_loadu_ps(&s[i]), v, a0);
			a1 = _mm256_fmadd_ps(_mm256_loadu_ps(&s[i + 8]), v, a1);
		}

		_mm256_storeu_ps(&dst[i], a0);
		_mm256_storeu_ps(&dst[i + 8], a1);
	}

	for (; i < n_vec; i += 8) {
		__m256 a0 = zero;

		for (j = 0; j < n_src; j++)
			a0 = _mm256_fmadd_ps(
					_mm256_loadu_ps(((const float *) src[j]) + i),
					_mm256_set1_ps(vols[j]), a0);

		_mm256_storeu_ps(&dst[i], a0);
	}

	/* tail */
	for (; i < n_samples; i++) {
		float acc = 0.0f;
		for (j = 0; j < n_src; j++)
			acc += ((const float *) src[j])[i] * vols[j];
		dst[i] = acc;
	}
}
