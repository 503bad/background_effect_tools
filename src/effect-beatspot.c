/* Beat Spotlight: a row of stage spotlights that react on every kick. On each
 * beat the beams recolour, swing to a new angle, flip their on/off checker, and
 * (optionally) toggle between shining downward as light cones and facing the
 * camera as glowing discs. Beat onsets come from a local low-band detector
 * (mirroring bgfx-audio.c); with no audio source the beams self-drive at a
 * configurable BPM so the effect is always usable. Each beam is drawn as one
 * additive full-canvas pass of data/effects/spotlight.effect. */

#include "effect-beatspot.h"
#include "bgfx-effect.h"
#include "bgfx-props.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

#define PRE "beatspot"
#define BG_TAU 6.28318530718f
#define MAXCOLS 16
#define MAXROWS 8
#define MAXB (MAXCOLS * MAXROWS) /* grid of spotlights */

/* Colour cycling mode. */
enum {
	CM_STEP = 0,   /* hue advances a fixed step each beat */
	CM_RANDOM = 1, /* random hue per beam per beat        */
	CM_PALETTE = 2,/* random blend of colour A..B         */
	CM_SINGLE = 3, /* fixed single colour (colour A)      */
};

/* On/off pattern. */
enum {
	PAT_ALT = 0,    /* checker, flips each beat */
	PAT_ALL = 1,    /* all lit                  */
	PAT_RANDOM = 2, /* random subset            */
};

/* Orientation. */
enum {
	OR_DOWN = 0,   /* downward cones      */
	OR_FRONT = 1,  /* front-facing discs  */
	OR_TOGGLE = 2, /* switch on the beat  */
};

struct beatspot_state {
	gs_effect_t *fx;
	gs_effect_t *viz; /* flat vertex-colour shader for the monitor */

	int      cols, rows;
	int      count; /* = cols * rows, clamped to MAXB */
	bool     monitor; /* draw the live kick-band / threshold meter */
	uint32_t color_a, color_b;
	int      colormode;
	float    length;
	float    cone_deg;
	float    swing_deg;
	int      pattern;
	int      orient_mode;
	int      orient_every;
	float    intensity;
	float    bloom;
	float    softness;

	float    sens;
	float    band_lo, band_hi;
	float    attack, decay;
	float    idle_bpm;

	/* runtime */
	uint32_t rng;
	bool     inited;
	int      beat_count;
	int      orient_cur; /* 0 cone, 1 disc */
	float    hue;        /* CM_STEP running hue */
	float    flash;      /* global beat punch, decays */
	float    kick_avg, kick_since;
	float    idle_timer;

	/* monitor / calibration meter followers */
	float    mon_energy; /* smoothed current kick-band energy */
	float    mon_peak;   /* slow-falling peak hold            */
	float    mon_thresh; /* live trigger threshold (avg×sens) */
	float    mon_flash;  /* lights up on each detected beat    */

	float    on_lvl[MAXB];   /* smoothed 0..1 */
	int      on_tgt[MAXB];   /* 0/1 target    */
	float    ang_cur[MAXB];  /* radians offset from straight-down */
	float    ang_tgt[MAXB];
	float    col_cur[MAXB][3];
	float    col_tgt[MAXB][3];
};

static uint32_t xs(uint32_t *st)
{
	uint32_t x = *st ? *st : 0x2468ACEu;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*st = x;
	return x;
}

static float frand(struct beatspot_state *s)
{
	return (float)(xs(&s->rng) & 0xFFFFFF) / (float)0x1000000;
}

static float frand_range(struct beatspot_state *s, float lo, float hi)
{
	return lo + (hi - lo) * frand(s);
}

