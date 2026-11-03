/* Volume kernel equivalence test.
 *
 * SPDX-License-Identifier: MIT
 *
 * Every SIMD kernel must agree with the portable C reference. Runs the
 * same inputs through each compiled kernel and compares.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <spa/support/cpu.h>
#include "mix-volume.h"

#define N_SRC		8
#define MAX_SAMPLES	4096

static float src_mem[N_SRC][MAX_SAMPLES];
static const void *src[N_SRC];
static float vols[N_SRC];

static float out_ref[MAX_SAMPLES];
static float out_test[MAX_SAMPLES];

static uint32_t seed = 12345u;
static float frand(void)
{
	seed = seed * 1103515245u + 12345u;
	return (float)((seed >> 8) & 0xFFFFFF) / (float)0x800000 * 2.0f - 1.0f;
}

/* Relative tolerance. The FMA paths fuse multiply-add into a single
 * rounding where the C reference rounds twice, so the kernels are not
 * bit-identical by construction -- FMA is the more accurate of the
 * two. Scale the bound with the magnitude of the sum, which grows
 * with the number of sources. A real bug is orders of magnitude
 * outside this. */
static int compare(const char *name, uint32_t n, float tol)
{
	uint32_t i;
	float worst = 0.0f;
	for (i = 0; i < n; i++) {
		float scale = SPA_MAX(1.0f, fabsf(out_ref[i]));
		float d = fabsf(out_test[i] - out_ref[i]) / scale;
		if (d > worst) worst = d;
	}
	if (worst > tol) {
		printf("  FAIL %-10s n=%-5u worst rel %.9g\n", name, n, worst);
		return 1;
	}
	printf("  ok   %-10s n=%-5u worst rel %.3g\n", name, n, worst);
	return 0;
}

static int run(const char *name, mix_volume_func_t fn, float tol)
{
	uint32_t lens[] = { 1, 3, 4, 7, 8, 15, 16, 17, 31, 32, 128, 1000, 1024 };
	uint32_t k, fails = 0;

	if (fn == NULL) {
		printf("  --   %-10s not compiled in\n", name);
		return 0;
	}

	for (k = 0; k < sizeof(lens) / sizeof(lens[0]); k++) {
		uint32_t n = lens[k];

		/* fresh inputs each length so nothing is cached */
		uint32_t j;
		for (j = 0; j < N_SRC; j++) {
			uint32_t i;
			for (i = 0; i < n; i++)
				src_mem[j][i] = frand();
			src[j] = src_mem[j];
			/* mix of unity, scaled and near-silent gains */
			vols[j] = (j % 4 == 0) ? 1.0f :
				(j % 4 == 1) ? frand() :
				(j % 4 == 2) ? 0.25f : 0.0f;
		}

		mix_volume_f32_c(out_ref, src, vols, N_SRC, n);
		memset(out_test, 0xAB, sizeof(out_test));
		fn(out_test, src, vols, N_SRC, n);

		if (compare(name, n, tol))
			fails++;
	}
	return fails ? 1 : 0;
}

int main(void)
{
	int fails = 0;

	printf("volume kernel equivalence vs C reference\n");

	fails += run("c", mix_volume_f32_c, 0.0f);

#if defined(HAVE_SSE)
	fails += run("sse", mix_volume_f32_sse, 1e-5f);
#endif
#if defined(HAVE_AVX2)
	fails += run("avx2", mix_volume_f32_avx2, 1e-5f);
#endif
#if defined(HAVE_NEON)
	fails += run("neon", mix_volume_f32_neon, 1e-5f);
#endif

	/* Dispatch must pick the widest kernel the reported CPU flags allow,
	 * and must never pick one the CPU does not have. The compile-time
	 * HAVE_* flags decide which kernels exist in the binary; these
	 * probes check the runtime choice between them, which is the part
	 * that decides whether an NEON-less ARM core ever gets handed
	 * NEON instructions. */
	struct mix_volume_ops ops;
	printf("\ndispatch\n");

	/* No flags at all: the unconditional C fallback. */
	if (mix_volume_init(&ops, 0) == 0 && ops.process == mix_volume_f32_c)
		printf("  ok   flags=0x0 -> c\n");
	else { printf("  FAIL flags=0x0 should fall back to c\n"); fails++; }

	/* A CPU with no SIMD at all must still land on C, even though
	 * wider kernels were compiled in. */
	if (mix_volume_init(&ops, SPA_CPU_FLAG_FMA3) == 0 &&
	    ops.process == mix_volume_f32_c)
		printf("  ok   flags with no matching kernel -> c\n");
	else { printf("  FAIL unmatched flags should fall back to c\n"); fails++; }

#if defined(HAVE_NEON)
	if (mix_volume_init(&ops, SPA_CPU_FLAG_NEON) == 0 &&
	    ops.process == mix_volume_f32_neon)
		printf("  ok   SPA_CPU_FLAG_NEON -> neon\n");
	else { printf("  FAIL SPA_CPU_FLAG_NEON should select neon\n"); fails++; }

	/* NEON plus unrelated x86 flags: the x86 entries are absent from
	 * this binary, so NEON must win rather than anything falling
	 * through. */
	if (mix_volume_init(&ops, SPA_CPU_FLAG_NEON | SPA_CPU_FLAG_SSE) == 0 &&
	    ops.process == mix_volume_f32_neon)
		printf("  ok   NEON selected despite unrelated flags\n");
	else { printf("  FAIL NEON should win over unrelated flags\n"); fails++; }
#endif

#if defined(HAVE_SSE)
	if (mix_volume_init(&ops, SPA_CPU_FLAG_SSE) == 0 &&
	    ops.process != mix_volume_f32_c)
		printf("  ok   SPA_CPU_FLAG_SSE -> a simd kernel\n");
	else { printf("  FAIL SPA_CPU_FLAG_SSE should select simd\n"); fails++; }
#endif

#if defined(HAVE_AVX2)
	if (mix_volume_init(&ops, SPA_CPU_FLAG_AVX2 | SPA_CPU_FLAG_FMA3) == 0 &&
	    ops.process == mix_volume_f32_avx2)
		printf("  ok   AVX2+FMA3 -> avx2\n");
	else { printf("  FAIL AVX2+FMA3 should select avx2\n"); fails++; }

	/* Partial support must not select the wider kernel: AVX2 without
	 * FMA3 is not enough. */
	if (mix_volume_init(&ops, SPA_CPU_FLAG_AVX2) == 0 &&
	    ops.process != mix_volume_f32_avx2)
		printf("  ok   AVX2 without FMA3 does not select avx2\n");
	else { printf("  FAIL AVX2 without FMA3 must not select avx2\n"); fails++; }
#endif

	/* Whatever is chosen, it must be callable and non-NULL. */
	if (mix_volume_init(&ops, 0xFFFFFFFFu) == 0 && ops.process != NULL)
		printf("  ok   all-flags probe yields a usable kernel\n");
	else { printf("  FAIL all-flags probe\n"); fails++; }

	printf("\n%s\n", fails ? "FAILED" : "ALL PASS");
	return fails ? 1 : 0;
}
