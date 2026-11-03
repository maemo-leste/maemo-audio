/* Ladder cache unit test.
 *
 * The assertion under test is that a ladder is parsed once and that the
 * thing handed back is the same map the caller would have built itself. The
 * first half checks the parser's tolerance rules, because those rules
 * are shared with the applet and must not drift.
 */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "maemo-ladder-cache.h"
#include "maemo-vol-map.h"

static int failures;

#define CHECK(cond, fmt, ...) do { \
	if (!(cond)) { printf("  FAIL " fmt "\n", ##__VA_ARGS__); failures++; } \
	else printf("  ok   " fmt "\n", ##__VA_ARGS__); } while (0)

#define NEAR(a, b) (fabs((double)(a) - (double)(b)) < 1e-6)

static void test_parser(void)
{
	unsigned n = 0;
	double *v;

	printf("parser: plain values\n");
	v = maemo_vol_parse_cb_csv("-4000,-2000,0", &n);
	CHECK(v != NULL && n == 3, "three values parsed (n=%u)", n);
	if (v) {
		CHECK(NEAR(v[0], -4000.0), "first is -4000 (got %g)", v[0]);
		CHECK(NEAR(v[1], -2000.0), "second is -2000 (got %g)", v[1]);
		CHECK(NEAR(v[2], 0.0), "third is 0 (got %g)", v[2]);
		free(v);
	}

	printf("parser: punctuation it must survive\n");
	/* Leading space and a trailing comma come from config files;
	 * quotes come from the spa_dict round trip. Losing the curve over
	 * any of them would be a silly way to lose your volume control. */
	v = maemo_vol_parse_cb_csv(" -4000, -2000 , 0, ", &n);
	CHECK(v != NULL && n == 3, "whitespace and trailing comma (n=%u)", n);
	if (v) free(v);

	v = maemo_vol_parse_cb_csv("\"-4000,-2000,0\"", &n);
	CHECK(v != NULL && n == 3, "wrapped in quotes (n=%u)", n);
	if (v) free(v);

	printf("parser: what it must refuse\n");
	CHECK(maemo_vol_parse_cb_csv("", &n) == NULL, "empty string yields nothing");
	CHECK(maemo_vol_parse_cb_csv(",,,", &n) == NULL, "no numbers yields nothing");
	CHECK(maemo_vol_parse_cb_csv(NULL, &n) == NULL, "NULL yields nothing");

	/* A non-number is skipped, not guessed at. Inventing a level is
	 * worse than dropping one. */
	v = maemo_vol_parse_cb_csv("-4000,not_a_number,0", &n);
	CHECK(v != NULL && n == 2, "non-numeric token skipped (n=%u)", n);
	if (v) {
		CHECK(NEAR(v[1], 0.0), "the survivor is still 0 (got %g)", v[1]);
		free(v);
	}
}

static void test_cache_identity(void)
{
	struct maemo_ladder_cache *c = maemo_ladder_cache_new();
	const char *curve = "-4000,-2000,0";
	const struct maemo_vol_map *a, *b;

	printf("cache: the same curve is built once\n");
	a = maemo_ladder_cache_get(c, curve);
	b = maemo_ladder_cache_get(c, curve);

	CHECK(a != NULL, "first request returns a map");
	CHECK(a == b, "second request returns the same map, not a copy");
	CHECK(maemo_ladder_cache_parses(c) == 1, "only one parse happened (got %u)",
	      maemo_ladder_cache_parses(c));
	CHECK(maemo_ladder_cache_hits(c) == 1, "second was a hit (got %u)",
	      maemo_ladder_cache_hits(c));
	CHECK(maemo_ladder_cache_entries(c) == 1, "one curve held (got %u)",
	      maemo_ladder_cache_entries(c));

	printf("cache: a different curve is a different map\n");
	const struct maemo_vol_map *d = maemo_ladder_cache_get(c, "-3000,-1000,0");
	CHECK(d != NULL && d != a, "distinct curve, distinct map");
	CHECK(maemo_ladder_cache_parses(c) == 2, "second curve cost a parse (got %u)",
	      maemo_ladder_cache_parses(c));
	CHECK(maemo_ladder_cache_entries(c) == 2, "two curves held (got %u)",
	      maemo_ladder_cache_entries(c));

	/* And the first one is still cached, not displaced. */
	CHECK(maemo_ladder_cache_get(c, curve) == a, "first curve still cached");
	CHECK(maemo_ladder_cache_parses(c) == 2, "still two parses (got %u)",
	      maemo_ladder_cache_parses(c));

	maemo_ladder_cache_free(c);
}

static void test_cache_content(void)
{
	struct maemo_ladder_cache *c = maemo_ladder_cache_new();
	/* Ascending cB, the shape a real speaker curve has. Checking the
	 * amplitudes proves the cache actually converted rather than just
	 * remembering a pointer. */
	const char *curve = "-2400,-1800,-1200,-900,-600";
	const struct maemo_vol_map *m = maemo_ladder_cache_get(c, curve);

	printf("cache: the map it hands back has the right numbers\n");
	CHECK(m != NULL, "map built");
	if (m != NULL) {
		/* The slider carries one position more than the hardware
		 * ladder: index 0 is silence. The ladder itself keeps its
		 * five hardware steps. */
		CHECK(m->n_slider == 6, "six slider steps (got %u)", m->n_slider);
		CHECK(m->n_ladder == 5, "five ladder steps (got %u)", m->n_ladder);

		/* The whole point of the prepended step: the left end of
		 * the slider can actually reach silence. Without it the
		 * bottom sits at the ladder's lowest hardware level,
		 * -2400 cB here (0.2512 linear, -12 dBFS), and music
		 * keeps playing no matter where the user puts the slider. */
		CHECK(maemo_vol_map_virtual(m, 0) == 0.0f,
		      "slider 0 is silence (got %.6f)",
		      maemo_vol_map_virtual(m, 0));

		/* -1200 cB (-12 dB) -> 10^(-1200/2000) = 10^-0.6 ~= 0.2512 */
		float mid = maemo_vol_map_hw(m, 2);
		CHECK(NEAR(mid, pow(10.0, -1200.0 / 2000.0)),
		      "step 2 is -1200 cB (got %.6f)", mid);

		float top = maemo_vol_map_hw(m, 4);
		CHECK(NEAR(top, pow(10.0, -600.0 / 2000.0)),
		      "top step is -600 cB (got %.6f)", top);

		/* slider == ladder shifted by the silence step: slider i
		 * rides on ladder i-1, and silence rides on the lowest
		 * hardware step. The bottom of the slider is therefore a
		 * rung of the ladder rather than a hole in it, and reaching
		 * silence does not depend on the codec having a mute --
		 * over Bluetooth there is no route to write at all. */
		CHECK(m->step_hw[0] == 0,
		      "silence rides the lowest hw step (got %d)", m->step_hw[0]);
		for (unsigned i = 1; i < m->n_slider; i++)
			CHECK(m->step_hw[i] == (int) i - 1,
			      "slider %u maps to ladder %u (got %d)", i, i - 1,
			      m->step_hw[i]);
	}
	maemo_ladder_cache_free(c);
}

static void test_cache_rejects_bad(void)
{
	struct maemo_ladder_cache *c = maemo_ladder_cache_new();

	printf("cache: a bad curve is refused and not remembered\n");
	/* Descending: not a ladder. */
	CHECK(maemo_ladder_cache_get(c, "0,-1000,-2000") == NULL,
	      "descending curve rejected");
	CHECK(maemo_ladder_cache_entries(c) == 0, "nothing cached from a bad curve");

	/* Asking again must also fail -- and must not have quietly cached
	 * something on the first try. */
	CHECK(maemo_ladder_cache_get(c, "0,-1000,-2000") == NULL,
	      "still refused on a second ask");
	CHECK(maemo_ladder_cache_entries(c) == 0, "still nothing cached");

	/* A good curve after a bad one still works: a failure must not
	 * poison the cache. */
	CHECK(maemo_ladder_cache_get(c, "-2000,-1000,0") != NULL,
	      "a good curve after a bad one still builds");
	CHECK(maemo_ladder_cache_entries(c) == 1, "one good curve held");

	maemo_ladder_cache_free(c);
}

static void test_cache_many(void)
{
	struct maemo_ladder_cache *c = maemo_ladder_cache_new();
	const struct maemo_vol_map *held[32];

	printf("cache: many curves, all distinct, all reused\n");
	for (unsigned i = 0; i < 32; i++) {
		char csv[64];
		snprintf(csv, sizeof(csv), "-%u,-%u,0", 4000 - i * 10, 2000 - i * 10);
		held[i] = maemo_ladder_cache_get(c, csv);
		if (held[i] == NULL) {
			CHECK(false, "curve %u failed to build", i);
			break;
		}
	}
	CHECK(maemo_ladder_cache_entries(c) == 32, "32 curves held (got %u)",
	      maemo_ladder_cache_entries(c));

	/* Second pass: every one must come back identical and free. */
	for (unsigned i = 0; i < 32; i++) {
		char csv[64];
		snprintf(csv, sizeof(csv), "-%u,-%u,0", 4000 - i * 10, 2000 - i * 10);
		if (maemo_ladder_cache_get(c, csv) != held[i]) {
			CHECK(false, "curve %u came back different", i);
			break;
		}
	}
	CHECK(maemo_ladder_cache_parses(c) == 32,
	      "second pass cost no parses (parses=%u)",
	      maemo_ladder_cache_parses(c));
	CHECK(maemo_ladder_cache_hits(c) == 32, "second pass all hits (hits=%u)",
	      maemo_ladder_cache_hits(c));

	maemo_ladder_cache_free(c);
}

int main(void)
{
	test_parser();
	test_cache_identity();
	test_cache_content();
	test_cache_rejects_bad();
	test_cache_many();

	printf("\n%s\n", failures ? "FAIL" : "ALL PASS");
	return failures ? 1 : 0;
}
