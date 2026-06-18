/* Audio-reactive particle visualizer.
 *
 * One selectable effect that hosts six presets (a "Preset" dropdown, mirroring
 * the spectrum/vortex multi-mode effects). The novel part versus the existing
 * spectrum effect is the *drive-signal* layer (§1/§12 of the spec): each frame
 * the presets read a single `struct viz_drive` (rms / bands / spectrum / beat /
 * peak). Those signals come from the host FFT meter when audio is live, or from
 * a built-in Seed generator (§2.6: LFO + scrolling noise + auto-beat) when the
 * source is absent / silent / switched off. The two supplies cross-fade, so the
 * preset logic is written once and never freezes on a dead canvas.
 *
 * Presets:
 *   0 BARS        spectrum particle columns                (autonomy: limited)
 *   1 RAIN        digital-rain sphere                       (autonomy: full)
 *   2 SPECTROGRAM 2.5D waterfall                            (autonomy: limited)
 *   3 FIELD       waveform displacement band               (autonomy: full)
 *   4 COMET       turbulence flow / comet                   (autonomy: full)
 *   5 TWINKLE     ambient twinkle background layer          (autonomy: full)
 */

#include "effect-audioviz.h"
#include "bgfx-effect.h"
#include "bgfx-particles.h"
#include "bgfx-props.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>

#include <math.h>

#define PRE "audioviz"
#define CAPACITY 40000
#define BG_TAU 6.28318530718f

#define SPEC_BINS  BG_FFT_BARS /* 64 drive-spectrum bins (low→high) */
#define HIST_CAP   150         /* spectrogram history rows (capped)  */
#define FREQ_CAP   96          /* spectrogram columns (capped)       */

enum av_preset {
	AV_BARS = 0,
	AV_RAIN = 1,
	AV_SPECTRO = 2,
	AV_FIELD = 3,
	AV_COMET = 4,
	AV_TWINKLE = 5,
};

enum av_lfo { LFO_SINE = 0, LFO_TRI = 1, LFO_SAW = 2, LFO_SNOISE = 3 };
enum av_colmode { COL_SINGLE = 0, COL_GRAD = 1, COL_SPEC = 2 };
enum av_blend { BLEND_ADD = 0, BLEND_SCREEN = 1, BLEND_NORMAL = 2 };

static const struct bg_common_spec k_spec = {
	.size_min = 1.0, .size_max = 64.0, .size_step = 0.5, .size_def = 6.0,
	.life_min = 0.1, .life_max = 10.0, .life_def = 2.0,
	.rate_max = 5000.0, .rate_def = 1000.0,
	.max_cap = CAPACITY, .max_def = 20000,
	.color_def = 0xFFFF7F2D, /* #2D7FFF blue */
	.alpha_def = 1.0,
};

/* One frame of drive signals (the common input vocabulary, §1). */
struct viz_drive {
	float rms;             /* 0..1 overall level                     */
	float band[3];         /* low / mid / high, 0..1                  */
	float spectrum[SPEC_BINS]; /* 0..1, low→high                     */
	float beat;            /* 0..1, decays after an onset            */
	bool  beat_trigger;    /* true on the frame an onset fires       */
	float peak;            /* 0..1 short-time peak                    */
};

struct audioviz_state {
	gs_effect_t *viz;    /* flat vertex-colour shader (geometry)   */
	gs_effect_t *sprite; /* billboard shader (particle presets)    */
	struct bg_particle_system *sys;

	struct bg_common common;
	struct bg_post post;

	/* ---- shared params (§2.3–2.5) ---- */
	int      preset;
	float    reactivity;
	float    time_scale;
	int      colmode;
	uint32_t color2;
	float    grad;
	int      blend;
	bool     transparent;
	uint32_t bgcolor;
	float    gravity;
	float    drag;
	float    turb_int, turb_scale, turb_speed;
	float    jitter;

	/* ---- Seed generator (§2.6) ---- */
	bool     auto_idle;
	float    idle_fade;
	float    lfo_rate, lfo_depth;
	int      lfo_wave;
	float    idle_noise_speed, idle_noise_amount;
	bool     autobeat_on;
	float    autobeat_bpm, autobeat_jitter;

	/* ---- per-preset params ---- */
	/* bars */
	int   bars_count, bars_ppb, bars_idle;
	float bars_width, bars_hscale, bars_spread, bars_boost, bars_tilt;
	/* rain */
	int   rain_columns, rain_trail;
	float rain_radius, rain_fall, rain_flash, rain_depth, rain_edge, rain_rot;
	/* spectrogram */
	int   spec_freq, spec_hist, spec_ramp, spec_idle;
	float spec_hscale, spec_scroll, spec_angle, spec_dist;
	/* field */
	int   field_gx, field_gy, field_wsrc, field_wcount;
	float field_width, field_amp, field_travel, field_bokeh, field_glow,
		field_pulse;
	/* comet */
	int   comet_path;
	float comet_curv, comet_speed, comet_width, comet_tail, comet_spark,
		comet_travel, comet_swirl;
	/* twinkle */
	int   tw_count;
	float tw_speed, tw_depth, tw_drift, tw_bright, tw_sparkle, tw_top;

	/* ---- runtime ---- */
	float col1[4], col2[4];
	float clock;            /* time-scaled effect clock              */
	float noise_seed;       /* per-instance offset for seed-gen noise */
	struct viz_drive dr;    /* this frame's drive signals            */

	/* seed generator carry / followers */
	float idle_blend;       /* 0 = audio, 1 = seed generator         */
	float idle_beat;        /* decaying auto-beat pulse              */
	bool  idle_beat_trig;
	float beat_acc;         /* auto-beat accumulator                */
	float next_beat;        /* jittered period to the next pulse     */
	float peak_audio;       /* peak follower over the audio level    */

	/* spectrogram + waterfall scroll */
	float hist[HIST_CAP][FREQ_CAP];
	int   hist_head;        /* index of the newest row              */
	int   hist_len;         /* rows filled so far                   */
	float scroll_acc;       /* fractional row scroll carry          */

	int   last_preset;      /* detect preset switches → clear pool   */

	/* twinkle / field need a persistent particle set; this flags first run */
	bool  seeded_field;
	bool  seeded_twinkle;
	float twinkle_seed_w, twinkle_seed_h; /* canvas the stars were laid on */
	float field_seed_w, field_seed_h;
};

/* ---- small math helpers -------------------------------------------------- */

static float clamp01f(float v)
{
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

static uint32_t pack_rgba(float r, float g, float b, float a)
{
	uint32_t R = (uint32_t)(r < 0 ? 0 : (r > 1 ? 255 : r * 255.0f + 0.5f));
	uint32_t G = (uint32_t)(g < 0 ? 0 : (g > 1 ? 255 : g * 255.0f + 0.5f));
	uint32_t B = (uint32_t)(b < 0 ? 0 : (b > 1 ? 255 : b * 255.0f + 0.5f));
	uint32_t A = (uint32_t)(a < 0 ? 0 : (a > 1 ? 255 : a * 255.0f + 0.5f));
	return (A << 24) | (B << 16) | (G << 8) | R; /* OBS 0xAABBGGRR */
}

/* Premultiplied-alpha colour for the viz geometry blend. */
static uint32_t premul(const float rgb[3], float intensity)
{
	float i = clamp01f(intensity);
	return pack_rgba(rgb[0] * i, rgb[1] * i, rgb[2] * i, i);
}

static float fract1(float x) { return x - floorf(x); }

static float hash1(float x)
{
	return fract1(sinf(x * 127.1f) * 43758.5453f);
}

/* 1D value noise (smooth, 0..1). */
static float vnoise1(float x)
{
	float i = floorf(x), f = x - i;
	f = f * f * (3.0f - 2.0f * f);
	return lerpf(hash1(i), hash1(i + 1.0f), f);
}

/* 1D fbm, 0..1-ish. */
static float fbm1(float x)
{
	float f = 0.0f, amp = 0.5f, sum = 0.0f;
	for (int i = 0; i < 4; ++i) {
		f += amp * vnoise1(x);
		sum += amp;
		x *= 2.03f;
		amp *= 0.5f;
	}
	return f / sum;
}

static float hash2(float x, float y)
{
	return fract1(sinf(x * 127.1f + y * 311.7f) * 43758.5453f);
}

static float vnoise2(float x, float y)
{
	float ix = floorf(x), iy = floorf(y);
	float fx = x - ix, fy = y - iy;
	float ux = fx * fx * (3.0f - 2.0f * fx);
	float uy = fy * fy * (3.0f - 2.0f * fy);
	float a = hash2(ix, iy), b = hash2(ix + 1, iy);
	float c = hash2(ix, iy + 1), d = hash2(ix + 1, iy + 1);
	return lerpf(lerpf(a, b, ux), lerpf(c, d, ux), uy);
}

/* Divergence-free 2D curl of fbm potential ψ(x,y,t). Used by the comet flow. */
static void curl2(float x, float y, float t, float scale, float *vx, float *vy)
{
	const float e = 1.0f;
	float ns = scale;
#define PSI(px, py) (fbm1((px)*ns + (py)*ns * 1.7f + t) + \
		     vnoise2((px)*ns * 0.5f, (py)*ns * 0.5f + t))
	float dpdx = (PSI(x + e, y) - PSI(x - e, y)) / (2.0f * e);
	float dpdy = (PSI(x, y + e) - PSI(x, y - e)) / (2.0f * e);
#undef PSI
	*vx = dpdy;
	*vy = -dpdx;
}

