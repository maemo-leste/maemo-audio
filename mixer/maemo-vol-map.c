/* Volume map -- see maemo-vol-map.h for why the float comparison lives here. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "maemo-vol-map.h"

/* The one float comparison in the whole volume stack.
 *
 * Returns the ladder index whose amplitude is nearest to `amp`. A tie
 * goes to the higher index: software can pull a louder hardware level
 * down without clipping, but cannot push a quieter one up without
 * spending headroom.
 *
 * The epsilon matters. "0.35 is exactly between 0.3 and 0.4" is true in
 * decimal and false in binary, and the tables are float32 rather than
 * double, which makes the discrepancy larger than it looks. See the
 * tolerance comment below.
 *
 * This runs once per slider step, at build time. Its output is an int. */
static int nearest_step(const float *steps, unsigned n, float amp)
{
	int best = 0;
	double best_d = fabs((double) steps[0] - (double) amp);

	for (unsigned i = 1; i < n; i++) {
		double d = fabs((double) steps[i] - (double) amp);
		/* The epsilon has to be sized to float32, not double. The tables
	 * are float, so "0.2 is exactly between 0.1 and 0.3" is already
	 * false before we do anything: in float32, 0.2f-0.1f and
	 * 0.3f-0.2f differ by about 7.5e-9, so a 1e-9 window misses the
	 * tie and silently picks the lower step. float32 carries roughly
	 * 1.2e-7 relative precision, so 1e-6 relative sits safely above
	 * the representation noise and far below any difference a real
	 * ladder would care about. */
	double tol = 1e-6 * fmax(fabs((double) steps[i]), fabs((double) amp));

		if (d + tol < best_d) {
			best = (int) i;
			best_d = d;
		}
		/* An exact tie leaves `best` on the lower index, so
		 * handle it explicitly: prefer the higher one. */
		else if (fabs(d - best_d) <= tol && (int) i > best)
			best = (int) i;
	}
	return best;
}

struct maemo_vol_map *maemo_vol_map_new(const float *slider, unsigned n_slider,
		const float *ladder, unsigned n_ladder)
{
	struct maemo_vol_map *m;

	if (slider == NULL || ladder == NULL || n_slider == 0 || n_ladder == 0)
		return NULL;

	m = calloc(1, sizeof(*m));
	if (m == NULL)
		return NULL;

	m->slider = malloc(n_slider * sizeof(*m->slider));
	m->ladder = malloc(n_ladder * sizeof(*m->ladder));
	m->step_hw = malloc(n_slider * sizeof(*m->step_hw));
	if (!m->slider || !m->ladder || !m->step_hw) {
		maemo_vol_map_free(m);
		return NULL;
	}

	memcpy(m->slider, slider, n_slider * sizeof(*m->slider));
	memcpy(m->ladder, ladder, n_ladder * sizeof(*m->ladder));
	m->n_slider = n_slider;
	m->n_ladder = n_ladder;

	for (unsigned i = 0; i < n_slider; i++)
		m->step_hw[i] = nearest_step(m->ladder, m->n_ladder, m->slider[i]);

	return m;
}

void maemo_vol_map_free(struct maemo_vol_map *m)
{
	if (m == NULL)
		return;
	free(m->slider);
	free(m->ladder);
	free(m->step_hw);
	free(m);
}

int maemo_vol_map_hw_step(const struct maemo_vol_map *m, unsigned slider_step)
{
	if (m == NULL || slider_step >= m->n_slider)
		return -1;
	return m->step_hw[slider_step];
}

float maemo_vol_map_virtual(const struct maemo_vol_map *m, unsigned slider_step)
{
	if (m == NULL || slider_step >= m->n_slider)
		return 0.0f;
	return m->slider[slider_step];
}

bool maemo_vol_map_is_silence(const struct maemo_vol_map *m, int slider_step)
{
	return m != NULL && slider_step == 0 && m->n_slider > m->n_ladder;
}

