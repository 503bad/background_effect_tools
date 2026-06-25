#include "bgfx-audio.h"
#include "bgfx-props.h"

#include <obs.h>
#include <obs-module.h>
#include <media-io/audio-io.h>
#include <util/threading.h>
#include <util/bmem.h>

#include <math.h>
#include <string.h>

/* Settings keys. "audio_enable" is the master switch (a host-level bool);
 * everything else is namespaced under "audio_". */
#define K_SOURCE      "audio_source"
#define K_GAIN        "audio_gain"
#define K_ATTACK      "audio_attack"
#define K_RELEASE     "audio_release"
#define K_SIZE_ON     "audio_size"
#define K_SIZE_AMT    "audio_size_amt"
#define K_COLOR_ON    "audio_color"
#define K_COLOR_AMT   "audio_color_amt"
#define K_PEAK_ON     "audio_color_peak_on"
#define K_PEAK        "audio_color_peak"
#define K_BOUNCE_ON   "audio_bounce"
#define K_BOUNCE_AMT  "audio_bounce_amt"
#define K_BOUNCE_SPD  "audio_bounce_speed"

/* 4096-pt window → ~11.7 Hz bins at 48 kHz. 1024 was too coarse for the bass:
 * the whole 30–90 Hz kick range fell into a single FFT bin, so per-effect kick
 * band (lo/hi Hz) selectors all read the same data and appeared to do nothing.
 * Bar magnitudes are normalised by FFT_N (line ~288) so absolute levels — and
 * the thresholds that depend on them — are unchanged by this. */
#define FFT_N    4096        /* analysis window (power of two)            */
#define RING_CAP 8192        /* PCM ring capacity (power of two)          */
#define RING_MASK (RING_CAP - 1)

struct bg_audio_meter {
	pthread_mutex_t mutex;

	/* Strong ref to the captured source while attached (NULL otherwise). */
	obs_source_t *src;
	char         *name;

	/* Audio-thread → video-thread PCM hand-off (mono, channel 0). */
	float    ring[RING_CAP];
	uint32_t widx;       /* next write index                           */
	uint64_t total;      /* total samples written (for warm-up gating) */

	/* Precomputed FFT tables. */
	float    hann[FFT_N];
	float    wcos[FFT_N / 2];
	float    wsin[FFT_N / 2];
	uint16_t brev[FFT_N];

	/* Analysis scratch / smoothed results (video thread only). */
	float    re[FFT_N];
	float    im[FFT_N];
	float    level;       /* smoothed overall                          */
	float    bass_avg;    /* running bass average for onset detection  */
	float    since_beat;  /* seconds since the last beat               */
	struct bg_audio_fft fft;
};

/* --- audio thread: copy channel 0 into the ring --------------------------- */
static void capture_cb(void *param, obs_source_t *source,
		       const struct audio_data *ad, bool muted)
{
	UNUSED_PARAMETER(source);
	struct bg_audio_meter *m = param;
	if (!ad || ad->frames == 0)
		return;
	const float *ch = (const float *)ad->data[0];

	pthread_mutex_lock(&m->mutex);
	uint32_t w = m->widx;
	for (uint32_t i = 0; i < ad->frames; ++i) {
		m->ring[w & RING_MASK] = (ch && !muted) ? ch[i] : 0.0f;
		++w;
	}
	m->widx = w;
	m->total += ad->frames;
	pthread_mutex_unlock(&m->mutex);
}

struct bg_audio_meter *bg_audio_create(void)
{
	struct bg_audio_meter *m = bzalloc(sizeof(*m));
	pthread_mutex_init(&m->mutex, NULL);

	/* Hann window + FFT twiddle/bit-reversal tables (computed once). */
	for (int i = 0; i < FFT_N; ++i)
		m->hann[i] = 0.5f - 0.5f * cosf(6.28318530718f * (float)i /
						(float)(FFT_N - 1));
	for (int k = 0; k < FFT_N / 2; ++k) {
		float ang = -6.28318530718f * (float)k / (float)FFT_N;
		m->wcos[k] = cosf(ang);
		m->wsin[k] = sinf(ang);
	}
	for (uint32_t i = 0; i < FFT_N; ++i) {
		uint32_t r = 0, x = i;
		for (int b = 1; b < FFT_N; b <<= 1) {
			r = (r << 1) | (x & 1);
			x >>= 1;
		}
		m->brev[i] = (uint16_t)r;
	}
	m->fft.bar_count = BG_FFT_BARS;
	m->fft.wave_count = BG_WAVE_POINTS;
	return m;
}