/* ---- colour selection ---------------------------------------------------- */

/* Fill `out` with the colour for normalized position/level `t` (0..1) under the
 * current colour mode. SPECTRUM walks blue→white→pink across `t`. */
static void av_color(const struct audioviz_state *s, float t, float out[3])
{
	t = clamp01f(t);
	if (s->colmode == COL_SINGLE) {
		out[0] = s->col1[0];
		out[1] = s->col1[1];
		out[2] = s->col1[2];
	} else if (s->colmode == COL_GRAD) {
		float u = t * s->grad;
		for (int i = 0; i < 3; ++i)
			out[i] = s->col1[i] + (s->col2[i] - s->col1[i]) * u;
	} else { /* COL_SPEC: A → white → B */
		if (t < 0.5f) {
			float u = t * 2.0f;
			for (int i = 0; i < 3; ++i)
				out[i] = s->col1[i] + (1.0f - s->col1[i]) * u;
		} else {
			float u = (t - 0.5f) * 2.0f;
			for (int i = 0; i < 3; ++i)
				out[i] = 1.0f + (s->col2[i] - 1.0f) * u;
		}
	}
}

/* Heat / mono / spectrum ramp for the spectrogram height colour. */
static void av_ramp(const struct audioviz_state *s, float t, float out[3])
{
	t = clamp01f(t);
	if (s->spec_ramp == 0) { /* heat: dark red → yellow → white */
		out[0] = clamp01f(0.3f + t * 2.0f);
		out[1] = clamp01f(t * 1.6f - 0.1f);
		out[2] = clamp01f(t * t * 1.3f - 0.2f);
	} else if (s->spec_ramp == 1) { /* mono (col1 brightness) */
		out[0] = s->col1[0] * (0.2f + 0.8f * t);
		out[1] = s->col1[1] * (0.2f + 0.8f * t);
		out[2] = s->col1[2] * (0.2f + 0.8f * t);
	} else {
		av_color(s, t, out);
	}
}

/* ---- vertex helpers ------------------------------------------------------ */

static void vtx(uint32_t col, float x, float y)
{
	gs_color(col);
	gs_vertex2f(x, y);
}

static void quad(uint32_t col, float ax, float ay, float bx, float by, float cx,
		 float cy, float dx, float dy)
{
	vtx(col, ax, ay);
	vtx(col, bx, by);
	vtx(col, cx, cy);
	vtx(col, ax, ay);
	vtx(col, cx, cy);
	vtx(col, dx, dy);
}

static void push_blend(int mode)
{
	gs_blend_state_push();
	if (mode == BLEND_ADD)
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ONE);
	else if (mode == BLEND_SCREEN)
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCCOLOR);
	else
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
}

/* ---- lifecycle ----------------------------------------------------------- */

static void *audioviz_create(void)
{
	struct audioviz_state *s = bzalloc(sizeof(*s));
	s->sys = bg_particles_create(CAPACITY);
	s->sys->fade_in = 0.06f;
	s->sys->fade_out = 0.2f;
	s->next_beat = 0.6f;
	s->idle_blend = 1.0f;
	s->last_preset = -1;
	return s;
}

static void audioviz_destroy(void *data)
{
	struct audioviz_state *s = data;
	if (!s)
		return;
	if (s->viz)
		gs_effect_destroy(s->viz);
	if (s->sprite)
		gs_effect_destroy(s->sprite);
	bg_particles_destroy(s->sys);
	bfree(s);
}

static gs_effect_t *load_viz(void)
{
	char *path = obs_module_file("effects/viz.effect");
	gs_effect_t *e = NULL;
	if (path)
		e = gs_effect_create_from_file(path, NULL);
	bfree(path);
	return e;
}

static void audioviz_load_graphics(void *data)
{
	struct audioviz_state *s = data;
	s->viz = load_viz();
	s->sprite = bg_particles_load_effect();
}

static double getd(obs_data_t *s, const char *name)
{
	char k[96];
	return obs_data_get_double(s, bg_key(k, sizeof(k), PRE, name));
}
static long long geti(obs_data_t *s, const char *name)
{
	char k[96];
	return obs_data_get_int(s, bg_key(k, sizeof(k), PRE, name));
}
static bool getb(obs_data_t *s, const char *name)
{
	char k[96];
	return obs_data_get_bool(s, bg_key(k, sizeof(k), PRE, name));
}

static void audioviz_update(void *data, obs_data_t *settings)
{
	struct audioviz_state *s = data;
	bg_common_update(&s->common, settings, PRE);
	bg_post_update(&s->post, settings, PRE);

	s->preset = (int)geti(settings, "preset");
	s->reactivity = (float)getd(settings, "reactivity");
	s->time_scale = (float)getd(settings, "time_scale");
	s->colmode = (int)geti(settings, "colmode");
	s->color2 = (uint32_t)geti(settings, "color2");
	s->grad = (float)getd(settings, "grad");
	s->blend = (int)geti(settings, "blend");
	s->transparent = getb(settings, "transparent");
	s->bgcolor = (uint32_t)geti(settings, "bgcolor");
	s->gravity = (float)getd(settings, "gravity");
	s->drag = (float)getd(settings, "drag");
	s->turb_int = (float)getd(settings, "turb_int");
	s->turb_scale = (float)getd(settings, "turb_scale");
	s->turb_speed = (float)getd(settings, "turb_speed");
	s->jitter = (float)getd(settings, "jitter");

	s->auto_idle = getb(settings, "auto_idle");
	s->idle_fade = (float)getd(settings, "idle_fade");
	s->lfo_rate = (float)getd(settings, "lfo_rate");
	s->lfo_depth = (float)getd(settings, "lfo_depth");
	s->lfo_wave = (int)geti(settings, "lfo_wave");
	s->idle_noise_speed = (float)getd(settings, "idle_noise_speed");
	s->idle_noise_amount = (float)getd(settings, "idle_noise_amount");
	s->autobeat_on = getb(settings, "autobeat");
	s->autobeat_bpm = (float)getd(settings, "autobeat_bpm");
	s->autobeat_jitter = (float)getd(settings, "autobeat_jitter");

	s->bars_count = (int)geti(settings, "bars_count");
	s->bars_ppb = (int)geti(settings, "bars_ppb");
	s->bars_idle = (int)geti(settings, "bars_idle");
	s->bars_width = (float)getd(settings, "bars_width");
	s->bars_hscale = (float)getd(settings, "bars_hscale");
	s->bars_spread = (float)getd(settings, "bars_spread");
	s->bars_boost = (float)getd(settings, "bars_boost");
	s->bars_tilt = (float)getd(settings, "bars_tilt");

	s->rain_columns = (int)geti(settings, "rain_columns");
	s->rain_trail = (int)geti(settings, "rain_trail");
	s->rain_radius = (float)getd(settings, "rain_radius");
	s->rain_fall = (float)getd(settings, "rain_fall");
	s->rain_flash = (float)getd(settings, "rain_flash");
	s->rain_depth = (float)getd(settings, "rain_depth");
	s->rain_edge = (float)getd(settings, "rain_edge");
	s->rain_rot = (float)getd(settings, "rain_rot");

	s->spec_freq = (int)geti(settings, "spec_freq");
	s->spec_hist = (int)geti(settings, "spec_hist");
	s->spec_ramp = (int)geti(settings, "spec_ramp");
	s->spec_idle = (int)geti(settings, "spec_idle");
	s->spec_hscale = (float)getd(settings, "spec_hscale");
	s->spec_scroll = (float)getd(settings, "spec_scroll");
	s->spec_angle = (float)getd(settings, "spec_angle");
	s->spec_dist = (float)getd(settings, "spec_dist");

	s->field_gx = (int)geti(settings, "field_gx");
	s->field_gy = (int)geti(settings, "field_gy");
	s->field_wsrc = (int)geti(settings, "field_wsrc");
	s->field_wcount = (int)geti(settings, "field_wcount");
	s->field_width = (float)getd(settings, "field_width");
	s->field_amp = (float)getd(settings, "field_amp");
	s->field_travel = (float)getd(settings, "field_travel");
	s->field_bokeh = (float)getd(settings, "field_bokeh");
	s->field_glow = (float)getd(settings, "field_glow");
	s->field_pulse = (float)getd(settings, "field_pulse");

	s->comet_path = (int)geti(settings, "comet_path");
	s->comet_curv = (float)getd(settings, "comet_curv");
	s->comet_speed = (float)getd(settings, "comet_speed");
	s->comet_width = (float)getd(settings, "comet_width");
	s->comet_tail = (float)getd(settings, "comet_tail");
	s->comet_spark = (float)getd(settings, "comet_spark");
	s->comet_travel = (float)getd(settings, "comet_travel");
	s->comet_swirl = (float)getd(settings, "comet_swirl");

	s->tw_count = (int)geti(settings, "tw_count");
	s->tw_speed = (float)getd(settings, "tw_speed");
	s->tw_depth = (float)getd(settings, "tw_depth");
	s->tw_drift = (float)getd(settings, "tw_drift");
	s->tw_bright = (float)getd(settings, "tw_bright");
	s->tw_sparkle = (float)getd(settings, "tw_sparkle");
	s->tw_top = (float)getd(settings, "tw_top");

	bg_unpack_color(s->common.color, s->col1);
	bg_unpack_color(s->color2, s->col2);

	/* Switching presets leaves the previous pool / history behind (twinkle's
	 * stars never expire); clear the transient state so the new look starts
	 * clean. The host seed is preserved by passing 0 to the pool reset. */
	if (s->last_preset >= 0 && s->last_preset != s->preset) {
		bg_particles_reset(s->sys, 0);
		s->seeded_twinkle = false;
		s->seeded_field = false;
		s->hist_head = 0;
		s->hist_len = 0;
		s->scroll_acc = 0.0f;
	}
	s->last_preset = s->preset;
}