float maemo_vol_map_hw(const struct maemo_vol_map *m, int hw_step)
{
	if (m == NULL || hw_step < 0 || (unsigned) hw_step >= m->n_ladder)
		return 0.0f;
	return m->ladder[hw_step];
}

int maemo_vol_map_step_for_amp(const struct maemo_vol_map *m, float amp)
{
	if (m == NULL || m->n_slider == 0)
		return -1;
	return nearest_step(m->slider, m->n_slider, amp);
}

/* Tuning centibels to linear amplitude: amp = 10^(cb / 2000).
 *
 * Not the PulseAudio step scale, which the two are easy to confuse
 * because both read "65536 * 10^(cb / ...)". PA's is cubic --
 * step = NORM * cbrt(amp) = NORM * 10^(cb / 6000) -- and that is what
 * the applet's table holds. The mixer multiplies real samples and drives
 * a real codec, so it needs the amplitude, not its cube root: in the
 * step scale -4000 cB becomes 0.2154 (-13.3 dBFS) instead of 0.01,
 * collapsing the bottom 26.7 dB of the ladder into the top.
 *
 * The two scales differ by exactly the cube pipewire-pulse applies in
 * volume_to_linear(), so a ladder read here and a target read there land
 * on the same step. */
float maemo_vol_cb_to_amp(double centibel)
{
	return (float) pow(10.0, centibel / 2000.0);
}

/* Convert one cB table to amplitudes. Returns NULL on failure; the
 * caller frees both tables. */
static float *cb_table_to_amp(const double *cb, unsigned n)
{
	float *amp;

	if (cb == NULL || n == 0)
		return NULL;

	amp = malloc(n * sizeof(*amp));
	if (amp == NULL)
		return NULL;

	for (unsigned i = 0; i < n; i++) {
		amp[i] = maemo_vol_cb_to_amp(cb[i]);

		/* A ladder must ascend. The applet rejects a non-ascending
		 * tuning property for the same reason: nearest_step assumes
		 * it, and a table that wanders would pick an arbitrary step
		 * rather than an obviously wrong one. Catching it here turns
		 * a bad config file into a load failure instead of a volume
		 * curve that is subtly wrong in the middle. */
		if (i > 0 && amp[i] <= amp[i - 1]) {
			free(amp);
			return NULL;
		}
	}
	return amp;
}

struct maemo_vol_map *maemo_vol_map_new_from_cb(const double *slider_cb,
		unsigned n_slider, const double *ladder_cb, unsigned n_ladder)
{
	float *slider_amp;
	float *ladder_amp;
	struct maemo_vol_map *m;

	slider_amp = cb_table_to_amp(slider_cb, n_slider);
	if (slider_amp == NULL)
		return NULL;

	ladder_amp = cb_table_to_amp(ladder_cb, n_ladder);
	if (ladder_amp == NULL) {
		free(slider_amp);
		return NULL;
	}

	m = maemo_vol_map_new(slider_amp, n_slider, ladder_amp, n_ladder);
	free(slider_amp);
	free(ladder_amp);
	return m;
}

struct maemo_vol_map *maemo_vol_map_new_from_cb_silence(
		const double *ladder_cb, unsigned n_ladder)
{
	float *ladder_amp;
	float *slider_amp;
	struct maemo_vol_map *m;

	ladder_amp = cb_table_to_amp(ladder_cb, n_ladder);
	if (ladder_amp == NULL)
		return NULL;

	/* The slider is the ladder with silence in front of it. Copy the
	 * converted amplitudes rather than converting twice so the two
	 * tables cannot drift apart on a rounding difference. */
	slider_amp = malloc((n_ladder + 1) * sizeof(*slider_amp));
	if (slider_amp == NULL) {
		free(ladder_amp);
		return NULL;
	}
	slider_amp[0] = 0.0f;
	memcpy(slider_amp + 1, ladder_amp, n_ladder * sizeof(*ladder_amp));

	m = maemo_vol_map_new(slider_amp, n_ladder + 1, ladder_amp, n_ladder);
	free(slider_amp);
	free(ladder_amp);
	return m;
}