static void detach(struct bg_audio_meter *m)
{
	if (m->src) {
		obs_source_remove_audio_capture_callback(m->src, capture_cb, m);
		obs_source_release(m->src);
		m->src = NULL;
	}
}

void bg_audio_destroy(struct bg_audio_meter *m)
{
	if (!m)
		return;
	detach(m);
	bfree(m->name);
	pthread_mutex_destroy(&m->mutex);
	bfree(m);
}

void bg_audio_set_source(struct bg_audio_meter *m, const char *name)
{
	if (!m)
		return;
	if (name && name[0] == '\0')
		name = NULL;

	/* No change → keep the existing attachment. */
	bool same = (!name && !m->name) ||
		    (name && m->name && strcmp(name, m->name) == 0);
	if (same)
		return;

	detach(m);
	bfree(m->name);
	m->name = name ? bstrdup(name) : NULL;

	if (m->name) {
		obs_source_t *src = obs_get_source_by_name(m->name);
		if (src) {
			obs_source_add_audio_capture_callback(src, capture_cb, m);
			m->src = src; /* keep the ref until detach */
		}
	}

	pthread_mutex_lock(&m->mutex);
	memset(m->ring, 0, sizeof(m->ring));
	m->widx = 0;
	m->total = 0;
	pthread_mutex_unlock(&m->mutex);

	m->level = 0.0f;
	m->bass_avg = 0.0f;
	m->since_beat = 1.0f;
	memset(&m->fft.bars, 0, sizeof(m->fft.bars));
	memset(&m->fft.wave, 0, sizeof(m->fft.wave));
	m->fft.bass = m->fft.mid = m->fft.treble = 0.0f;
	m->fft.beat = 0.0f;
}

/* In-place iterative radix-2 FFT using the precomputed tables. */
static void fft_run(struct bg_audio_meter *m)
{
	float *re = m->re, *im = m->im;
	for (uint32_t i = 0; i < FFT_N; ++i) {
		uint32_t j = m->brev[i];
		if (j > i) {
			float t = re[i]; re[i] = re[j]; re[j] = t;
			t = im[i]; im[i] = im[j]; im[j] = t;
		}
	}
	for (int len = 2; len <= FFT_N; len <<= 1) {
		int half = len >> 1;
		int step = FFT_N / len;
		for (int i = 0; i < FFT_N; i += len) {
			int k = 0;
			for (int jj = 0; jj < half; ++jj) {
				float wr = m->wcos[k], wi = m->wsin[k];
				int a = i + jj, b = a + half;
				float tr = wr * re[b] - wi * im[b];
				float ti = wr * im[b] + wi * re[b];
				re[b] = re[a] - tr;
				im[b] = im[a] - ti;
				re[a] += tr;
				im[a] += ti;
				k += step;
			}
		}
	}
}