static void audioviz_reset(void *data, uint32_t seed)
{
	struct audioviz_state *s = data;
	bg_particles_reset(s->sys, seed);
	s->clock = 0.0f;
	s->noise_seed = (float)((seed ? seed : 1u) & 0xFFFFu) * 0.0137f;
	s->idle_blend = 1.0f;
	s->idle_beat = 0.0f;
	s->beat_acc = 0.0f;
	s->next_beat = 0.6f;
	s->peak_audio = 0.0f;
	s->hist_head = 0;
	s->hist_len = 0;
	s->scroll_acc = 0.0f;
	s->seeded_field = false;
	s->seeded_twinkle = false;
	for (int i = 0; i < HIST_CAP; ++i)
		for (int j = 0; j < FREQ_CAP; ++j)
			s->hist[i][j] = 0.0f;
}

/* ---- drive-signal layer (§1, §2.6, §12) ---------------------------------- */

/* Seed-generator LFO value in 0..1 at the current clock. */
static float seed_lfo(const struct audioviz_state *s)
{
	float ph = s->clock * s->lfo_rate; /* cycles */
	float p = fract1(ph);
	float w;
	switch (s->lfo_wave) {
	case LFO_TRI:
		w = p < 0.5f ? p * 2.0f : 2.0f - p * 2.0f;
		break;
	case LFO_SAW:
		w = p;
		break;
	case LFO_SNOISE:
		w = fbm1(s->clock * s->lfo_rate * 2.0f + s->noise_seed);
		break;
	case LFO_SINE:
	default:
		w = 0.5f + 0.5f * sinf(ph * BG_TAU);
		break;
	}
	return clamp01f(w);
}

/* Compute this frame's drive signals into s->dr (cross-fading audio↔seed). */
static void compute_drive(struct audioviz_state *s, const struct bg_ctx *ctx,
			  float dt)
{
	const struct bg_audio_fft *fft = ctx->fft;
	bool have_audio = ctx->audio.enabled && fft && fft->valid;
	bool audio_alive = have_audio && fft->level > 0.02f;

	/* Decide how far toward the Seed generator we should be. */
	float target;
	if (!ctx->audio.enabled)
		target = 1.0f;                 /* no source at all → seed     */
	else if (!s->auto_idle)
		target = 0.0f;                 /* always trust the audio path */
	else
		target = audio_alive ? 0.0f : 1.0f; /* fade out on silence    */

	float rate = (s->idle_fade > 1e-3f) ? dt / s->idle_fade : 1.0f;
	if (rate > 1.0f)
		rate = 1.0f;
	s->idle_blend += (target - s->idle_blend) * rate;

	/* ---- audio feature ---- */
	float a_rms = 0.0f, a_b[3] = {0, 0, 0}, a_beat = 0.0f, a_peak = 0.0f;
	bool a_trig = false;
	float a_spec[SPEC_BINS];
	for (int i = 0; i < SPEC_BINS; ++i)
		a_spec[i] = 0.0f;
	if (have_audio) {
		a_rms = fft->level;
		a_b[0] = fft->bass;
		a_b[1] = fft->mid;
		a_b[2] = fft->treble;
		a_beat = fft->beat;
		a_trig = fft->beat_trigger;
		for (int i = 0; i < SPEC_BINS && i < fft->bar_count; ++i)
			a_spec[i] = fft->bars[i];
		/* short-time peak follower: fast up, slow down */
		if (a_rms > s->peak_audio)
			s->peak_audio = a_rms;
		else
			s->peak_audio -= dt * 1.5f;
		if (s->peak_audio < 0.0f)
			s->peak_audio = 0.0f;
		a_peak = s->peak_audio;
	}

	/* ---- seed feature (§2.6) ---- */
	float i_rms = s->lfo_depth * seed_lfo(s);
	float ns = s->idle_noise_speed, na = s->idle_noise_amount;
	float i_b[3];
	i_b[0] = na * fbm1(s->clock * ns + s->noise_seed + 11.0f);
	i_b[1] = na * fbm1(s->clock * ns * 1.3f + s->noise_seed + 53.0f);
	i_b[2] = na * fbm1(s->clock * ns * 1.7f + s->noise_seed + 97.0f);
	float i_spec[SPEC_BINS];
	for (int i = 0; i < SPEC_BINS; ++i) {
		float fi = (float)i / SPEC_BINS;
		float oneOverF = powf(1.0f - fi, 1.4f) + 0.08f; /* low emphasis */
		float prof = fbm1(fi * 6.0f - s->clock * ns + s->noise_seed);
		i_spec[i] = clamp01f(na * oneOverF * (0.4f + 1.3f * prof));
	}

	/* auto-beat (§2.6): jittered periodic pulse */
	s->idle_beat_trig = false;
	if (s->autobeat_on && s->autobeat_bpm > 1.0f) {
		s->beat_acc += dt;
		if (s->beat_acc >= s->next_beat) {
			s->beat_acc = 0.0f;
			s->idle_beat = 1.0f;
			s->idle_beat_trig = true;
			float base = 60.0f / s->autobeat_bpm;
			float j = (vnoise1(s->clock * 3.3f + s->noise_seed) -
				   0.5f) * 2.0f;
			s->next_beat = base * (1.0f + s->autobeat_jitter * j);
			if (s->next_beat < 0.05f)
				s->next_beat = 0.05f;
		} else {
			s->idle_beat *= expf(-dt / 0.12f);
		}
	}
	float i_peak = clamp01f(i_rms + 0.5f * na *
				vnoise1(s->clock * 4.0f + s->noise_seed + 7.0f));

	/* ---- blend the two supplies (§12) ---- */
	float k = clamp01f(s->idle_blend);
	struct viz_drive *d = &s->dr;
	d->rms = lerpf(a_rms, i_rms, k);
	for (int i = 0; i < 3; ++i)
		d->band[i] = lerpf(a_b[i], i_b[i], k);
	for (int i = 0; i < SPEC_BINS; ++i)
		d->spectrum[i] = lerpf(a_spec[i], i_spec[i], k);
	d->beat = lerpf(a_beat, s->idle_beat, k);
	d->beat_trigger = (k < 0.5f) ? a_trig : s->idle_beat_trig;
	d->peak = lerpf(a_peak, i_peak, k);
}

/* Resample the drive spectrum to `n` bars (n ≤ SPEC_BINS handled by averaging,
 * n > SPEC_BINS by interpolation). */
static float drive_bar(const struct viz_drive *d, int b, int n)
{
	if (n <= 1)
		return d->spectrum[0];
	float pos = (float)b / (float)(n - 1) * (float)(SPEC_BINS - 1);
	int i0 = (int)pos;
	int i1 = i0 + 1 < SPEC_BINS ? i0 + 1 : SPEC_BINS - 1;
	return lerpf(d->spectrum[i0], d->spectrum[i1], pos - i0);
}

/* ---- preset: spectrum particle bars (§3) --------------------------------- */

static size_t live_cap(const struct audioviz_state *s)
{
	int m = s->common.max_count;
	if (m > (int)s->sys->capacity)
		m = (int)s->sys->capacity;
	return (size_t)(m < 0 ? 0 : m);
}

