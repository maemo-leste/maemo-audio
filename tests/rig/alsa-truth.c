/* alsa-truth -- what does an ALSA playback stream actually admit to?
 *
 * Standalone on purpose: it opens the PCM itself, so PipeWire must be
 * stopped first. That is also the point. Everything printed here is a
 * number the SPA module could report too; anything missing is a number we
 * cannot get through alsa-lib however we arrange the code above it.
 *
 * The question the whole flat-volume design turns on: can a playback
 * stream report the distance from "sample written" to "sample at the DAC"
 * as a measured quantity, the way the BlueZ sink reports its latency as a
 * sum of named components -- or is all ALSA will give us a buffer fill,
 * which is a property of the graph's buffering policy and not of the
 * hardware at all.
 *
 * What alsa-lib 1.2.14 does NOT expose, checked against the exported
 * symbol table rather than assumed:
 *   - snd_pcm_status_get_hw_ptr / _tick_seq / _htstamp_warning: the kernel
 *     struct carries them, there is no accessor. hw_ptr is recovered as
 *     written - delay, which is exact for playback because that is the
 *     definition of delay.
 *   - snd_pcm_hw_params_set_audio_tstamp_request: the report accessor
 *     exists (snd_pcm_status_get_audio_htstamp_report) but the request
 *     accessor does not, so a raw hardware audio timestamp can be read if
 *     the driver happens to be producing one and cannot be asked for.
 * What it does expose, and what matters:
 *   - snd_pcm_htimestamp(): the HSYNC ioctl, hardware pointer and the raw
 *     instant it was there. This is the only real measurement available.
 *   - snd_pcm_avail_delay(): avail and delay filled atomically, so the pair
 *     cannot straddle a period boundary the way two calls can.
 *
 * Build: gcc -O2 -o alsa-truth alsa-truth.c -lasound
 * Run:   pkill pipewire; pkill wireplumber; pkill pipewire-pulse
 *        ./alsa-truth [pcm] [period_frames] [seconds]
 */
#include <alsa/asoundlib.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(x) do { int _r = (x); if (_r < 0) { \
	fprintf(stderr, "%s: %s\n", #x, snd_strerror(_r)); return 1; } } while (0)

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000000000u + ts.tv_nsec;
}

static const char *tstamp_type_name(unsigned t)
{
	switch (t) {
	case SND_PCM_AUDIO_TSTAMP_TYPE_COMPAT: return "compat";
	case SND_PCM_AUDIO_TSTAMP_TYPE_DEFAULT: return "default(DMA per hw_ptr)";
	case SND_PCM_AUDIO_TSTAMP_TYPE_LINK: return "link";
	case SND_PCM_AUDIO_TSTAMP_TYPE_LINK_ABSOLUTE: return "link-absolute";
	case SND_PCM_AUDIO_TSTAMP_TYPE_LINK_ESTIMATED: return "link-estimated";
	case SND_PCM_AUDIO_TSTAMP_TYPE_LINK_SYNCHRONIZED: return "link-synced";
	default: return "?";
	}
}