static float clamp01(float v)
{
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

float bg_audio_tick(struct bg_audio_meter *m, float dt, float gain_db,
		    float attack, float release)
{
	if (!m)
		return 0.0f;

	/* The source may not have existed when it was first selected (scene
	 * load order); resolve lazily until we latch on. */
	if (m->name && !m->src) {
		obs_source_t *src = obs_get_source_by_name(m->name);
		if (src) {
			obs_source_add_audio_capture_callback(src, capture_cb, m);
			m->src = src;
		}
	}

	/* Snapshot the most recent FFT_N samples in order. */
	float win[FFT_N];
	uint32_t w;
	uint64_t total;
	pthread_mutex_lock(&m->mutex);
	w = m->widx;
	total = m->total;
	for (int i = 0; i < FFT_N; ++i)
		win[i] = m->ring[(w - FFT_N + i) & RING_MASK];
	pthread_mutex_unlock(&m->mutex);

	m->fft.valid = (m->src != NULL) && (total >= FFT_N);

	float gain = powf(10.0f, gain_db / 20.0f);

	/* --- overall level (RMS over the window) --- */
	double ss = 0.0;
	for (int i = 0; i < FFT_N; ++i)
		ss += (double)win[i] * (double)win[i];
	float rms = (float)sqrt(ss / (double)FFT_N);
	float lvl = clamp01(rms * gain);
	float tc = (lvl > m->level) ? attack : release;
	float sa = (tc > 1e-4f) ? 1.0f - expf(-dt / tc) : 1.0f;
	m->level += (lvl - m->level) * sa;
	m->fft.level = m->level;

	if (!m->fft.valid)
		return m->level;

	/* --- FFT --- */
	for (int i = 0; i < FFT_N; ++i) {
		m->re[i] = win[i] * m->hann[i];
		m->im[i] = 0.0f;
	}
	fft_run(m);

	int sr = 48000;
	audio_t *ao = obs_get_audio();
	if (ao)
		sr = (int)audio_output_get_sample_rate(ao);
	if (sr <= 0)
		sr = 48000;

	/* --- log-spaced bars --- */
	const float fmin = 30.0f, fmax = (float)sr * 0.45f;
	const float ratio = fmax / fmin;
	m->fft.freq_min = fmin;
	m->fft.freq_max = fmax;
	float ba = (lvl > m->fft.bars[0]) ? attack : release; /* reuse curves */
	float bar_a = (ba > 1e-4f) ? 1.0f - expf(-dt / (ba + 0.02f)) : 1.0f;
	for (int b = 0; b < BG_FFT_BARS; ++b) {
		float f_lo = fmin * powf(ratio, (float)b / BG_FFT_BARS);
		float f_hi = fmin * powf(ratio, (float)(b + 1) / BG_FFT_BARS);
		int k_lo = (int)(f_lo * FFT_N / sr);
		int k_hi = (int)(f_hi * FFT_N / sr);
		if (k_lo < 1)
			k_lo = 1;
		if (k_hi <= k_lo)
			k_hi = k_lo + 1;
		if (k_hi > FFT_N / 2)
			k_hi = FFT_N / 2;
		float peak = 0.0f;
		for (int k = k_lo; k < k_hi; ++k) {
			float mag = sqrtf(m->re[k] * m->re[k] +
					  m->im[k] * m->im[k]);
			if (mag > peak)
				peak = mag;
		}
		/* Normalise + perceptual curve; gain shifts sensitivity. */
		float e = peak / (FFT_N * 0.25f);
		float v = clamp01(sqrtf(e * gain));
		float prev = m->fft.bars[b];
		float a = (v > prev) ? bar_a : (release > 1e-4f
						? 1.0f - expf(-dt / release)
						: 1.0f);
		m->fft.bars[b] = prev + (v - prev) * a;
	}

	/* --- band energies from the bars --- */
	float bass = 0, mid = 0, treble = 0;
	for (int b = 0; b < 8; ++b)
		bass += m->fft.bars[b];
	for (int b = 8; b < 32; ++b)
		mid += m->fft.bars[b];
	for (int b = 32; b < BG_FFT_BARS; ++b)
		treble += m->fft.bars[b];
	m->fft.bass = clamp01(bass / 8.0f);
	m->fft.mid = clamp01(mid / 24.0f);
	m->fft.treble = clamp01(treble / 32.0f);

	/* --- beat detection on raw bass energy --- */
	float bass_now = m->fft.bass;
	m->bass_avg += (bass_now - m->bass_avg) * 0.12f;
	m->since_beat += dt;
	m->fft.beat_trigger = false;
	if (bass_now > m->bass_avg * 1.35f && bass_now > 0.12f &&
	    m->since_beat > 0.12f) {
		m->fft.beat = 1.0f;
		m->fft.beat_trigger = true;
		m->since_beat = 0.0f;
	} else {
		m->fft.beat *= expf(-dt / 0.12f);
	}

	/* --- downsampled waveform for the oscilloscope --- */
	int stride = FFT_N / BG_WAVE_POINTS;
	for (int i = 0; i < BG_WAVE_POINTS; ++i) {
		float v = win[i * stride] * gain;
		m->fft.wave[i] = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
	}

	return m->level;
}

const struct bg_audio_fft *bg_audio_get_fft(const struct bg_audio_meter *m)
{
	return m ? &m->fft : NULL;
}

/* --- properties ----------------------------------------------------------- */

static bool enum_audio_source(void *param, obs_source_t *src)
{
	obs_property_t *list = param;
	if (obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) {
		const char *name = obs_source_get_name(src);
		if (name && name[0])
			obs_property_list_add_string(list, name, name);
	}
	return true;
}

void bg_audio_props(obs_properties_t *g)
{
	obs_property_t *src = obs_properties_add_list(g, K_SOURCE,
		obs_module_text("AudioSource"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(src, obs_module_text("AudioSourceNone"), "");
	obs_enum_sources(enum_audio_source, src);

	obs_properties_add_float_slider(g, K_GAIN,
		obs_module_text("AudioGain"), -30.0, 30.0, 0.5);
	obs_properties_add_float_slider(g, K_ATTACK,
		obs_module_text("AudioAttack"), 0.0, 0.5, 0.005);
	obs_properties_add_float_slider(g, K_RELEASE,
		obs_module_text("AudioRelease"), 0.0, 1.5, 0.01);

	/* --- targets (independent, combinable) --- */
	obs_properties_add_bool(g, K_SIZE_ON, obs_module_text("AudioSize"));
	obs_properties_add_float_slider(g, K_SIZE_AMT,
		obs_module_text("AudioSizeAmount"), 0.0, 4.0, 0.05);

	obs_properties_add_bool(g, K_COLOR_ON, obs_module_text("AudioColor"));
	obs_properties_add_float_slider(g, K_COLOR_AMT,
		obs_module_text("AudioColorAmount"), 0.0, 1.0, 0.01);
	obs_properties_add_bool(g, K_PEAK_ON,
		obs_module_text("AudioColorPeakOn"));
	obs_properties_add_color(g, K_PEAK, obs_module_text("AudioColorPeak"));

	obs_properties_add_bool(g, K_BOUNCE_ON, obs_module_text("AudioBounce"));
	obs_properties_add_float_slider(g, K_BOUNCE_AMT,
		obs_module_text("AudioBounceAmount"), 0.0, 400.0, 1.0);
	obs_properties_add_float_slider(g, K_BOUNCE_SPD,
		obs_module_text("AudioBounceSpeed"), 0.5, 20.0, 0.1);
}

void bg_audio_defaults(obs_data_t *s)
{
	obs_data_set_default_string(s, K_SOURCE, "");
	obs_data_set_default_double(s, K_GAIN, 6.0);
	obs_data_set_default_double(s, K_ATTACK, 0.02);
	obs_data_set_default_double(s, K_RELEASE, 0.15);

	obs_data_set_default_bool(s, K_SIZE_ON, true);
	obs_data_set_default_double(s, K_SIZE_AMT, 1.5);

	obs_data_set_default_bool(s, K_COLOR_ON, false);
	obs_data_set_default_double(s, K_COLOR_AMT, 0.6);
	obs_data_set_default_bool(s, K_PEAK_ON, false);
	obs_data_set_default_int(s, K_PEAK, (long long)0xFFFFFFFF);

	obs_data_set_default_bool(s, K_BOUNCE_ON, false);
	obs_data_set_default_double(s, K_BOUNCE_AMT, 80.0);
	obs_data_set_default_double(s, K_BOUNCE_SPD, 8.0);
}

void bg_audio_read(struct bg_audio_mod *mod, float *gain_db, float *attack,
		   float *release, obs_data_t *s)
{
	*gain_db = (float)obs_data_get_double(s, K_GAIN);
	*attack = (float)obs_data_get_double(s, K_ATTACK);
	*release = (float)obs_data_get_double(s, K_RELEASE);

	mod->size_on = obs_data_get_bool(s, K_SIZE_ON);
	mod->size_amount = (float)obs_data_get_double(s, K_SIZE_AMT);

	mod->color_on = obs_data_get_bool(s, K_COLOR_ON);
	mod->color_amount = (float)obs_data_get_double(s, K_COLOR_AMT);
	mod->color_peak_on = obs_data_get_bool(s, K_PEAK_ON);
	mod->color_peak = (uint32_t)obs_data_get_int(s, K_PEAK);

	mod->bounce_on = obs_data_get_bool(s, K_BOUNCE_ON);
	mod->bounce_amount = (float)obs_data_get_double(s, K_BOUNCE_AMT);
	mod->bounce_speed = (float)obs_data_get_double(s, K_BOUNCE_SPD);
}

void bg_audio_react_init(struct bg_audio_react *r, const struct bg_audio_mod *m)
{
	r->size_mul = 1.0f;
	r->bounce_amp = 0.0f;
	r->bounce_speed = 0.0f;
	r->color_on = false;
	r->color_peak_on = false;
	r->color_amt = 0.0f;
	r->peak[0] = r->peak[1] = r->peak[2] = r->peak[3] = 0.0f;

	float lvl = (m && m->enabled) ? m->level : 0.0f;
	if (lvl <= 0.0f)
		return;

	if (m->size_on)
		r->size_mul = 1.0f + m->size_amount * lvl;
	if (m->bounce_on) {
		r->bounce_amp = m->bounce_amount * lvl;
		r->bounce_speed = m->bounce_speed;
	}
	if (m->color_on) {
		r->color_on = true;
		r->color_peak_on = m->color_peak_on;
		r->color_amt = m->color_amount * lvl;
		if (m->color_peak_on)
			bg_unpack_color(m->color_peak, r->peak);
	}
}