static void tick_bars(struct audioviz_state *s, const struct bg_ctx *ctx,
		      float dt)
{
	struct bg_particle_system *sys = s->sys;
	float w = (float)ctx->width, h = (float)ctx->height;
	int N = s->bars_count;
	if (N < 1)
		N = 1;
	float slot = w / (float)N;
	size_t cap = live_cap(s);
	const struct viz_drive *d = &s->dr;
	float react = s->reactivity;

	/* Idle Pattern=Flat → sink everything (near-silent standby). */
	bool flat = (s->idle_blend > 0.5f && s->bars_idle == 1);
	float boost = d->beat_trigger ? s->bars_boost : 0.0f;

	for (int b = 0; b < N && sys->live < cap; ++b) {
		float v = drive_bar(d, b, N);
		/* Sweep idle pattern: a travelling bump. */
		if (s->idle_blend > 0.5f && s->bars_idle == 2) {
			float c = 0.5f + 0.5f * sinf(s->clock * 0.6f);
			float dx = (float)b / N - c;
			v = clamp01f(expf(-dx * dx * 40.0f));
		}
		if (flat)
			v = 0.0f;
		/* High-end tilt correction. */
		v *= 1.0f + s->bars_tilt * ((float)b / N - 0.5f) * 2.0f;
		v = clamp01f(v + boost * 0.5f);

		/* Emit ~ppb*v particles per second per bar. */
		float want = (float)s->bars_ppb * (0.15f + v) * react * dt * 4.0f;
		int emit = (int)want;
		if (bg_frand(sys) < (want - emit))
			emit++;
		float cx = b * slot + slot * 0.5f;
		float halfw = slot * s->bars_width * 0.5f;
		for (int e = 0; e < emit && sys->live < cap; ++e) {
			bg_particle_t *p = bg_particles_spawn(sys);
			if (!p)
				break;
			p->x = cx + bg_frand_range(sys, -1.0f, 1.0f) * halfw *
					    (1.0f + s->bars_spread);
			p->y = h;
			float spd = (0.4f + v + d->peak) * s->bars_hscale *
				    (h * 0.45f) * bg_frand_range(sys, 0.7f, 1.0f);
			p->vy = -(spd + boost * h * 0.3f);
			p->vx = bg_frand_range(sys, -1.0f, 1.0f) * 20.0f *
				s->bars_spread;
			p->size = bg_vary(sys, s->common.size, s->common.size_var) *
				  0.5f;
			p->max_life = p->life = bg_vary(sys, s->common.lifetime,
							s->common.life_var);
			p->seed = bg_frand(sys);
			p->a = s->common.alpha * bg_frand_range(sys, 0.7f, 1.0f);
			float rgb[3];
			av_color(s, (float)b / N, rgb);
			p->r = rgb[0];
			p->g = rgb[1];
			p->b = rgb[2];
		}
	}

	/* Integrate: rise, gravity pull-back, drag, jitter. */
	float g = s->gravity + 600.0f; /* a gentle baseline so they arc down */
	float drag = 1.0f - s->drag * dt;
	if (drag < 0.0f)
		drag = 0.0f;
	for (size_t i = 0; i < sys->live; ++i) {
		bg_particle_t *p = &sys->items[i];
		p->vy += g * dt;
		p->vx *= drag;
		p->x += p->vx * dt +
			(bg_frand(sys) - 0.5f) * s->jitter * 6.0f;
		p->y += p->vy * dt;
		if (p->y > h + 40.0f)
			p->life = 0.0f;
		else
			p->life -= dt;
	}
	bg_particles_compact(sys);
}

/* ---- preset: digital-rain sphere (§4) ------------------------------------ */

static void tick_rain(struct audioviz_state *s, const struct bg_ctx *ctx,
		      float dt)
{
	struct bg_particle_system *sys = s->sys;
	float w = (float)ctx->width, h = (float)ctx->height;
	float cx = w * 0.5f, cy = h * 0.5f;
	float R = s->rain_radius * 0.5f * fminf(w, h);
	size_t cap = live_cap(s);
	const struct viz_drive *d = &s->dr;

	float speed = s->rain_fall * (1.0f + 0.8f * s->reactivity * d->rms);
	int cols = s->rain_columns;
	if (cols < 1)
		cols = 1;

	/* Continuous emission: columns spawning glyph-dashes near the top. */
	float want = (float)cols * speed * 1.6f * dt;
	int emit = (int)want;
	if (bg_frand(sys) < (want - emit))
		emit++;
	for (int e = 0; e < emit && sys->live < cap; ++e) {
		bg_particle_t *p = bg_particles_spawn(sys);
		if (!p)
			break;
		/* Sample a point on the sphere; project orthographically. */
		float u = bg_frand(sys) * BG_TAU;
		float vv = bg_frand(sys) * 2.0f - 1.0f; /* cos(theta) */
		float sr = sqrtf(1.0f - vv * vv);
		float sx = sr * cosf(u);
		float sz = sr * sinf(u); /* depth */
		p->x = cx + sx * R;
		p->y = cy + vv * R - R; /* start high on the column */
		p->aux0 = sz;           /* depth -1..1 (front +)     */
		p->aux1 = cx + sx * R;  /* keep the column x          */
		p->vy = speed * 60.0f * bg_frand_range(sys, 0.7f, 1.4f);
		p->size = bg_vary(sys, s->common.size, s->common.size_var) * 0.5f;
		p->len = p->size * (float)s->rain_trail * 0.25f; /* dash tail */
		p->max_life = p->life = bg_vary(sys, s->common.lifetime,
						s->common.life_var);
		p->seed = bg_frand(sys);
		float depth_b = 0.5f + 0.5f * sz;
		float br = lerpf(1.0f - s->rain_depth, 1.0f, depth_b);
		p->a = s->common.alpha * br * bg_frand_range(sys, 0.6f, 1.0f);
		/* green/cyan rain, occasional white flash. */
		bool flash = bg_frand(sys) < s->rain_flash ||
			     (d->beat_trigger && bg_frand(sys) < 0.3f);
		float rgb[3];
		av_color(s, depth_b, rgb);
		if (s->colmode == COL_SINGLE) {
			rgb[0] = 0.2f * br;
			rgb[1] = 1.0f * br;
			rgb[2] = 0.6f * br;
		}
		if (flash) {
			rgb[0] = rgb[1] = rgb[2] = 1.0f;
			p->a = clamp01f(p->a * 2.0f);
		}
		p->r = rgb[0];
		p->g = rgb[1];
		p->b = rgb[2];
		p->vrot = 0.0f;
		p->rot = BG_TAU * 0.25f; /* vertical dashes */
	}

	/* Fall straight down; slow self-rotation of the whole sphere is faked by
	 * advecting x toward the centre column (Idle Rotation). */
	float rot = s->rain_rot * 30.0f * dt;
	for (size_t i = 0; i < sys->live; ++i) {
		bg_particle_t *p = &sys->items[i];
		p->y += p->vy * dt;
		if (rot != 0.0f) {
			float dx = p->x - cx;
			float dy = (p->aux0) * R; /* pseudo z for the spin */
			float a = rot * (BG_TAU / 360.0f);
			float nx = dx * cosf(a) - dy * sinf(a);
			p->x = cx + nx;
		}
		if (p->y > h + 60.0f)
			p->life = 0.0f;
		else
			p->life -= dt;
	}
	bg_particles_compact(sys);
}

/* ---- preset: turbulence comet (§7) --------------------------------------- */

static void comet_emit_point(struct audioviz_state *s, const struct bg_ctx *ctx,
			     float *ox, float *oy)
{
	float w = (float)ctx->width, h = (float)ctx->height;
	float cx = w * 0.5f, cy = h * 0.5f;
	float t = s->clock * 0.2f;
	if (s->comet_path == 1) { /* line */
		*ox = w * 0.15f;
		*oy = cy + (bg_frand(s->sys) - 0.5f) * h * 0.1f *
				    s->comet_width;
	} else if (s->comet_path == 2) { /* spiral */
		float a = t * 2.0f;
		float r = 40.0f + 30.0f * a;
		r = fmodf(r, fminf(w, h) * 0.45f);
		*ox = cx + r * cosf(a);
		*oy = cy + r * sinf(a);
	} else { /* curve */
		float a = t;
		*ox = cx + sinf(a) * w * 0.3f * s->comet_curv +
		      (w * 0.5f - cx);
		*oy = cy + sinf(a * 1.7f) * h * 0.25f * s->comet_curv;
	}
}

