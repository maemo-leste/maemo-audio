/* Maemo Flat Volume Mixer: kernel selection.
 *
 * SPDX-License-Identifier: MIT
 *
 * Dispatch is keyed on CPU alone -- the DSP
 * format axis is already pinned to F32 by port_set_format().
 *
 * Ordered widest-first; the first entry whose required flags the CPU has
 * wins, and the final { 0, ... } entry is the unconditional fallback.
 */
#include <spa/support/cpu.h>
#include <spa/utils/defs.h>

#include "mix-volume.h"

struct mix_volume_info {
	uint32_t cpu_flags;
	mix_volume_func_t process;
};

static const struct mix_volume_info vol_table[] =
{
#if defined(HAVE_AVX2)
	{ SPA_CPU_FLAG_AVX2 | SPA_CPU_FLAG_FMA3,	mix_volume_f32_avx2 },
#endif
#if defined(HAVE_SSE)
	{ SPA_CPU_FLAG_SSE,				mix_volume_f32_sse },
#endif
#if defined(HAVE_NEON)
	{ SPA_CPU_FLAG_NEON,				mix_volume_f32_neon },
#endif
	{ 0,						mix_volume_f32_c },
};

int mix_volume_init(struct mix_volume_ops *ops, uint32_t cpu_flags)
{
	uint32_t i;

	spa_return_val_if_fail(ops != NULL, -EINVAL);

	for (i = 0; i < sizeof(vol_table) / sizeof(vol_table[0]); i++) {
		const struct mix_volume_info *e = &vol_table[i];
		if (e->cpu_flags == 0 ||
		    (cpu_flags & e->cpu_flags) == e->cpu_flags) {
			ops->cpu_flags = e->cpu_flags;
			ops->process = e->process;
			return 0;
		}
	}

	/* unreachable: the table ends with a zero-flag entry */
	ops->cpu_flags = 0;
	ops->process = mix_volume_f32_c;
	return 0;
}

const char *mix_volume_kernel_name(const struct mix_volume_ops *ops)
{
	if (ops == NULL || ops->process == NULL)
		return "none";
	if (ops->process == mix_volume_f32_c)
		return "c";
#if defined(HAVE_SSE)
	if (ops->process == mix_volume_f32_sse)
		return "sse";
#endif
#if defined(HAVE_AVX2)
	if (ops->process == mix_volume_f32_avx2)
		return "avx2";
#endif
#if defined(HAVE_NEON)
	if (ops->process == mix_volume_f32_neon)
		return "neon";
#endif
	return "unknown";
}