int main(int argc, char *argv[])
{
	const char *dev = argc > 1 ? argv[1] : "hw:0,0";
	snd_pcm_uframes_t want_period = argc > 2 ? atol(argv[2]) : 1024;
	int seconds = argc > 3 ? atoi(argv[3]) : 5;
	unsigned int rate = 48000;
	snd_pcm_t *pcm = NULL;
	snd_pcm_hw_params_t *hw;
	snd_pcm_sw_params_t *sw;
	snd_pcm_status_t *status;
	snd_pcm_uframes_t buffer = want_period * 4, psize = want_period;
	snd_pcm_audio_tstamp_report_t report;
	int dirs, res;
	int16_t *silence;
	uint64_t written = 0, t_start;

	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_sw_params_alloca(&sw);
	snd_pcm_status_alloca(&status);

	printf("# device=%s want_period=%zu want_buffer=%zu rate=%u\n",
			dev, (size_t) want_period, (size_t) buffer, rate);

	CHECK(snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0));
	CHECK(snd_pcm_hw_params_any(pcm, hw));
	CHECK(snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED));
	CHECK(snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE));
	CHECK(snd_pcm_hw_params_set_channels(pcm, hw, 2));
	CHECK(snd_pcm_hw_params_set_rate(pcm, hw, rate, 0));

	/* Ask, then report what came back. A USB device that cannot do
	 * hardware timestamps is the common case, and "it failed" is far
	 * less useful than "it gave me this instead". */
	res = snd_pcm_hw_params_set_period_size_near(pcm, hw, &psize, &dirs);
	printf("# period_size: wanted %zu -> got %zu (%s)\n",
			(size_t) want_period, (size_t) psize, snd_strerror(res));
	res = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer);
	printf("# buffer_size: wanted %zu -> got %zu (%s)\n",
			(size_t) buffer, (size_t) buffer, snd_strerror(res));

	snd_pcm_sw_params_current(pcm, sw);
	snd_pcm_sw_params_set_tstamp_mode(pcm, sw, SND_PCM_TSTAMP_ENABLE);
	snd_pcm_sw_params_set_tstamp_type(pcm, sw, SND_PCM_TSTAMP_TYPE_MONOTONIC);
	snd_pcm_sw_params_set_avail_min(pcm, sw, psize);
	snd_pcm_sw_params_set_start_threshold(pcm, sw, buffer);
	CHECK(snd_pcm_hw_params(pcm, hw));
	CHECK(snd_pcm_sw_params(pcm, sw));

	{
		snd_pcm_uframes_t bs, ps, st;
		snd_pcm_hw_params_get_buffer_size(hw, &bs);
		snd_pcm_hw_params_get_period_size(hw, &ps, &dirs);
		snd_pcm_sw_params_get_start_threshold(sw, &st);
		printf("# committed: buffer=%zu period=%zu start_threshold=%zu\n",
				(size_t) bs, (size_t) ps, (size_t) st);
	}

	silence = calloc(psize * 2, sizeof(int16_t));
	for (int n = 0; n < 6; n++)
		if (snd_pcm_writei(pcm, silence, psize) < 0)
			snd_pcm_prepare(pcm);
	written = (uint64_t) psize * 6;
	snd_pcm_start(pcm);

	printf("# audio tstamp report: ");
	res = snd_pcm_status(pcm, status);
	snd_pcm_status_get_audio_htstamp_report(status, &report);
	printf("valid=%u actual_type=%u (%s) accuracy_report=%u accuracy=%u\n",
			report.valid, report.actual_type,
			tstamp_type_name(report.actual_type),
			report.accuracy_report, report.accuracy);

	printf("wall_ns,written,hw_ptr_from_delay,avail_status,avail_delay,"
	       "delay_delay,delay_status,avail_hts,havail,hts_age_ms,"
	       "audio_tstamp_ns,audio_valid,audio_type,avail_max,overrange,state\n");

	t_start = now_ns();
	while (now_ns() - t_start < (uint64_t) seconds * 1000000000u) {
		snd_pcm_sframes_t a_delay = 0, d_delay = 0, d_status = 0;
		snd_pcm_uframes_t avail_status, avail_max, overrange, havail = 0;
		snd_htimestamp_t hts, ats;
		uint64_t wall;
		double hts_age = 0.0;
		int hres;

		while (snd_pcm_avail(pcm) < (snd_pcm_sframes_t) psize)
			snd_pcm_wait(pcm, 50);
		res = snd_pcm_writei(pcm, silence, psize);
		if (res < 0) { snd_pcm_prepare(pcm); continue; }
		written += res;
		wall = now_ns();

		/* One status call: its fields are a snapshot. Two calls would
		 * straddle a period boundary and the columns would disagree
		 * for reasons that have nothing to do with the driver. */
		if (snd_pcm_status(pcm, status) < 0)
			continue;
		avail_status = snd_pcm_status_get_avail(status);
		avail_max    = snd_pcm_status_get_avail_max(status);
		overrange    = snd_pcm_status_get_overrange(status);
		d_status     = snd_pcm_status_get_delay(status);
		snd_pcm_status_get_htstamp(status, &hts);
		snd_pcm_status_get_audio_htstamp(status, &ats);
		snd_pcm_status_get_audio_htstamp_report(status, &report);

		/* atomic pair, the only way to get a consistent (avail,delay) */
		snd_pcm_avail_delay(pcm, &a_delay, &d_delay);

		/* the raw path: hardware pointer + the instant it was there */
		hres = snd_pcm_htimestamp(pcm, &havail, &hts);
		if (hres == 0) {
			uint64_t hts_ns = (uint64_t) hts.tv_sec * 1000000000u + hts.tv_nsec;
			hts_age = (double)((int64_t) wall - (int64_t) hts_ns) / 1e6;
		}

		printf("%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRId64
			",%" PRId64 ",%" PRId64 ",%d,%zu,%.3f,%" PRIu64 ",%u,%u,"
			"%zu,%zu,%d\n",
			wall, written,
			written - (uint64_t) (d_delay >= 0 ? d_delay : 0),
			(uint64_t) avail_status, (int64_t) a_delay,
			(int64_t) d_delay, (int64_t) d_status,
			hres, (size_t) havail, hts_age,
			(uint64_t) ats.tv_sec * 1000000000u + ats.tv_nsec,
			report.valid, report.actual_type,
			(size_t) avail_max, (size_t) overrange,
			snd_pcm_status_get_state(status));
	}

	free(silence);
	snd_pcm_close(pcm);
	return 0;
}