static void tick_comet(struct audioviz_state *s, const struct bg_ctx *ctx,
		       float dt)
{
	struct bg_particle_system *sys = s->sys;
	float w = (float)ctx->width, h = (float)ctx->height;
	size_t cap = live_cap(s);
	const struct viz_drive *d = &s->dr;

	float loud = s->reactivity * (d->rms + d->band[1]);
	float flow = s->comet_speed * (1.0f + 0.6f * loud);

	/* Continuous emission along the path head. */
	float want = s->common.rate * (0.4f + 0.6f * (d->rms + 0.2f)) * dt;
	int emit = (int)want;
	if (bg_frand(sys) < (want - emit))
		emit++;
	float ox, oy;
	comet_emit_point(s, ctx, &ox, &oy);
	for (int e = 0; e < emit && sys->live < cap; ++e) {
		bg_particle_t *p = bg_particles_spawn(sys);
		if (!p)
			break;
		p->x = ox + bg_frand_range(sys, -1.0f, 1.0f) * 40.0f *
				    s->comet_width;
		p->y = oy + bg_frand_range(sys, -1.0f, 1.0f) * 40.0f *
				    s->comet_width;
		p->vx = flow * 80.0f;
		p->vy = bg_frand_range(sys, -10.0f, 10.0f);
		p->size = bg_vary(sys, s->common.size, s->common.size_var) * 0.5f;
		p->max_life = p->life = bg_vary(sys, s->common.lifetime,
						s->common.life_var) *
					s->comet_tail;
		p->seed = bg_frand(sys);
		p->a = s->common.alpha * bg_frand_range(sys, 0.6f, 1.0f);
	}

	/* Beat / auto-beat → a spark burst from the head. */
	if (d->beat_trigger && s->comet_spark > 0.0f) {
		int n = (int)(s->comet_spark * 40.0f * (0.5f + d->band[0]));
		for (int i = 0; i < n && sys->live < cap; ++i) {
			bg_particle_t *p = bg_particles_spawn(sys);
			if (!p)
				break;
			float a = bg_frand(sys) * BG_TAU;
			float v = bg_frand_range(sys, 80.0f, 320.0f) *
				  s->comet_spark;
			p->x = ox;
			p->y = oy;
			p->vx = cosf(a) * v;
			p->vy = sinf(a) * v;
			p->size = bg_vary(sys, s->common.size,
					  s->common.size_var) * 0.4f;
			p->max_life = p->life =
				bg_vary(sys, s->common.lifetime,
					s->common.life_var) * 0.5f;
			p->seed = bg_frand(sys);
			p->a = s->common.alpha;
		}
	}

	/* Curl-noise advection + colour-by-age. */
	float scale = 0.0025f * s->turb_scale + 0.0006f;
	float tflow = s->clock * (0.3f + s->turb_speed) +
		      s->comet_swirl * loud;
	float drag = 1.0f - s->drag * dt;
	if (drag < 0.0f)
		drag = 0.0f;
	const float margin = 250.0f;
	for (size_t i = 0; i < sys->live; ++i) {
		bg_particle_t *p = &sys->items[i];
		float cvx, cvy;
		curl2(p->x, p->y, tflow, scale, &cvx, &cvy);
		float ti = (s->turb_int + 0.3f) *
			   (1.0f + s->comet_swirl * loud) * 120.0f;
		float a = 5.0f * dt;
		if (a > 1.0f)
			a = 1.0f;
		p->vx += (cvx * ti + flow * 30.0f - p->vx) * a;
		p->vy += (cvy * ti + s->gravity - p->vy) * a;
		p->vx *= drag;
		p->x += p->vx * dt;
		p->y += p->vy * dt;
		p->len = fmaxf(p->size,
			       sqrtf(p->vx * p->vx + p->vy * p->vy) * 0.02f *
				       (0.5f + s->comet_tail));
		float age = p->max_life > 0 ? 1.0f - p->life / p->max_life : 0;
		float rgb[3];
		av_color(s, age * s->comet_travel, rgb);
		p->r = rgb[0];
		p->g = rgb[1];
		p->b = rgb[2];
		if (p->x < -margin || p->x > w + margin || p->y < -margin ||
		    p->y > h + margin)
			p->life = 0.0f;
		else
			p->life -= dt;
	}
	bg_particles_compact(sys);
}

/* ---- preset: ambient twinkle (§8) ---------------------------------------- */

static void tick_twinkle(struct audioviz_state *s, const struct bg_ctx *ctx,
			 float dt)
{
	struct bg_particle_system *sys = s->sys;
	float w = (float)ctx->width, h = (float)ctx->height;
	const struct viz_drive *d = &s->dr;
	int n = s->tw_count;
	UNUSED_PARAMETER(dt);
	if (n > (int)live_cap(s))
		n = (int)live_cap(s);

	/* (Re)lay the star field on first run / when the count or canvas moved. */
	if (!s->seeded_twinkle || (int)sys->live != n ||
	    s->twinkle_seed_w != w || s->twinkle_seed_h != h) {
		bg_particles_reset(sys, 0);
		for (int i = 0; i < n; ++i) {
			bg_particle_t *p = bg_particles_spawn(sys);
			if (!p)
				break;
			p->x = bg_frand(sys) * w;
			p->y = bg_frand(sys) * h;
			p->aux0 = p->x; /* home position */
			p->aux1 = p->y;
			p->size = bg_vary(sys, s->common.size,
					  s->common.size_var) * 0.5f;
			p->seed = bg_frand(sys);
			/* Persistent stars: keep them parked in the lifetime
			 * "hold" zone so bg_particles_render's fade-in/out
			 * envelope leaves them fully visible (age must clear
			 * fade_in*max_life and life must stay above
			 * fade_out*max_life). */
			p->max_life = 1e9f;
			p->life = 1e9f * 0.5f;
			p->a = s->common.alpha;
			float rgb[3];
			/* teal/cyan base, slight per-star hue spread */
			av_color(s, 0.25f + 0.5f * p->seed, rgb);
			p->r = rgb[0];
			p->g = rgb[1];
			p->b = rgb[2];
		}
		s->seeded_twinkle = true;
		s->twinkle_seed_w = w;
		s->twinkle_seed_h = h;
	}

	float allbright = 1.0f + s->tw_bright * s->reactivity * d->rms;
	for (size_t i = 0; i < sys->live; ++i) {
		bg_particle_t *p = &sys->items[i];
		/* slow individual-phase twinkle */
		float ph = s->clock * s->tw_speed + p->seed * BG_TAU * 5.0f;
		float tw = 1.0f - s->tw_depth * (0.5f + 0.5f * sinf(ph));
		/* beat / auto-beat: a small fraction flares hard (step trick) */
		float spark = 1.0f;
		if (d->beat_trigger &&
		    hash1(p->seed * 91.7f + floorf(s->clock * 4.0f)) >
			    1.0f - s->tw_sparkle)
			spark = 3.0f;
		/* top light: brighter near the top edge */
		float topf = 1.0f + s->tw_top * (1.0f - p->aux1 / h);
		p->a = clamp01f(s->common.alpha * tw * allbright * spark * topf);
		/* gentle drift around home */
		float dr = s->tw_drift * 30.0f;
		p->x = p->aux0 + dr * sinf(s->clock * 0.3f + p->seed * 13.0f);
		p->y = p->aux1 + dr * cosf(s->clock * 0.27f + p->seed * 7.0f);
	}
	/* persistent: no compaction needed */
}

/* ---- preset: spectrogram waterfall (§5, geometry) ------------------------ */

static void tick_spectro(struct audioviz_state *s, const struct bg_ctx *ctx,
			 float dt)
{
	UNUSED_PARAMETER(ctx);
	const struct viz_drive *d = &s->dr;

	/* Freeze idle: stop pushing new rows, just keep scrolling existing. */
	bool freeze = (s->idle_blend > 0.5f && s->spec_idle == 1);

	/* Rows are always stored at full FREQ_CAP width (the whole spectrum);
	 * the chosen Freq Resolution only resamples the columns at draw time. */
	s->scroll_acc += s->spec_scroll * dt * 30.0f;
	while (s->scroll_acc >= 1.0f) {
		s->scroll_acc -= 1.0f;
		int prev = s->hist_head;
		s->hist_head = (s->hist_head + 1) % HIST_CAP;
		float *row = s->hist[s->hist_head];
		for (int j = 0; j < FREQ_CAP; ++j)
			row[j] = freeze ? s->hist[prev][j]
					: drive_bar(d, j, FREQ_CAP);
		if (s->hist_len < HIST_CAP)
			s->hist_len++;
	}
}

/* ---- preset: waveform field (§6, geometry) ------------------------------- */

static float field_disp(struct audioviz_state *s, const struct bg_ctx *ctx,
			float u)
{
	/* u in 0..1 across the band. Returns -1..1 displacement. */
	const struct viz_drive *d = &s->dr;
	const struct bg_audio_fft *fft = ctx->fft;
	float k = clamp01f(s->idle_blend);

	float audiov = 0.0f;
	if (s->field_wsrc == 0 && fft && fft->valid) { /* waveform */
		int n = fft->wave_count;
		float fp = u * (n - 1);
		int i0 = (int)fp;
		int i1 = i0 + 1 < n ? i0 + 1 : n - 1;
		audiov = lerpf(fft->wave[i0], fft->wave[i1], fp - i0);
	} else if (s->field_wsrc == 1) { /* spectrum */
		audiov = drive_bar(d, (int)(u * (SPEC_BINS - 1)), SPEC_BINS) *
			 2.0f - 1.0f;
	}

	/* Procedural: superposed travelling sines + a little noise. */
	float proc = 0.0f;
	int wc = s->field_wcount < 1 ? 1 : s->field_wcount;
	for (int i = 0; i < wc; ++i) {
		float fr = 1.5f + i * 1.7f;
		float ph = s->clock * s->field_travel * (0.6f + 0.3f * i);
		proc += sinf((u * fr + ph) * BG_TAU) / (i + 1.0f);
	}
	proc += (vnoise1(u * 6.0f + s->clock * s->field_travel) - 0.5f) * 0.6f;
	proc *= 0.5f;

	if (s->field_wsrc == 2)
		return proc;
	return lerpf(audiov, proc, k); /* audio source fades to procedural */
}

