/* Maemo Flat Volume Mixer: portable C volume kernel.
 *
 * SPDX-License-Identifier: MIT
 *
 * Reference implementation and the fallback when no SIMD path applies.
 * Written so the compiler can auto-vectorize the inner loops: the unity
 * case is a bare accumulate and the scaled case a fused multiply-add,
 * with no aliasing between dst and src.
 */
#include <spa/utils/defs.h>

#include "mix-volume.h"

void
mix_volume_f32_c(void * SPA_RESTRICT dst_,
		const void * SPA_RESTRICT src[], const float *vols,
		uint32_t n_src, uint32_t n_samples)
{
	float *SPA_RESTRICT dst = dst_;
	uint32_t i, j;

	for (i = 0; i < n_samples; i++)
		dst[i] = 0.0f;

	for (j = 0; j < n_src; j++) {
		const float *SPA_RESTRICT s = src[j];
		float v = vols[j];

		if (SPA_LIKELY(v == 1.0f)) {
			for (i = 0; i < n_samples; i++)
				dst[i] += s[i];
		} else {
			for (i = 0; i < n_samples; i++)
				dst[i] += s[i] * v;
		}
	}
}