static void hsv2rgb(float h, float sv, float vv, float out[3])
{
	h = h - floorf(h);
	float r = fabsf(h * 6.0f - 3.0f) - 1.0f;
	float g = 2.0f - fabsf(h * 6.0f - 2.0f);
	float b = 2.0f - fabsf(h * 6.0f - 4.0f);
	r = r < 0 ? 0 : (r > 1 ? 1 : r);
	g = g < 0 ? 0 : (g > 1 ? 1 : g);
	b = b < 0 ? 0 : (b > 1 ? 1 : b);
	out[0] = ((r - 1.0f) * sv + 1.0f) * vv;
	out[1] = ((g - 1.0f) * sv + 1.0f) * vv;
	out[2] = ((b - 1.0f) * sv + 1.0f) * vv;
}

static float clamp01f(float v)
{
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/* Premultiplied-alpha colour for the viz monitor (ONE/INVSRCALPHA blend). */
static uint32_t mcol(float r, float g, float b, float a)
{
	float R = r * a, G = g * a, B = b * a;
	uint32_t Ri = (uint32_t)(R < 0 ? 0 : (R > 1 ? 255 : R * 255.0f + 0.5f));
	uint32_t Gi = (uint32_t)(G < 0 ? 0 : (G > 1 ? 255 : G * 255.0f + 0.5f));
	uint32_t Bi = (uint32_t)(B < 0 ? 0 : (B > 1 ? 255 : B * 255.0f + 0.5f));
	uint32_t Ai = (uint32_t)(a < 0 ? 0 : (a > 1 ? 255 : a * 255.0f + 0.5f));
	return (Ai << 24) | (Bi << 16) | (Gi << 8) | Ri;
}

static void mquad(uint32_t c, float x0, float y0, float x1, float y1)
{
	gs_color(c);
	gs_vertex2f(x0, y0);
	gs_color(c);
	gs_vertex2f(x1, y0);
	gs_color(c);
	gs_vertex2f(x1, y1);
	gs_color(c);
	gs_vertex2f(x0, y0);
	gs_color(c);
	gs_vertex2f(x1, y1);
	gs_color(c);
	gs_vertex2f(x0, y1);
}

static void *beatspot_create(void)
{
	struct beatspot_state *s = bzalloc(sizeof(*s));
	s->rng = 0x1234567u;
	s->kick_since = 1.0f;
	return s;
}

static void beatspot_destroy(void *data)
{
	struct beatspot_state *s = data;
	if (!s)
		return;
	if (s->fx)
		gs_effect_destroy(s->fx);
	if (s->viz)
		gs_effect_destroy(s->viz);
	bfree(s);
}

static void beatspot_load_graphics(void *data)
{
	struct beatspot_state *s = data;
	char *path = obs_module_file("effects/spotlight.effect");
	if (path) {
		s->fx = gs_effect_create_from_file(path, NULL);
		if (!s->fx)
			obs_log(LOG_ERROR, "failed to load spotlight.effect (%s)",
				path);
	}
	bfree(path);
	char *vpath = obs_module_file("effects/viz.effect");
	if (vpath) {
		s->viz = gs_effect_create_from_file(vpath, NULL);
		if (!s->viz)
			obs_log(LOG_ERROR, "failed to load viz.effect (%s)",
				vpath);
	}
	bfree(vpath);
}

static double getd(obs_data_t *s, const char *n)
{
	char k[96];
	return obs_data_get_double(s, bg_key(k, sizeof(k), PRE, n));
}

static long long geti(obs_data_t *s, const char *n)
{
	char k[96];
	return obs_data_get_int(s, bg_key(k, sizeof(k), PRE, n));
}

static bool getb(obs_data_t *s, const char *n)
{
	char k[96];
	return obs_data_get_bool(s, bg_key(k, sizeof(k), PRE, n));
}

static void beatspot_update(void *data, obs_data_t *settings)
{
	struct beatspot_state *s = data;
	s->cols = (int)geti(settings, "cols");
	if (s->cols < 1)
		s->cols = 1;
	if (s->cols > MAXCOLS)
		s->cols = MAXCOLS;
	s->rows = (int)geti(settings, "rows");
	if (s->rows < 1)
		s->rows = 1;
	if (s->rows > MAXROWS)
		s->rows = MAXROWS;
	s->count = s->cols * s->rows;
	if (s->count > MAXB)
		s->count = MAXB;
	s->monitor = getb(settings, "monitor");
	s->color_a = (uint32_t)geti(settings, "color_a");
	s->color_b = (uint32_t)geti(settings, "color_b");
	s->colormode = (int)geti(settings, "colormode");
	s->length = (float)getd(settings, "length");
	s->cone_deg = (float)getd(settings, "cone");
	s->swing_deg = (float)getd(settings, "swing");
	s->pattern = (int)geti(settings, "pattern");
	s->orient_mode = (int)geti(settings, "orient");
	s->orient_every = (int)geti(settings, "orient_every");
	if (s->orient_every < 1)
		s->orient_every = 1;
	s->intensity = (float)getd(settings, "intensity");
	s->bloom = (float)getd(settings, "bloom");
	s->softness = (float)getd(settings, "softness");
	s->sens = (float)getd(settings, "sens");
	s->band_lo = (float)getd(settings, "band_lo");
	s->band_hi = (float)getd(settings, "band_hi");
	s->attack = (float)getd(settings, "attack");
	s->decay = (float)getd(settings, "decay");
	s->idle_bpm = (float)getd(settings, "idle_bpm");

	if (s->orient_mode == OR_FRONT)
		s->orient_cur = 1;
	else if (s->orient_mode == OR_DOWN)
		s->orient_cur = 0;
}

static void beatspot_reset(void *data, uint32_t seed)
{
	struct beatspot_state *s = data;
	s->rng = seed ? seed : 0x1234567u;
	s->inited = false;
	s->beat_count = 0;
	s->hue = 0.0f;
	s->flash = 0.0f;
	s->kick_avg = 0.0f;
	s->kick_since = 1.0f;
	s->idle_timer = 0.0f;
	s->mon_energy = 0.0f;
	s->mon_peak = 0.0f;
	s->mon_thresh = 0.0f;
	s->mon_flash = 0.0f;
	s->orient_cur = (s->orient_mode == OR_FRONT) ? 1 : 0;
}

/* Pick this beam's new colour for the current beat. */
static void beam_color(struct beatspot_state *s, int i, float out[3])
{
	if (s->colormode == CM_SINGLE) {
		float a[4];
		bg_unpack_color(s->color_a, a);
		out[0] = a[0];
		out[1] = a[1];
		out[2] = a[2];
	} else if (s->colormode == CM_RANDOM) {
		hsv2rgb(frand(s), 0.85f, 1.0f, out);
	} else if (s->colormode == CM_PALETTE) {
		float a[4], b[4];
		bg_unpack_color(s->color_a, a);
		bg_unpack_color(s->color_b, b);
		float t = frand(s);
		out[0] = a[0] + (b[0] - a[0]) * t;
		out[1] = a[1] + (b[1] - a[1]) * t;
		out[2] = a[2] + (b[2] - a[2]) * t;
	} else { /* CM_STEP */
		float h = s->hue + (float)i * 0.13f;
		hsv2rgb(h, 0.85f, 1.0f, out);
	}
}

/* Apply one beat: new on/off, angles, colours, orientation, punch. */
static void apply_beat(struct beatspot_state *s)
{
	s->beat_count++;
	s->hue += 0.11f;
	s->flash = 1.0f;

	if (s->orient_mode == OR_TOGGLE) {
		if (s->beat_count % s->orient_every == 0)
			s->orient_cur ^= 1;
	} else {
		s->orient_cur = (s->orient_mode == OR_FRONT) ? 1 : 0;
	}

	float swing = s->swing_deg * (BG_TAU / 360.0f);
	int cols = s->cols < 1 ? 1 : s->cols;
	for (int i = 0; i < s->count; ++i) {
		int col = i % cols, row = i / cols;
		int on;
		if (s->pattern == PAT_ALL)
			on = 1;
		else if (s->pattern == PAT_RANDOM)
			on = (frand(s) < 0.6f) ? 1 : 0;
		else /* PAT_ALT: checkerboard that flips each beat */
			on = ((col + row + s->beat_count) & 1) ? 0 : 1;
		s->on_tgt[i] = on;

		s->ang_tgt[i] = frand_range(s, -swing, swing);
		beam_color(s, i, s->col_tgt[i]);
	}
}

static void beatspot_tick(void *data, const struct bg_ctx *ctx, float dt)
{
	struct beatspot_state *s = data;

	if (!s->inited) {
		apply_beat(s);
		for (int i = 0; i < s->count; ++i) {
			s->ang_cur[i] = s->ang_tgt[i];
			s->on_lvl[i] = (float)s->on_tgt[i];
			for (int c = 0; c < 3; ++c)
				s->col_cur[i][c] = s->col_tgt[i][c];
		}
		s->inited = true;
	}

	const struct bg_audio_fft *fft = ctx->fft;
	bool have = fft && fft->valid;
	bool kick = false;

	if (have) {
		float e = bg_fft_band(fft, s->band_lo, s->band_hi);
		s->kick_avg += (e - s->kick_avg) * 0.12f;
		s->kick_since += dt;
		if (e > s->kick_avg * s->sens && e > 0.06f &&
		    s->kick_since > 0.10f) {
			kick = true;
			s->kick_since = 0.0f;
		}
		/* Monitor followers: fast-attack / slow-release energy + peak hold
		 * and the live threshold (avg × sensitivity) for the meter. */
		if (e > s->mon_energy)
			s->mon_energy = e;
		else
			s->mon_energy += (e - s->mon_energy) * 0.30f;
		if (s->mon_energy > s->mon_peak)
			s->mon_peak = s->mon_energy;
		else
			s->mon_peak -= dt * 0.4f;
		if (s->mon_peak < 0.0f)
			s->mon_peak = 0.0f;
		s->mon_thresh = s->kick_avg * s->sens;
	} else {
		s->mon_energy *= expf(-dt / 0.2f);
		s->mon_peak -= dt * 0.4f;
		if (s->mon_peak < 0.0f)
			s->mon_peak = 0.0f;
		s->mon_thresh = 0.0f;
		/* Self-drive at the chosen BPM when no audio is analysing. */
		float interval = (s->idle_bpm > 1.0f) ? 60.0f / s->idle_bpm
						      : 0.5f;
		s->idle_timer += dt;
		if (s->idle_timer >= interval) {
			s->idle_timer -= interval;
			kick = true;
		}
	}

	if (kick) {
		apply_beat(s);
		s->mon_flash = 1.0f;
	}
	s->mon_flash *= expf(-dt / 0.12f);

	/* Smooth every beam toward its target each frame. */
	float ka = 1.0f - expf(-dt / (s->attack > 0.005f ? s->attack : 0.005f));
	float kd = 1.0f - expf(-dt / (s->decay > 0.02f ? s->decay : 0.02f));
	for (int i = 0; i < s->count; ++i) {
		s->ang_cur[i] += (s->ang_tgt[i] - s->ang_cur[i]) * ka;
		for (int c = 0; c < 3; ++c)
			s->col_cur[i][c] +=
				(s->col_tgt[i][c] - s->col_cur[i][c]) * ka;
		float tgt = (float)s->on_tgt[i];
		float rate = (tgt > s->on_lvl[i]) ? ka : kd;
		s->on_lvl[i] += (tgt - s->on_lvl[i]) * rate;
	}
	s->flash *= (1.0f - kd);
}

static void set_f(gs_effect_t *e, const char *n, float v)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(e, n);
	if (p)
		gs_effect_set_float(p, v);
}