/* ---- tick dispatch ------------------------------------------------------- */

static void audioviz_tick(void *data, const struct bg_ctx *ctx, float dt)
{
	struct audioviz_state *s = data;
	dt *= s->time_scale;
	if (dt > 0.1f)
		dt = 0.1f; /* clamp huge frame gaps */
	s->clock += dt;
	s->sys->clock += dt;

	compute_drive(s, ctx, dt);

	switch (s->preset) {
	case AV_BARS:
		tick_bars(s, ctx, dt);
		break;
	case AV_RAIN:
		tick_rain(s, ctx, dt);
		break;
	case AV_SPECTRO:
		tick_spectro(s, ctx, dt);
		break;
	case AV_FIELD:
		/* field is drawn directly from the drive each render */
		break;
	case AV_COMET:
		tick_comet(s, ctx, dt);
		break;
	case AV_TWINKLE:
		tick_twinkle(s, ctx, dt);
		break;
	}
}

/* ---- rendering ----------------------------------------------------------- */

static void draw_bg(struct audioviz_state *s, const struct bg_ctx *ctx)
{
	if (s->transparent || !s->viz)
		return;
	float w = (float)ctx->width, h = (float)ctx->height;
	float rgb[3];
	bg_unpack_color(s->bgcolor, rgb); /* rgba; rgb only */
	uint32_t col = pack_rgba(rgb[0], rgb[1], rgb[2], 1.0f);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	while (gs_effect_loop(s->viz, "Draw")) {
		gs_render_start(true);
		quad(col, 0, 0, w, 0, w, h, 0, h);
		gs_render_stop(GS_TRIS);
	}
	gs_blend_state_pop();
}

static void render_spectro(struct audioviz_state *s, const struct bg_ctx *ctx)
{
	if (!s->viz || s->hist_len < 1)
		return;
	float w = (float)ctx->width, h = (float)ctx->height;
	float cx = w * 0.5f, baseY = h * 0.82f;
	int F = s->spec_freq;
	if (F > FREQ_CAP)
		F = FREQ_CAP;
	if (F < 2)
		F = 2;
	int R = s->spec_hist;
	if (R > s->hist_len)
		R = s->hist_len;
	if (R > HIST_CAP)
		R = HIST_CAP;
	float ang = s->spec_angle * (BG_TAU / 360.0f);
	float depthSpan = h * 0.5f * sinf(ang);

	push_blend(s->blend);
	while (gs_effect_loop(s->viz, "Draw")) {
		gs_render_start(true);
		/* back to front so nearer ridges overdraw farther ones */
		for (int r = R - 1; r >= 0; --r) {
			float depth = (float)r / (float)R;
			int row = (s->hist_head - r + HIST_CAP * 2) % HIST_CAP;
			float *vals = s->hist[row];
			float persp = 1.0f - 0.35f * depth;
			float yoff = baseY - depth * depthSpan;
			float fade = 1.0f - s->spec_dist * depth;
			float cell = (w * 0.8f * persp) / (float)(F - 1);
			for (int j = 0; j < F; ++j) {
				/* resample the full-width row to F columns */
				float fp = (float)j / (float)(F - 1) *
					   (float)(FREQ_CAP - 1);
				int c0 = (int)fp;
				int c1 = c0 + 1 < FREQ_CAP ? c0 + 1
							   : FREQ_CAP - 1;
				float v = lerpf(vals[c0], vals[c1], fp - c0);
				float un = (float)j / (float)(F - 1) - 0.5f;
				float x = cx + un * w * 0.8f * persp;
				float ht = v * s->spec_hscale * h * 0.4f;
				float y = yoff - ht;
				float rgb[3];
				av_ramp(s, v, rgb);
				float inten = s->common.alpha * fade *
					      (0.25f + 0.9f * v);
				uint32_t col = premul(rgb, inten);
				float hw = cell * 0.45f + 1.0f;
				/* a small vertical ridge bar per cell */
				quad(col, x - hw, yoff, x + hw, yoff, x + hw, y,
				     x - hw, y);
			}
		}
		gs_render_stop(GS_TRIS);
	}
	gs_blend_state_pop();
}

static void render_field(struct audioviz_state *s, const struct bg_ctx *ctx)
{
	if (!s->viz)
		return;
	float w = (float)ctx->width, h = (float)ctx->height;
	float cx = w * 0.5f, cy = h * 0.5f;
	float bandw = w * s->field_width;
	int GX = s->field_gx;
	if (GX < 2)
		GX = 2;
	if (GX > 500)
		GX = 500;
	int GY = s->field_gy;
	if (GY < 1)
		GY = 1;
	if (GY > 20)
		GY = 20;
	const struct viz_drive *d = &s->dr;
	float amp = s->field_amp * h * 0.25f;
	float pulse = 1.0f + s->field_pulse * s->reactivity *
				     (d->rms + (d->beat_trigger ? 0.5f : 0.0f));
	float thick = (h * 0.01f + s->common.size) * (1.0f + s->field_bokeh * 2.0f);

	push_blend(s->blend);
	while (gs_effect_loop(s->viz, "Draw")) {
		gs_render_start(true);
		for (int gx = 0; gx < GX; ++gx) {
			float u = (float)gx / (float)(GX - 1);
			float x = cx + (u - 0.5f) * bandw;
			float disp = field_disp(s, ctx, u) * amp * pulse;
			for (int gy = 0; gy < GY; ++gy) {
				float layer = GY > 1 ? (float)gy / (GY - 1) - 0.5f
						     : 0.0f;
				float y = cy + disp + layer * thick * 2.5f;
				float mag = clamp01f(0.3f + 0.7f *
							fabsf(disp) / (amp + 1.0f));
				float rgb[3];
				av_color(s, 0.5f + disp / (amp * 2.0f + 1.0f),
					 rgb);
				/* central glow boost */
				float gl = 1.0f + s->field_glow *
						  expf(-fabsf(u - 0.5f) * 6.0f);
				uint32_t col = premul(rgb, s->common.alpha * mag *
							    gl *
							    (1.0f - 0.4f *
								    fabsf(layer) *
								    2.0f));
				float r = thick * (1.0f - 0.3f * fabsf(layer) *
							   2.0f);
				quad(col, x - r, y - r, x + r, y - r, x + r,
				     y + r, x - r, y + r);
			}
		}
		gs_render_stop(GS_TRIS);
	}
	gs_blend_state_pop();
}

static void audioviz_render(void *data, const struct bg_ctx *ctx)
{
	struct audioviz_state *s = data;

	draw_bg(s, ctx);

	switch (s->preset) {
	case AV_SPECTRO:
		render_spectro(s, ctx);
		break;
	case AV_FIELD:
		render_field(s, ctx);
		break;
	default: {
		if (!s->sprite)
			break;
		int shape = (s->preset == AV_TWINKLE) ? BG_SHAPE_STAR
						      : BG_SHAPE_SOFT;
		push_blend(s->blend);
		bg_particles_render(s->sys, s->sprite, shape, &s->post, NULL);
		gs_blend_state_pop();
		break;
	}
	}
}

/* ---- properties ---------------------------------------------------------- */

#define ADDF(name, key, lo, hi, step)                                          \
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, name),    \
					obs_module_text(key), lo, hi, step)
#define ADDI(name, key, lo, hi, step)                                          \
	obs_properties_add_int_slider(g, bg_key(k, sizeof(k), PRE, name),      \
				      obs_module_text(key), lo, hi, step)
#define ADDB(name, key)                                                        \
	obs_properties_add_bool(g, bg_key(k, sizeof(k), PRE, name),            \
				obs_module_text(key))
#define ADDC(name, key)                                                        \
	obs_properties_add_color(g, bg_key(k, sizeof(k), PRE, name),           \
				 obs_module_text(key))

