/* Volume map unit test.
 *
 * The map is where the one float comparison in the volume stack lives, so
 * its output deserves the closest looking-at: if the step mapping is
 * wrong, every downstream integer decision is confidently wrong.
 */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "maemo-vol-map.h"

static int failures;

#define CHECK(cond, fmt, ...) do { \
	if (!(cond)) { printf("  FAIL " fmt "\n", ##__VA_ARGS__); failures++; } \
	else printf("  ok   " fmt "\n", ##__VA_ARGS__); } while (0)

#define NEAR(a, b) (fabs((a) - (b)) < 1e-6)

/* 11 slider steps, 0.00 .. 1.00 */
static const float SLIDER[] = {
	0.0f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f
};
#define N_SLIDER (sizeof(SLIDER)/sizeof(SLIDER[0]))

/* A deliberately coarse codec ladder */
static const float LADDER[] = { 0.1f, 0.3f, 0.5f, 0.7f, 1.0f };
#define N_LADDER (sizeof(LADDER)/sizeof(LADDER[0]))

int main(void)
{
	struct maemo_vol_map *m;

	printf("build\n");
	m = maemo_vol_map_new(SLIDER, N_SLIDER, LADDER, N_LADDER);
	CHECK(m != NULL, "map built");
	if (!m) goto done;
	CHECK(m->n_slider == N_SLIDER && m->n_ladder == N_LADDER, "sizes kept");

	printf("\nthe mapping is monotonic non-decreasing\n");
	/* This is what makes "going up" a meaningful integer comparison
	 * downstream: a higher slider step must never want a lower
	 * hardware step. */
	int prev = -1;
	bool monotonic = true;
	for (unsigned i = 0; i < N_SLIDER; i++) {
		int hw = maemo_vol_map_hw_step(m, i);
		if (hw < prev) monotonic = false;
		prev = hw;
	}
	CHECK(monotonic, "slider step up never means hardware step down");

	printf("\nnearest ladder step, ties going up\n");
	struct { unsigned step; int expect; const char *why; } cases[] = {
		{ 1, 0, "0.1 is exactly a ladder step" },
		{ 2, 1, "0.2 ties 0.1/0.3 -> up" },
		{ 3, 1, "0.3 is exactly a ladder step" },
		{ 4, 2, "0.4 ties 0.3/0.5 -> up" },
		{ 5, 2, "0.5 is exactly a ladder step" },
		{ 6, 3, "0.6 ties 0.5/0.7 -> up" },
		{ 7, 3, "0.7 is exactly a ladder step" },
		{ 8, 3, "0.8 is nearer 0.7 than 1.0" },
		{ 9, 4, "0.9 is nearer 1.0 than 0.7" },
		{ 10, 4, "1.0 is exactly a ladder step" },
	};
	for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
		int hw = maemo_vol_map_hw_step(m, cases[i].step);
		CHECK(hw == cases[i].expect,
				"slider %.1f -> hw step %d (%s), got %d",
				SLIDER[cases[i].step], cases[i].expect,
				cases[i].why, hw);
	}

	printf("\namplitude lookups\n");
	CHECK(NEAR(maemo_vol_map_virtual(m, 5), 0.5f), "virtual(5) = 0.5");
	CHECK(NEAR(maemo_vol_map_hw(m, 2), 0.5f), "hw(2) = 0.5");
	CHECK(NEAR(maemo_vol_map_hw(m, 4), 1.0f), "hw(4) = 1.0");

	printf("\nadopting a level something else set\n");
	CHECK(maemo_vol_map_step_for_amp(m, 0.5f) == 5, "0.5 -> step 5");
	CHECK(maemo_vol_map_step_for_amp(m, 0.51f) == 5, "0.51 -> step 5");
	CHECK(maemo_vol_map_step_for_amp(m, 0.95f) == 10, "0.95 -> step 10");
	CHECK(maemo_vol_map_step_for_amp(m, 0.0f) == 0, "0.0 -> step 0");

	printf("\nout of range is refused, not silently zero\n");
	CHECK(maemo_vol_map_hw_step(m, 999) == -1, "slider step past the end");
	CHECK(maemo_vol_map_hw(m, 99) == 0.0f, "hw step past the end");
	CHECK(maemo_vol_map_hw(m, -1) == 0.0f, "negative hw step");
	maemo_vol_map_free(m);

	printf("\nbad inputs give NULL\n");
	CHECK(maemo_vol_map_new(NULL, 3, LADDER, N_LADDER) == NULL, "no slider");
	CHECK(maemo_vol_map_new(SLIDER, N_SLIDER, NULL, 3) == NULL, "no ladder");
	CHECK(maemo_vol_map_new(SLIDER, 0, LADDER, N_LADDER) == NULL, "empty slider");
	CHECK(maemo_vol_map_hw_step(NULL, 1) == -1, "NULL map lookup");
	CHECK(maemo_vol_map_step_for_amp(NULL, 0.5f) == -1, "NULL adopt");

	printf("\nthe caller's arrays are copied, not borrowed\n");
	float slider_copy[3] = { 0.2f, 0.6f, 1.0f };
	float ladder_copy[2] = { 0.2f, 1.0f };
	m = maemo_vol_map_new(slider_copy, 3, ladder_copy, 2);
	slider_copy[0] = 99.0f;
	ladder_copy[1] = -5.0f;
	CHECK(NEAR(maemo_vol_map_virtual(m, 0), 0.2f), "slider copied (%f)",
			maemo_vol_map_virtual(m, 0));
	CHECK(NEAR(maemo_vol_map_hw(m, 1), 1.0f), "ladder copied (%f)",
			maemo_vol_map_hw(m, 1));
	maemo_vol_map_free(m);

	printf("\na one-step ladder maps everything onto it\n");
	float one[] = { 0.5f };
	m = maemo_vol_map_new(SLIDER, N_SLIDER, one, 1);
	bool all_zero = true;
	for (unsigned i = 0; i < N_SLIDER; i++)
		if (maemo_vol_map_hw_step(m, i) != 0) all_zero = false;
	CHECK(all_zero, "every slider step maps to the only hardware step");
	maemo_vol_map_free(m);

	printf("\nfloat32 ties really are the trap this handles\n");
	/* Worth spelling out why the tolerance exists: in float32 these
	 * two distances are not equal, and a naive comparison picks the
	 * wrong side of what is a tie in every sense that matters. */
	float lo = 0.1f, mid = 0.2f, hi = 0.3f;
	double d_lo = fabs((double) mid - (double) lo);
	double d_hi = fabs((double) hi - (double) mid);
	printf("       0.2f-0.1f = %.12f\n", d_lo);
	printf("       0.3f-0.2f = %.12f\n", d_hi);
	printf("       they differ by %.3g -- not equal in float32\n",
			fabs(d_lo - d_hi));
	CHECK(d_lo != d_hi, "confirmed: the tie is not exact in binary");
	/* And the map still resolves it upward, which is the whole point. */
	CHECK(maemo_vol_map_hw_step(maemo_vol_map_new(SLIDER, N_SLIDER,
			LADDER, N_LADDER), 2) == 1,
			"map resolves the float32 tie upward anyway");

	printf("\ncentibel conversion\n");
	CHECK(NEAR(maemo_vol_cb_to_amp(0.0), 1.0),
			"0 cB is unity (%f)", maemo_vol_cb_to_amp(0.0));
	CHECK(NEAR(maemo_vol_cb_to_amp(-6000.0), 0.001),
			"-60 dB is a thousandth (%f)", maemo_vol_cb_to_amp(-6000.0));
	CHECK(NEAR(maemo_vol_cb_to_amp(-900.0), 0.354813),
			"-900 cB (-9 dB) is 10^-0.45 = %f", maemo_vol_cb_to_amp(-900.0));

	printf("\nmatches the applet's own formula on the real droid4 ladder\n");
	/* x-maemo.volume.tuning for the droid4 internal speaker, taken
	 * verbatim from 50-maemo-volume-droid4.conf. ctrl 0..12, -33 dB
	 * to +3 dB, with the +3 dB of boost left off so the top entry is
	 * -900 cB rather than 0. */
	static const double DROID4_SPEAKER[] = {
		-4500, -4200, -3900, -3600, -3300, -3000, -2700,
		-2400, -2100, -1800, -1500, -1200, -900
	};
	const unsigned n_d4 = sizeof(DROID4_SPEAKER) / sizeof(DROID4_SPEAKER[0]);

	/* The applet's tuning_cb_to_volume(), copied exactly including
	 * the lrint() -- the applet notes that truncating disagrees with
	 * PulseAudio on about half of all values, which would shift every
	 * ladder by a step. If this loop ever fails, the two have drifted
	 * apart and the slider will disagree with the codec.
	 *
	 * The two sides live on different scales and this is where they
	 * meet: the applet holds a perceptual step, we hold amplitude, and
	 * step = 65536 * cbrt(amp). Applying the cbrt is what makes the
	 * comparison meaningful; comparing the raw amplitude against the
	 * step would be comparing different quantities. */
	int worst = 0;
	for (unsigned i = 0; i < n_d4; i++) {
		double db = DROID4_SPEAKER[i] / 100.0;
		long applet = lrint(65536.0 * pow(10.0, db / 60.0));
		long ours = lrint(65536.0 * cbrt((double) maemo_vol_cb_to_amp(DROID4_SPEAKER[i])));
		int delta = (int) labs(applet - ours);
		if (delta > worst)
			worst = delta;
	}
	CHECK(worst == 0,
			"every droid4 step agrees with the applet to the integer (worst delta %d)",
			worst);

	/* The config states the slider maximum is 46396, not
	 * PA_VOLUME_NORM, because the top entry is -900 cB. Assert that
	 * here so a change to the table cannot silently move the ceiling. */
	CHECK(lrint(65536.0 * cbrt((double) maemo_vol_cb_to_amp(-900.0))) == 46396,
			"top of the droid4 ladder is 46396 as the config says (%ld)",
			lrint(65536.0 * cbrt((double) maemo_vol_cb_to_amp(-900.0))));

	printf("\nbuilding a map from cB tables\n");
	static const double SLIDER_CB[] = {
		-6000, -5000, -4000, -3000, -2000, -1000, 0
	};
	struct maemo_vol_map *mc = maemo_vol_map_new_from_cb(
			SLIDER_CB, 7, DROID4_SPEAKER, n_d4);
	CHECK(mc != NULL, "map built from cB tables");
	if (mc) {
		CHECK(NEAR(maemo_vol_map_virtual(mc, 6), 1.0),
				"top slider step (0 cB) is unity (%f)",
				maemo_vol_map_virtual(mc, 6));
		CHECK(NEAR(maemo_vol_map_hw(mc, 12), 0.354813),
				"top ladder step (-900 cB = -9 dB) is 0.3548 (%f)",
				maemo_vol_map_hw(mc, 12));
		/* The whole point of the conversion: the mapping is still
		 * integers, and the top slider step cannot be carried by
		 * the ladder, so it lands on the ceiling. */
		CHECK(maemo_vol_map_hw_step(mc, 6) == 12,
				"0 cB slider maps to the ladder ceiling, step 12 (%d)",
				maemo_vol_map_hw_step(mc, 6));
		maemo_vol_map_free(mc);
	}

	printf("\na non-ascending ladder is rejected, not silently misused\n");
	static const double BAD[] = { -1000, -2000, -3000 };
	CHECK(maemo_vol_map_new_from_cb(SLIDER_CB, 7, BAD, 3) == NULL,
			"descending ladder fails the build rather than picking a random step");

	done:
		printf("\n%s\n", failures ? "FAILURES" : "ALL PASS");
		return failures ? 1 : 0;
}