/* Live calibration meter: a bar of the current kick-band energy with the
 * trigger threshold (avg × sensitivity) and a slow peak-hold marked on it, so
 * the user can drag Sensitivity until the threshold line sits just above the
 * idle energy and the kicks punch past it. Flashes on every detected beat. */
static void beatspot_draw_monitor(struct beatspot_state *s,
				  const struct bg_ctx *ctx)
{
	if (!s->viz)
		return;
	float w = (float)ctx->width, h = (float)ctx->height;
	float mw = fminf(w * 0.42f, 520.0f);
	float mh = 28.0f;
	float pad = 4.0f;
	float mx = 24.0f, my = h - 28.0f - mh;
	float ix0 = mx + pad, ix1 = mx + mw - pad;
	float iy0 = my + pad, iy1 = my + mh - pad;
	float iw = ix1 - ix0;

	/* Auto-scale so the bar fills the track regardless of input gain. */
	float sc = fmaxf(0.25f, s->mon_peak * 1.15f);
	float ef = clamp01f(s->mon_energy / sc);
	float tf = clamp01f(s->mon_thresh / sc);
	float pf = clamp01f(s->mon_peak / sc);
	bool over = s->mon_energy >= s->mon_thresh && s->mon_thresh > 0.0f;

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(s->viz, "Draw")) {
		gs_render_start(true);
		/* panel + inner track */
		mquad(mcol(0.0f, 0.0f, 0.0f, 0.55f), mx, my, mx + mw, my + mh);
		mquad(mcol(0.10f, 0.12f, 0.16f, 0.9f), ix0, iy0, ix1, iy1);
		/* energy fill: green under threshold, hot when over */
		float fillx = ix0 + iw * ef;
		uint32_t fc = over ? mcol(1.0f, 0.85f, 0.3f, 0.95f)
				   : mcol(0.2f, 0.9f, 0.5f, 0.9f);
		mquad(fc, ix0, iy0, fillx, iy1);
		/* peak-hold marker (white) */
		float px = ix0 + iw * pf;
		mquad(mcol(1.0f, 1.0f, 1.0f, 0.9f), px - 1.5f, iy0, px + 1.5f,
		      iy1);
		/* threshold marker (orange line, taller) */
		float thx = ix0 + iw * tf;
		mquad(mcol(1.0f, 0.45f, 0.15f, 1.0f), thx - 1.5f, my, thx + 1.5f,
		      my + mh);
		/* beat flash: brighten the whole panel border */
		if (s->mon_flash > 0.01f) {
			uint32_t bf = mcol(0.6f, 0.9f, 1.0f, 0.5f * s->mon_flash);
			mquad(bf, mx, my, mx + mw, my + 3.0f);
			mquad(bf, mx, my + mh - 3.0f, mx + mw, my + mh);
		}
		gs_render_stop(GS_TRIS);
	}
	gs_blend_state_pop();
}