/* Show only the active preset's own parameters. */
static void av_apply_vis(obs_properties_t *props, int preset)
{
	char k[96];
	obs_property_t *p;
#define VIS(name, cond)                                                        \
	if ((p = obs_properties_get(props, bg_key(k, sizeof(k), PRE, name))))  \
	obs_property_set_visible(p, (cond))
	bool bars = preset == AV_BARS, rain = preset == AV_RAIN;
	bool spec = preset == AV_SPECTRO, field = preset == AV_FIELD;
	bool comet = preset == AV_COMET, tw = preset == AV_TWINKLE;

	VIS("bars_count", bars); VIS("bars_ppb", bars); VIS("bars_width", bars);
	VIS("bars_hscale", bars); VIS("bars_spread", bars);
	VIS("bars_boost", bars); VIS("bars_tilt", bars); VIS("bars_idle", bars);

	VIS("rain_radius", rain); VIS("rain_columns", rain);
	VIS("rain_fall", rain); VIS("rain_trail", rain); VIS("rain_flash", rain);
	VIS("rain_depth", rain); VIS("rain_edge", rain); VIS("rain_rot", rain);

	VIS("spec_freq", spec); VIS("spec_hist", spec); VIS("spec_hscale", spec);
	VIS("spec_scroll", spec); VIS("spec_angle", spec); VIS("spec_ramp", spec);
	VIS("spec_dist", spec); VIS("spec_idle", spec);

	VIS("field_width", field); VIS("field_gx", field); VIS("field_gy", field);
	VIS("field_amp", field); VIS("field_wsrc", field);
	VIS("field_wcount", field); VIS("field_travel", field);
	VIS("field_bokeh", field); VIS("field_glow", field);
	VIS("field_pulse", field);

	VIS("comet_path", comet); VIS("comet_curv", comet);
	VIS("comet_speed", comet); VIS("comet_width", comet);
	VIS("comet_tail", comet); VIS("comet_spark", comet);
	VIS("comet_travel", comet); VIS("comet_swirl", comet);

	VIS("tw_count", tw); VIS("tw_speed", tw); VIS("tw_depth", tw);
	VIS("tw_drift", tw); VIS("tw_bright", tw); VIS("tw_sparkle", tw);
	VIS("tw_top", tw);
#undef VIS
}

static bool on_preset_changed(void *priv, obs_properties_t *props,
			      obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(priv);
	UNUSED_PARAMETER(prop);
	char k[96];
	av_apply_vis(props, (int)obs_data_get_int(settings,
		bg_key(k, sizeof(k), PRE, "preset")));
	return true;
}

static bool on_transparent_changed(void *priv, obs_properties_t *props,
				   obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(priv);
	UNUSED_PARAMETER(prop);
	char k[96];
	obs_property_t *p = obs_properties_get(props,
		bg_key(k, sizeof(k), PRE, "bgcolor"));
	if (p)
		obs_property_set_visible(p, !obs_data_get_bool(settings,
			bg_key(k, sizeof(k), PRE, "transparent")));
	return true;
}

