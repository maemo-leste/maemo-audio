/* Volume map -- the tables, and the index mapping between them.
 *
 * The decision path must not compare amplitudes: "is 0.35 exactly
 * between 0.3 and 0.4" has no good answer in binary, and a float ->
 * string -> float round-trip never matched afterwards.
 *
 * The answer is not fixed point either. These quantities are inherently
 * discrete -- the slider has N steps, the codec has M, and choosing
 * between them is a comparison of step numbers -- so the indices travel
 * through the decisions and amplitudes stay float for the only thing they
 * are good at, being multiplied.
 *
 * **No float is ever used in a branch.** Exactly one float comparison
 * survives, at map build time, choosing which hardware step carries each
 * slider step: it runs once, yields an integer, and can be audited. */
#ifndef MAEMO_VOL_MAP_H
#define MAEMO_VOL_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include "maemo-export.h"
#include <stdint.h>

struct maemo_vol_map {
	/* Amplitudes. Float, multiplied, never branched on. */
	float *slider;		/* slider step -> virtual amplitude */
	unsigned n_slider;

	float *ladder;		/* hardware step -> hardware amplitude */
	unsigned n_ladder;

	/* The whole point: slider step -> the hardware step that carries
	 * it. Computed once, ascending, so direction is an integer
	 * comparison all the way down. */
	int *step_hw;
};

/* Build the map. Both amplitude tables are copied, so the caller may
 * free its arrays immediately. Returns NULL on allocation failure or if
 * a table is empty. */
struct maemo_vol_map *maemo_vol_map_new(const float *slider, unsigned n_slider,
		const float *ladder, unsigned n_ladder);

/* Build the map from one centibel ladder, with a Fremantle silence step
 * prepended to the slider.
 *
 * The slider has one more position than the hardware ladder and position
 * 0 is literal amplitude 0. It cannot be a cB entry: 10^(cb/2000) is
 * strictly positive, and -4000 cB is still -40 dBFS -- audible through
 * anything better than a tin speaker. Measured on the VM: target 0
 * landed at amp 0.2154 and music kept playing at the bottom of the
 * slider.
 *
 * The ladder keeps its own lowest step, which is the quietest the
 * hardware can produce, so the bottom of the slider is a rung of the
 * ladder and not a hole in it. Slider 0 maps to amplitude 0 and asks for
 * nothing of the hardware, which is also what makes this work over
 * Bluetooth, where there is no route to write and no hardware mute to
 * reach. */
struct maemo_vol_map *maemo_vol_map_new_from_cb_silence(
		const double *ladder_cb, unsigned n_ladder);

void maemo_vol_map_free(struct maemo_vol_map *m);

/* The hardware step chosen to carry `slider_step`.
 * Returns -1 if the step is out of range. */
MAEMO_EXPORT int maemo_vol_map_hw_step(const struct maemo_vol_map *m, unsigned slider_step);

/* Amplitude lookups. Return 0.0 when out of range. */
MAEMO_EXPORT float maemo_vol_map_virtual(const struct maemo_vol_map *m, unsigned slider_step);

/* Is this slider position the silence rung?
 *
 * Silence exists only on a map built by maemo_vol_map_new_from_cb_silence(),
 * where the slider has one more position than the hardware ladder and
 * position 0 is amplitude 0. That length difference is the whole
 * signature, so the test stays in integers, which is the rule for every
 * decision in here: amplitudes are multiplied, never compared. */
MAEMO_EXPORT bool maemo_vol_map_is_silence(const struct maemo_vol_map *m, int slider_step);
MAEMO_EXPORT float maemo_vol_map_hw(const struct maemo_vol_map *m, int hw_step);

/* The slider step that best matches a measured amplitude, for adopting a
 * level that something else set. Returns -1 if the map is unusable. */
MAEMO_EXPORT int maemo_vol_map_step_for_amp(const struct maemo_vol_map *m, float amp);

/* Tuning centibels to linear amplitude.
 *
 * A ladder centibel is an acoustic decibel times ten, so this is the
 * ordinary electrical conversion, amp = 10^(cb/2000):
 *
 *   -6000 cB (-60 dB) -> 0.001
 *    -900 cB  (-9 dB) -> 0.3548
 *       0 cB           -> 1.0
 *
 * Not to be confused with the PulseAudio step scale, which is cubic in
 * amplitude: step = 65536 * cbrt(amp) = 65536 * 10^(cb/6000). The
 * applet's slider is in that scale; this function is not, because a
 * ladder is quoted in centibels and both the mixer and the codec want
 * amplitude. Converting a slider step to amplitude is the cube in the
 * other direction, which is what the module applies to the target. */
float maemo_vol_cb_to_amp(double centibel);

/* Build the map from centibel tables instead of amplitudes. This is the
 * form the ladders actually arrive in -- the tuning properties and the
 * per-device config files are all in cB -- so the conversion happens
 * once, here, and nothing downstream has to know the unit exists. */
struct maemo_vol_map *maemo_vol_map_new_from_cb(const double *slider_cb,
		unsigned n_slider, const double *ladder_cb, unsigned n_ladder);

#endif /* MAEMO_VOL_MAP_H */