static void beatspot_render(void *data, const struct bg_ctx *ctx)
{
	struct beatspot_state *s = data;
	if (s->monitor)
		beatspot_draw_monitor(s, ctx);
	if (!s->fx || s->count < 1)
		return;

	float w = (float)ctx->width, h = (float)ctx->height;
	float half_angle = s->cone_deg * 0.5f * (BG_TAU / 360.0f);
	float radius = s->length * 0.35f;

	gs_eparam_t *pcanvas = gs_effect_get_param_by_name(s->fx, "canvas");
	gs_eparam_t *papex = gs_effect_get_param_by_name(s->fx, "apex");
	gs_eparam_t *pcol = gs_effect_get_param_by_name(s->fx, "beam_color");

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ONE); /* additive light */

	int cols = s->cols < 1 ? 1 : s->cols;
	int rows = s->rows < 1 ? 1 : s->rows;
	for (int i = 0; i < s->count; ++i) {
		float lvl = s->on_lvl[i];
		if (lvl <= 0.01f)
			continue;
		float intensity = s->intensity * lvl * (1.0f + s->flash * 0.6f);

		int col = i % cols, row = i / cols;
		float apex_x = (col + 0.5f) / (float)cols * w;
		/* Cones hang from each row's top edge; discs sit at its centre. */
		float apex_y = (s->orient_cur == 0)
				       ? (float)row / (float)rows * h
				       : (row + 0.5f) / (float)rows * h;
		float dir = BG_TAU * 0.25f + s->ang_cur[i]; /* straight down + swing */

		if (pcanvas) {
			struct vec2 c;
			vec2_set(&c, w, h);
			gs_effect_set_vec2(pcanvas, &c);
		}
		if (papex) {
			struct vec2 a;
			vec2_set(&a, apex_x, apex_y);
			gs_effect_set_vec2(papex, &a);
		}
		if (pcol) {
			struct vec4 cc;
			vec4_set(&cc, s->col_cur[i][0], s->col_cur[i][1],
				 s->col_cur[i][2], 1.0f);
			gs_effect_set_vec4(pcol, &cc);
		}
		set_f(s->fx, "dir", dir);
		set_f(s->fx, "beam_len", s->length);
		set_f(s->fx, "half_angle", half_angle);
		set_f(s->fx, "beam_type", (float)s->orient_cur);
		set_f(s->fx, "radius", radius);
		set_f(s->fx, "intensity", intensity);
		set_f(s->fx, "softness", s->softness);
		set_f(s->fx, "bloom", s->bloom);

		while (gs_effect_loop(s->fx, "Draw"))
			gs_draw_sprite(NULL, 0, ctx->width, ctx->height);
	}

	gs_blend_state_pop();
}