static void audioviz_properties(obs_properties_t *g, obs_data_t *settings)
{
	char k[96];

	/* Preset selector leads the panel. */
	obs_property_t *pre = obs_properties_add_list(g,
		bg_key(k, sizeof(k), PRE, "preset"),
		obs_module_text("AVPreset"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(pre, obs_module_text("AVPresetBars"), AV_BARS);
	obs_property_list_add_int(pre, obs_module_text("AVPresetRain"), AV_RAIN);
	obs_property_list_add_int(pre, obs_module_text("AVPresetSpectro"),
				  AV_SPECTRO);
	obs_property_list_add_int(pre, obs_module_text("AVPresetField"),
				  AV_FIELD);
	obs_property_list_add_int(pre, obs_module_text("AVPresetComet"),
				  AV_COMET);
	obs_property_list_add_int(pre, obs_module_text("AVPresetTwinkle"),
				  AV_TWINKLE);
	obs_property_set_modified_callback2(pre, on_preset_changed, NULL);

	/* ---- per-preset params (grouped, only active one shown) ---- */
	ADDI("bars_count", "AVBarCount", 8, 256, 1);
	ADDI("bars_ppb", "AVBarPPB", 5, 200, 1);
	ADDF("bars_width", "AVBarWidth", 0.1, 1.0, 0.01);
	ADDF("bars_hscale", "AVBarHeight", 0.1, 4.0, 0.05);
	ADDF("bars_spread", "AVBarSpread", 0.0, 1.0, 0.01);
	ADDF("bars_boost", "AVBarBoost", 0.0, 2.0, 0.05);
	ADDF("bars_tilt", "AVBarTilt", -1.0, 1.0, 0.05);
	obs_property_t *bi = obs_properties_add_list(g,
		bg_key(k, sizeof(k), PRE, "bars_idle"),
		obs_module_text("AVBarIdle"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(bi, obs_module_text("AVIdleNoise"), 0);
	obs_property_list_add_int(bi, obs_module_text("AVIdleFlat"), 1);
	obs_property_list_add_int(bi, obs_module_text("AVIdleSweep"), 2);

	ADDF("rain_radius", "AVRainRadius", 0.1, 1.0, 0.01);
	ADDI("rain_columns", "AVRainColumns", 20, 500, 1);
	ADDF("rain_fall", "AVRainFall", 0.1, 4.0, 0.05);
	ADDI("rain_trail", "AVRainTrail", 3, 40, 1);
	ADDF("rain_flash", "AVRainFlash", 0.0, 0.5, 0.01);
	ADDF("rain_depth", "AVRainDepth", 0.0, 1.0, 0.01);
	ADDF("rain_edge", "AVRainEdge", 0.0, 2.0, 0.05);
	ADDF("rain_rot", "AVRainRot", -1.0, 1.0, 0.01);

	ADDI("spec_freq", "AVSpecFreq", 16, 512, 1);
	ADDI("spec_hist", "AVSpecHist", 10, 300, 1);
	ADDF("spec_hscale", "AVSpecHeight", 0.1, 4.0, 0.05);
	ADDF("spec_scroll", "AVSpecScroll", 0.1, 4.0, 0.05);
	ADDF("spec_angle", "AVSpecAngle", 0.0, 90.0, 1.0);
	ADDF("spec_dist", "AVSpecDist", 0.0, 1.0, 0.01);
	obs_property_t *sr = obs_properties_add_list(g,
		bg_key(k, sizeof(k), PRE, "spec_ramp"),
		obs_module_text("AVSpecRamp"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(sr, obs_module_text("AVRampHeat"), 0);
	obs_property_list_add_int(sr, obs_module_text("AVRampMono"), 1);
	obs_property_list_add_int(sr, obs_module_text("AVRampSpectrum"), 2);
	obs_property_t *si = obs_properties_add_list(g,
		bg_key(k, sizeof(k), PRE, "spec_idle"),
		obs_module_text("AVSpecIdle"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(si, obs_module_text("AVIdleNoise"), 0);
	obs_property_list_add_int(si, obs_module_text("AVIdleFreeze"), 1);

	ADDF("field_width", "AVFieldWidth", 0.2, 1.0, 0.01);
	ADDI("field_gx", "AVFieldGX", 20, 500, 1);
	ADDI("field_gy", "AVFieldGY", 1, 20, 1);
	ADDF("field_amp", "AVFieldAmp", 0.0, 2.0, 0.05);
	obs_property_t *fw = obs_properties_add_list(g,
		bg_key(k, sizeof(k), PRE, "field_wsrc"),
		obs_module_text("AVFieldSrc"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(fw, obs_module_text("AVFieldSrcWave"), 0);
	obs_property_list_add_int(fw, obs_module_text("AVFieldSrcSpec"), 1);
	obs_property_list_add_int(fw, obs_module_text("AVFieldSrcProc"), 2);
	ADDI("field_wcount", "AVFieldWaves", 1, 6, 1);
	ADDF("field_travel", "AVFieldTravel", 0.0, 4.0, 0.05);
	ADDF("field_bokeh", "AVFieldBokeh", 0.0, 1.0, 0.01);
	ADDF("field_glow", "AVFieldGlow", 0.0, 2.0, 0.05);
	ADDF("field_pulse", "AVFieldPulse", 0.0, 1.0, 0.01);

	obs_property_t *cp = obs_properties_add_list(g,
		bg_key(k, sizeof(k), PRE, "comet_path"),
		obs_module_text("AVCometPath"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(cp, obs_module_text("AVCometCurve"), 0);
	obs_property_list_add_int(cp, obs_module_text("AVCometLine"), 1);
	obs_property_list_add_int(cp, obs_module_text("AVCometSpiral"), 2);
	ADDF("comet_curv", "AVCometCurv", 0.0, 1.0, 0.01);
	ADDF("comet_speed", "AVCometSpeed", 0.1, 4.0, 0.05);
	ADDF("comet_width", "AVCometWidth", 0.0, 1.0, 0.01);
	ADDF("comet_tail", "AVCometTail", 0.1, 4.0, 0.05);
	ADDF("comet_spark", "AVCometSpark", 0.0, 2.0, 0.05);
	ADDF("comet_travel", "AVCometTravel", 0.0, 1.0, 0.01);
	ADDF("comet_swirl", "AVCometSwirl", 0.0, 2.0, 0.05);

	ADDI("tw_count", "AVTwCount", 50, 5000, 10);
	ADDF("tw_speed", "AVTwSpeed", 0.1, 4.0, 0.05);
	ADDF("tw_depth", "AVTwDepth", 0.0, 1.0, 0.01);
	ADDF("tw_drift", "AVTwDrift", 0.0, 0.5, 0.01);
	ADDF("tw_bright", "AVTwBright", 0.0, 1.0, 0.01);
	ADDF("tw_sparkle", "AVTwSparkle", 0.0, 0.3, 0.01);
	ADDF("tw_top", "AVTwTop", 0.0, 1.0, 0.01);

	/* ---- shared particle / appearance / motion ---- */
	bg_common_props(g, PRE, &k_spec);

	ADDF("reactivity", "AVReactivity", 0.0, 2.0, 0.05);
	ADDF("time_scale", "AVTimeScale", 0.1, 4.0, 0.05);

	obs_property_t *cm = obs_properties_add_list(g,
		bg_key(k, sizeof(k), PRE, "colmode"),
		obs_module_text("AVColorMode"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(cm, obs_module_text("AVColorSingle"),
				  COL_SINGLE);
	obs_property_list_add_int(cm, obs_module_text("AVColorGrad"), COL_GRAD);
	obs_property_list_add_int(cm, obs_module_text("AVColorSpectrum"),
				  COL_SPEC);
	ADDC("color2", "AVColorB");
	ADDF("grad", "AVGradStrength", 0.0, 1.0, 0.01);

	obs_property_t *bl = obs_properties_add_list(g,
		bg_key(k, sizeof(k), PRE, "blend"),
		obs_module_text("AVBlend"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(bl, obs_module_text("AVBlendAdd"), BLEND_ADD);
	obs_property_list_add_int(bl, obs_module_text("AVBlendScreen"),
				  BLEND_SCREEN);
	obs_property_list_add_int(bl, obs_module_text("AVBlendNormal"),
				  BLEND_NORMAL);

	obs_property_t *tr = obs_properties_add_bool(g,
		bg_key(k, sizeof(k), PRE, "transparent"),
		obs_module_text("AVTransparent"));
	obs_property_set_modified_callback2(tr, on_transparent_changed, NULL);
	ADDC("bgcolor", "AVBgColor");

	ADDF("gravity", "AVGravity", -2000.0, 2000.0, 10.0);
	ADDF("drag", "AVDrag", 0.0, 1.0, 0.01);
	ADDF("turb_int", "AVTurbInt", 0.0, 2.0, 0.05);
	ADDF("turb_scale", "AVTurbScale", 0.1, 10.0, 0.1);
	ADDF("turb_speed", "AVTurbSpeed", 0.0, 4.0, 0.05);
	ADDF("jitter", "AVJitter", 0.0, 1.0, 0.01);

	bg_post_props(g, PRE);

	/* ---- Seed generator (§2.6) ---- */
	ADDB("auto_idle", "AVAutoIdle");
	ADDF("idle_fade", "AVIdleFade", 0.2, 5.0, 0.05);
	ADDF("lfo_rate", "AVLfoRate", 0.01, 4.0, 0.01);
	ADDF("lfo_depth", "AVLfoDepth", 0.0, 1.0, 0.01);
	obs_property_t *lw = obs_properties_add_list(g,
		bg_key(k, sizeof(k), PRE, "lfo_wave"),
		obs_module_text("AVLfoWave"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(lw, obs_module_text("AVLfoSine"), LFO_SINE);
	obs_property_list_add_int(lw, obs_module_text("AVLfoTri"), LFO_TRI);
	obs_property_list_add_int(lw, obs_module_text("AVLfoSaw"), LFO_SAW);
	obs_property_list_add_int(lw, obs_module_text("AVLfoSNoise"),
				  LFO_SNOISE);
	ADDF("idle_noise_speed", "AVIdleNoiseSpeed", 0.0, 4.0, 0.05);
	ADDF("idle_noise_amount", "AVIdleNoiseAmount", 0.0, 1.0, 0.01);
	ADDB("autobeat", "AVAutoBeat");
	ADDF("autobeat_bpm", "AVAutoBeatBpm", 30.0, 240.0, 1.0);
	ADDF("autobeat_jitter", "AVAutoBeatJitter", 0.0, 1.0, 0.01);

	int active = settings ? (int)obs_data_get_int(settings,
		bg_key(k, sizeof(k), PRE, "preset")) : AV_BARS;
	av_apply_vis(g, active);
	{
		obs_property_t *p = obs_properties_get(g,
			bg_key(k, sizeof(k), PRE, "bgcolor"));
		if (p)
			obs_property_set_visible(p, settings ?
				!obs_data_get_bool(settings,
					bg_key(k, sizeof(k), PRE,
					       "transparent")) : false);
	}
}

#undef ADDF
#undef ADDI
#undef ADDB
#undef ADDC

static void audioviz_defaults(obs_data_t *s)
{
	char k[96];
	bg_common_defaults(s, PRE, &k_spec);

	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "preset"), AV_BARS);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "reactivity"),
				    1.0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "time_scale"),
				    1.0);
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "colmode"),
				 COL_GRAD);
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "color2"),
				 (long long)0xFFD84FFF); /* #FF4FD8 pink */
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "grad"), 1.0);
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "blend"),
				 BLEND_ADD);
	obs_data_set_default_bool(s, bg_key(k, sizeof(k), PRE, "transparent"),
				  true);
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "bgcolor"),
				 (long long)0xFF000000);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "gravity"), 0.0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "drag"), 0.1);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "turb_int"),
				    0.3);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "turb_scale"),
				    1.0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "turb_speed"),
				    0.5);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "jitter"), 0.1);

	/* Seed generator */
	obs_data_set_default_bool(s, bg_key(k, sizeof(k), PRE, "auto_idle"),
				  true);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "idle_fade"),
				    1.5);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "lfo_rate"),
				    0.2);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "lfo_depth"),
				    0.7);
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "lfo_wave"),
				 LFO_SINE);
	obs_data_set_default_double(s,
		bg_key(k, sizeof(k), PRE, "idle_noise_speed"), 0.4);
	obs_data_set_default_double(s,
		bg_key(k, sizeof(k), PRE, "idle_noise_amount"), 0.6);
	obs_data_set_default_bool(s, bg_key(k, sizeof(k), PRE, "autobeat"),
				  true);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "autobeat_bpm"),
				    100.0);
	obs_data_set_default_double(s,
		bg_key(k, sizeof(k), PRE, "autobeat_jitter"), 0.15);

	/* bars */
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "bars_count"), 64);
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "bars_ppb"), 40);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "bars_width"),
				    0.7);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "bars_hscale"),
				    1.0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "bars_spread"),
				    0.2);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "bars_boost"),
				    0.5);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "bars_tilt"),
				    0.3);
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "bars_idle"), 0);

	/* rain */
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "rain_radius"),
				    0.7);
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "rain_columns"),
				 120);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "rain_fall"),
				    1.0);
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "rain_trail"), 12);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "rain_flash"),
				    0.15);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "rain_depth"),
				    0.6);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "rain_edge"),
				    0.8);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "rain_rot"),
				    0.1);

	/* spectrogram */
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "spec_freq"), 128);
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "spec_hist"), 120);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "spec_hscale"),
				    1.0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "spec_scroll"),
				    1.0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "spec_angle"),
				    45.0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "spec_dist"),
				    0.7);
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "spec_ramp"), 0);
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "spec_idle"), 0);

	/* field */
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "field_width"),
				    0.9);
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "field_gx"), 200);
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "field_gy"), 5);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "field_amp"),
				    0.6);
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "field_wsrc"), 0);
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "field_wcount"), 3);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "field_travel"),
				    0.5);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "field_bokeh"),
				    0.5);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "field_glow"),
				    1.0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "field_pulse"),
				    0.4);

	/* comet */
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "comet_path"), 0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "comet_curv"),
				    0.5);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "comet_speed"),
				    1.0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "comet_width"),
				    0.3);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "comet_tail"),
				    1.5);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "comet_spark"),
				    0.8);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "comet_travel"),
				    0.6);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "comet_swirl"),
				    1.0);

	/* twinkle */
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "tw_count"), 800);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "tw_speed"),
				    0.8);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "tw_depth"),
				    0.6);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "tw_drift"),
				    0.05);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "tw_bright"),
				    0.4);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "tw_sparkle"),
				    0.1);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "tw_top"), 0.5);

	bg_post_defaults(s, PRE, 0.5, 0.4, 0.4, 0.0);

	/* Override common particle defaults that differ from the spec. */
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "size"), 6.0);
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "size_var"), 30);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "lifetime"),
				    2.0);
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "life_var"), 20);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "rate"), 1000.0);
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "max"), 20000);
}

const struct bg_effect bgfx_audioviz = {
	.id             = "audioviz",
	.name_key       = "EffectAudioViz",
	.create         = audioviz_create,
	.destroy        = audioviz_destroy,
	.load_graphics  = audioviz_load_graphics,
	.update         = audioviz_update,
	.tick           = audioviz_tick,
	.render         = audioviz_render,
	.reset          = audioviz_reset,
	.get_properties = audioviz_properties,
	.get_defaults   = audioviz_defaults,
};