static void beatspot_properties(obs_properties_t *g, obs_data_t *settings)
{
	char k[96];
	UNUSED_PARAMETER(settings);

	obs_properties_add_int_slider(g, bg_key(k, sizeof(k), PRE, "cols"),
		obs_module_text("BeatSpotCols"), 1, MAXCOLS, 1);
	obs_properties_add_int_slider(g, bg_key(k, sizeof(k), PRE, "rows"),
		obs_module_text("BeatSpotRows"), 1, MAXROWS, 1);

	obs_property_t *cm = obs_properties_add_list(g,
		bg_key(k, sizeof(k), PRE, "colormode"),
		obs_module_text("BeatSpotColorMode"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(cm, obs_module_text("BeatSpotColorSingle"),
				  CM_SINGLE);
	obs_property_list_add_int(cm, obs_module_text("BeatSpotColorStep"),
				  CM_STEP);
	obs_property_list_add_int(cm, obs_module_text("BeatSpotColorRandom"),
				  CM_RANDOM);
	obs_property_list_add_int(cm, obs_module_text("BeatSpotColorPalette"),
				  CM_PALETTE);
	obs_properties_add_color(g, bg_key(k, sizeof(k), PRE, "color_a"),
		obs_module_text("BeatSpotColorA"));
	obs_properties_add_color(g, bg_key(k, sizeof(k), PRE, "color_b"),
		obs_module_text("BeatSpotColorB"));

	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "length"),
		obs_module_text("BeatSpotLength"), 200.0, 2400.0, 10.0);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "cone"),
		obs_module_text("BeatSpotCone"), 2.0, 60.0, 0.5);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "swing"),
		obs_module_text("BeatSpotSwing"), 0.0, 70.0, 1.0);

	obs_property_t *pat = obs_properties_add_list(g,
		bg_key(k, sizeof(k), PRE, "pattern"),
		obs_module_text("BeatSpotPattern"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(pat, obs_module_text("BeatSpotPatAlt"),
				  PAT_ALT);
	obs_property_list_add_int(pat, obs_module_text("BeatSpotPatAll"),
				  PAT_ALL);
	obs_property_list_add_int(pat, obs_module_text("BeatSpotPatRandom"),
				  PAT_RANDOM);

	obs_property_t *orr = obs_properties_add_list(g,
		bg_key(k, sizeof(k), PRE, "orient"),
		obs_module_text("BeatSpotOrient"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(orr, obs_module_text("BeatSpotOrientDown"),
				  OR_DOWN);
	obs_property_list_add_int(orr, obs_module_text("BeatSpotOrientFront"),
				  OR_FRONT);
	obs_property_list_add_int(orr, obs_module_text("BeatSpotOrientToggle"),
				  OR_TOGGLE);
	obs_properties_add_int_slider(g,
		bg_key(k, sizeof(k), PRE, "orient_every"),
		obs_module_text("BeatSpotOrientEvery"), 1, 8, 1);

	obs_properties_add_float_slider(g,
		bg_key(k, sizeof(k), PRE, "intensity"),
		obs_module_text("BeatSpotIntensity"), 0.0, 3.0, 0.05);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "bloom"),
		obs_module_text("Bloom"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "softness"),
		obs_module_text("BeatSpotSoftness"), 0.0, 1.0, 0.01);

	obs_properties_add_bool(g, bg_key(k, sizeof(k), PRE, "monitor"),
		obs_module_text("BeatSpotMonitor"));
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "sens"),
		obs_module_text("BeatDotSens"), 0.5, 3.0, 0.01);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "band_lo"),
		obs_module_text("BeatDotBandLo"), 20.0, 200.0, 1.0);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "band_hi"),
		obs_module_text("BeatDotBandHi"), 40.0, 400.0, 1.0);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "attack"),
		obs_module_text("BeatSpotAttack"), 0.005, 0.5, 0.005);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "decay"),
		obs_module_text("BeatSpotDecay"), 0.05, 1.5, 0.01);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "idle_bpm"),
		obs_module_text("BeatSpotIdleBpm"), 40.0, 220.0, 1.0);
}

static void beatspot_defaults(obs_data_t *settings)
{
	char k[96];
	obs_data_set_default_int(settings, bg_key(k, sizeof(k), PRE, "cols"), 9);
	obs_data_set_default_int(settings, bg_key(k, sizeof(k), PRE, "rows"), 1);
	obs_data_set_default_bool(settings,
		bg_key(k, sizeof(k), PRE, "monitor"), false);
	obs_data_set_default_int(settings,
		bg_key(k, sizeof(k), PRE, "colormode"), CM_PALETTE);
	obs_data_set_default_int(settings, bg_key(k, sizeof(k), PRE, "color_a"),
				 (long long)0xFFFF5050); /* #5050ff blue */
	obs_data_set_default_int(settings, bg_key(k, sizeof(k), PRE, "color_b"),
				 (long long)0xFF50FFFF); /* #ffff50 yellow */
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "length"), 480.0);
	obs_data_set_default_double(settings, bg_key(k, sizeof(k), PRE, "cone"),
				    14.5);
	obs_data_set_default_double(settings, bg_key(k, sizeof(k), PRE, "swing"),
				    5.0);
	obs_data_set_default_int(settings, bg_key(k, sizeof(k), PRE, "pattern"),
				 PAT_ALT);
	obs_data_set_default_int(settings, bg_key(k, sizeof(k), PRE, "orient"),
				 OR_FRONT);
	obs_data_set_default_int(settings,
		bg_key(k, sizeof(k), PRE, "orient_every"), 3);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "intensity"), 1.4);
	obs_data_set_default_double(settings, bg_key(k, sizeof(k), PRE, "bloom"),
				    0.41);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "softness"), 0.19);
	obs_data_set_default_double(settings, bg_key(k, sizeof(k), PRE, "sens"),
				    1.30);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "band_lo"), 32.0);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "band_hi"), 76.0);
	obs_data_set_default_double(settings, bg_key(k, sizeof(k), PRE, "attack"),
				    0.005);
	obs_data_set_default_double(settings, bg_key(k, sizeof(k), PRE, "decay"),
				    0.45);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "idle_bpm"), 120.0);
}

const struct bg_effect bgfx_beatspot = {
	.id             = "beatspot",
	.name_key       = "EffectBeatSpot",
	.create         = beatspot_create,
	.destroy        = beatspot_destroy,
	.load_graphics  = beatspot_load_graphics,
	.update         = beatspot_update,
	.tick           = beatspot_tick,
	.render         = beatspot_render,
	.reset          = beatspot_reset,
	.get_properties = beatspot_properties,
	.get_defaults   = beatspot_defaults,
};
