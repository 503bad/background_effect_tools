#include "effect-spectrum.h"
#include "bgfx-effect.h"
#include "bgfx-particles.h"
#include "bgfx-props.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

#define PRE "spectrum"
#define CAPACITY 6000
#define BG_TAU 6.28318530718f

enum spectrum_mode {
	SPEC_CIRCULAR = 0, /* radial bars + glow ring (Trap Nation)  */
	SPEC_BARS = 1,     /* bottom bars + peak caps (Monstercat)   */
	SPEC_WAVE = 2,     /* time-domain waveform wrapped to a ring  */
	SPEC_BEAT = 3,     /* beat-driven particle bursts             */
	SPEC_DOTS = 4,     /* LED dot-matrix bars (cells light up)    */
};

static const struct bg_common_spec k_spec = {
	.size_min = 1.0, .size_max = 40.0, .size_step = 0.5, .size_def = 8.0,
	.life_min = 0.3, .life_max = 8.0, .life_def = 2.0,
	.rate_max = 1500.0, .rate_def = 200.0,
	.max_cap = CAPACITY, .max_def = 4000,
	.color_def = 0xFFFFC864, /* cyan (#64C8FF) */
	.alpha_def = 1.0,
};

struct spectrum_state {
	gs_effect_t *viz;    /* flat vertex-colour shader (viz.effect) */
	gs_effect_t *sprite; /* billboard shader for the bursts        */
	struct bg_particle_system *sys;

	struct bg_common common;
	struct bg_post post;

	int      mode;
	float    radius;
	float    bar_len;
	float    bar_width;
	float    rotate;
	uint32_t color2;
	float    grad;
	float    burst;
	float    burst_speed;
	bool     mirror;
	int      dot_cols;   /* dot-matrix column (division) count */
	float    dot_size;   /* one cell/dot pixel pitch           */

	float    col1[4], col2[4];
	float    peak[BG_FFT_BARS];
	float    phase;
};

/* ---- helpers ------------------------------------------------------------- */

static float clamp01f(float v)
{
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static uint32_t pack_rgba(float r, float g, float b, float a)
{
	uint32_t R = (uint32_t)(r < 0 ? 0 : (r > 1 ? 255 : r * 255.0f + 0.5f));
	uint32_t G = (uint32_t)(g < 0 ? 0 : (g > 1 ? 255 : g * 255.0f + 0.5f));
	uint32_t B = (uint32_t)(b < 0 ? 0 : (b > 1 ? 255 : b * 255.0f + 0.5f));
	uint32_t A = (uint32_t)(a < 0 ? 0 : (a > 1 ? 255 : a * 255.0f + 0.5f));
	return (A << 24) | (B << 16) | (G << 8) | R; /* 0xAABBGGRR */
}

/* Premultiplied-alpha colour for the ONE/INVSRCALPHA viz blend. */
static uint32_t premul(const float rgb[3], float intensity)
{
	float i = clamp01f(intensity);
	return pack_rgba(rgb[0] * i, rgb[1] * i, rgb[2] * i, i);
}

static void grad_rgb(const struct spectrum_state *s, float t, float out[3])
{
	t = clamp01f(t) * s->grad;
	out[0] = s->col1[0] + (s->col2[0] - s->col1[0]) * t;
	out[1] = s->col1[1] + (s->col2[1] - s->col1[1]) * t;
	out[2] = s->col1[2] + (s->col2[2] - s->col1[2]) * t;
}

static void vtx(uint32_t col, float x, float y)
{
	gs_color(col);
	gs_vertex2f(x, y);
}

static void quad(uint32_t col, float ax, float ay, float bx, float by,
		 float cx, float cy, float dx, float dy)
{
	vtx(col, ax, ay);
	vtx(col, bx, by);
	vtx(col, cx, cy);
	vtx(col, ax, ay);
	vtx(col, cx, cy);
	vtx(col, dx, dy);
}

/* ---- lifecycle ----------------------------------------------------------- */

static void *spectrum_create(void)
{
	struct spectrum_state *s = bzalloc(sizeof(*s));
	s->sys = bg_particles_create(CAPACITY);
	s->sys->fade_in = 0.05f;
	s->sys->fade_out = 0.25f;
	return s;
}

static void spectrum_destroy(void *data)
{
	struct spectrum_state *s = data;
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
	if (path) {
		e = gs_effect_create_from_file(path, NULL);
		if (!e)
			obs_log(LOG_ERROR, "failed to load viz.effect (%s)",
				path);
	}
	bfree(path);
	return e;
}

static void spectrum_load_graphics(void *data)
{
	struct spectrum_state *s = data;
	s->viz = load_viz();
	s->sprite = bg_particles_load_effect();
}

static void spectrum_update(void *data, obs_data_t *settings)
{
	struct spectrum_state *s = data;
	char k[96];
	bg_common_update(&s->common, settings, PRE);
	bg_post_update(&s->post, settings, PRE);

	s->mode = (int)obs_data_get_int(settings,
		bg_key(k, sizeof(k), PRE, "mode"));
	s->radius = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "radius"));
	s->bar_len = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "bar_len"));
	s->bar_width = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "bar_width"));
	s->rotate = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "rotate"));
	s->color2 = (uint32_t)obs_data_get_int(settings,
		bg_key(k, sizeof(k), PRE, "color2"));
	s->grad = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "grad"));
	s->burst = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "burst"));
	s->burst_speed = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "burst_speed"));
	s->mirror = obs_data_get_bool(settings,
		bg_key(k, sizeof(k), PRE, "mirror"));
	s->dot_cols = (int)obs_data_get_int(settings,
		bg_key(k, sizeof(k), PRE, "dot_cols"));
	s->dot_size = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "dot_size"));

	bg_unpack_color(s->common.color, s->col1);
	bg_unpack_color(s->color2, s->col2);
}

static void spectrum_reset(void *data, uint32_t seed)
{
	struct spectrum_state *s = data;
	bg_particles_reset(s->sys, seed);
	for (int i = 0; i < BG_FFT_BARS; ++i)
		s->peak[i] = 0.0f;
	s->phase = 0.0f;
}

/* ---- particle bursts ----------------------------------------------------- */

static void spawn_one(struct spectrum_state *s, float ox, float oy,
		      float spd, float size_scale, float colt)
{
	bg_particle_t *p = bg_particles_spawn(s->sys);
	if (!p)
		return;
	struct bg_particle_system *sys = s->sys;
	const struct bg_common *c = &s->common;

	float ang = bg_frand(sys) * BG_TAU;
	float v = spd * bg_frand_range(sys, 0.5f, 1.3f);
	p->x = ox;
	p->y = oy;
	p->vx = cosf(ang) * v;
	p->vy = sinf(ang) * v;
	p->size = bg_vary(sys, c->size, c->size_var) * 0.5f * size_scale;
	p->max_life = p->life = bg_vary(sys, c->lifetime, c->life_var);
	p->seed = bg_frand(sys);
	p->a = c->alpha * bg_frand_range(sys, 0.7f, 1.0f);
	p->grow = 0.6f;

	float rgb[3];
	grad_rgb(s, colt, rgb);
	p->r = rgb[0];
	p->g = rgb[1];
	p->b = rgb[2];
}

static void spectrum_tick(void *data, const struct bg_ctx *ctx, float dt)
{
	struct spectrum_state *s = data;
	struct bg_particle_system *sys = s->sys;
	const struct bg_audio_fft *fft = ctx->fft;
	sys->clock += dt;

	s->phase = fmodf(s->phase + s->rotate * (BG_TAU / 360.0f) * dt, BG_TAU);

	bool live_audio = fft && fft->valid;

	/* Peak caps (bars mode) fall back slowly. */
	for (int b = 0; b < BG_FFT_BARS; ++b) {
		float v = live_audio ? fft->bars[b] : 0.0f;
		if (v > s->peak[b])
			s->peak[b] = v;
		else {
			s->peak[b] -= 0.5f * dt;
			if (s->peak[b] < 0.0f)
				s->peak[b] = 0.0f;
		}
	}

	float w = (float)ctx->width, h = (float)ctx->height;
	float cx = w * 0.5f, cy = h * 0.5f;
	size_t cap = (size_t)(s->common.max_count < (int)sys->capacity
				      ? s->common.max_count
				      : (int)sys->capacity);

	if (live_audio) {
		/* Continuous emission for the beat mode, denser with treble. */
		if (s->mode == SPEC_BEAT) {
			sys->emit_accum += s->common.rate *
					   (0.3f + fft->treble) * dt;
			while (sys->emit_accum >= 1.0f && sys->live < cap) {
				sys->emit_accum -= 1.0f;
				spawn_one(s, cx, cy, s->burst_speed * 0.25f,
					  0.6f + fft->bass, bg_frand(sys));
			}
			if (sys->emit_accum > 8.0f)
				sys->emit_accum = 8.0f;
		}

		/* Beat onset → a radial burst (circular & beat modes). */
		if ((s->mode == SPEC_BEAT || s->mode == SPEC_CIRCULAR) &&
		    fft->beat_trigger) {
			int n = (int)(s->burst * (0.4f + fft->bass));
			float ox = cx, oy = cy;
			float r = s->radius * (1.0f + 0.25f * fft->bass);
			for (int i = 0; i < n && sys->live < cap; ++i) {
				if (s->mode == SPEC_CIRCULAR) {
					float a = bg_frand(sys) * BG_TAU;
					ox = cx + r * cosf(a);
					oy = cy + r * sinf(a);
				}
				spawn_one(s, ox, oy, s->burst_speed,
					  0.8f + fft->bass, 0.7f + 0.3f * fft->treble);
			}
		}
	}

	/* Integrate the particles: radial coast with drag. */
	const float margin = 200.0f;
	for (size_t i = 0; i < sys->live; ++i) {
		bg_particle_t *p = &sys->items[i];
		float drag = 1.0f - 1.2f * dt;
		if (drag < 0.0f)
			drag = 0.0f;
		p->vx *= drag;
		p->vy *= drag;
		p->x += p->vx * dt;
		p->y += p->vy * dt;
		if (p->x < -margin || p->x > w + margin || p->y < -margin ||
		    p->y > h + margin)
			p->life = 0.0f;
		else
			p->life -= dt;
	}
	bg_particles_compact(sys);
}

/* ---- drawing ------------------------------------------------------------- */

static void draw_circular(struct spectrum_state *s, const struct bg_audio_fft *fft,
			  float cx, float cy)
{
	int N = BG_FFT_BARS;
	int total = s->mirror ? N * 2 : N;
	float R = s->radius * (1.0f + 0.25f * fft->bass);
	float ringw = fmaxf(2.0f, s->radius * 0.03f);

	for (int seg = 0; seg < total; ++seg) {
		int bidx = s->mirror ? (seg < N ? seg : (total - 1 - seg)) : seg;
		float a0 = s->phase + (float)seg / total * BG_TAU;
		float a1 = s->phase + (float)(seg + 1) / total * BG_TAU;
		float pad = (1.0f - s->bar_width) * 0.5f;
		float ab0 = a0 + (a1 - a0) * pad;
		float ab1 = a1 - (a1 - a0) * pad;

		float v = fft->bars[bidx];
		float len = v * s->bar_len;
		float rgb[3];
		grad_rgb(s, (float)bidx / N, rgb);

		/* Base ring band (full slot, steady glow). */
		uint32_t rcol = premul(rgb, s->common.alpha * 0.55f);
		quad(rcol, cx + (R - ringw) * cosf(a0),
		     cy + (R - ringw) * sinf(a0), cx + (R - ringw) * cosf(a1),
		     cy + (R - ringw) * sinf(a1), cx + R * cosf(a1),
		     cy + R * sinf(a1), cx + R * cosf(a0), cy + R * sinf(a0));

		if (len < 0.5f)
			continue;
		/* Radiating bar. */
		uint32_t col = premul(rgb, s->common.alpha * (0.35f + 0.75f * v));
		float ro = R + len;
		quad(col, cx + R * cosf(ab0), cy + R * sinf(ab0),
		     cx + R * cosf(ab1), cy + R * sinf(ab1),
		     cx + ro * cosf(ab1), cy + ro * sinf(ab1),
		     cx + ro * cosf(ab0), cy + ro * sinf(ab0));
	}
}

static void draw_bars(struct spectrum_state *s, const struct bg_audio_fft *fft,
		      float w, float h)
{
	int N = BG_FFT_BARS;
	float slot = w / (float)N;
	for (int b = 0; b < N; ++b) {
		float v = fft->bars[b];
		float len = v * s->bar_len;
		float x0 = b * slot + slot * (1.0f - s->bar_width) * 0.5f;
		float x1 = x0 + slot * s->bar_width;
		float rgb[3];
		grad_rgb(s, (float)b / N, rgb);

		if (len > 0.5f) {
			uint32_t col = premul(rgb,
				s->common.alpha * (0.4f + 0.6f * v));
			quad(col, x0, h, x1, h, x1, h - len, x0, h - len);
		}
		/* Peak cap. */
		float pk = s->peak[b];
		if (pk > 0.01f) {
			float cyp = h - pk * s->bar_len;
			float caph = fmaxf(2.0f, slot * 0.2f);
			uint32_t pcol = premul(rgb, s->common.alpha * 0.9f);
			quad(pcol, x0, cyp, x1, cyp, x1, cyp - caph, x0,
			     cyp - caph);
		}
	}
}

/* LED dot-matrix: `dot_cols` frequency columns, each a stack of square cells
 * lit from the bottom up to its level. The grid is sized from `dot_size` (cell
 * pitch); a single brighter cell marks the slow-falling peak. */
static void draw_dots(struct spectrum_state *s, const struct bg_audio_fft *fft,
		      float w, float h)
{
	int N = s->dot_cols;
	if (N < 2)
		N = 2;
	float cell = s->dot_size;
	if (cell < 3.0f)
		cell = 3.0f;
	int rows = (int)(h / cell);
	if (rows < 1)
		rows = 1;
	float gap = cell * 0.16f;
	float half = (cell - gap) * 0.5f;
	float slot = w / (float)N;
	/* Centre the cell stack vertically within the canvas. */
	float gridH = rows * cell;
	float botY = h - (h - gridH) * 0.5f; /* baseline of row 0 */

	for (int b = 0; b < N; ++b) {
		/* Resample the BG_FFT_BARS spectrum to N columns. */
		float fp = (float)b / (float)(N - 1) * (float)(BG_FFT_BARS - 1);
		int i0 = (int)fp;
		int i1 = i0 + 1 < BG_FFT_BARS ? i0 + 1 : BG_FFT_BARS - 1;
		float v = fft->bars[i0] + (fft->bars[i1] - fft->bars[i0]) *
						  (fp - (float)i0);
		float pk = s->peak[i0] + (s->peak[i1] - s->peak[i0]) *
						 (fp - (float)i0);

		int lit = (int)(clamp01f(v) * (float)rows + 0.5f);
		int pkRow = (int)(clamp01f(pk) * (float)(rows - 1) + 0.5f);
		float cxp = b * slot + slot * 0.5f;

		for (int r = 0; r < rows; ++r) {
			float rowFrac = (float)r / (float)(rows > 1 ? rows - 1
								       : 1);
			float rgb[3];
			grad_rgb(s, rowFrac, rgb); /* low→high colour ramp */
			float inten;
			if (r < lit)
				inten = s->common.alpha * (0.55f + 0.45f * v);
			else if (r == pkRow && pk > 0.02f)
				inten = s->common.alpha * 0.9f; /* peak cap cell */
			else
				inten = s->common.alpha * 0.06f; /* dim idle cell */
			uint32_t col = premul(rgb, inten);
			float cyp = botY - (r + 0.5f) * cell;
			quad(col, cxp - half, cyp + half, cxp + half, cyp + half,
			     cxp + half, cyp - half, cxp - half, cyp - half);
		}
	}
}

static void draw_wave(struct spectrum_state *s, const struct bg_audio_fft *fft,
		      float cx, float cy)
{
	int P = fft->wave_count;
	float R = s->radius * (1.0f + 0.2f * fft->bass);
	float amp = s->bar_len * 0.5f;
	float th = fmaxf(2.0f, s->radius * 0.02f);

	float px = 0.0f, py = 0.0f;
	for (int i = 0; i <= P; ++i) {
		int idx = i % P;
		float a = s->phase + (float)i / P * BG_TAU;
		float r = R + fft->wave[idx] * amp;
		float x = cx + r * cosf(a);
		float y = cy + r * sinf(a);
		if (i > 0) {
			float dx = x - px, dy = y - py;
			float l = sqrtf(dx * dx + dy * dy) + 1e-4f;
			float nx = -dy / l * th * 0.5f;
			float ny = dx / l * th * 0.5f;
			float rgb[3];
			grad_rgb(s, (float)idx / P, rgb);
			float mag = fabsf(fft->wave[idx]);
			uint32_t col = premul(rgb,
				s->common.alpha * (0.45f + 0.55f * mag));
			quad(col, px + nx, py + ny, x + nx, y + ny, x - nx,
			     y - ny, px - nx, py - ny);
		}
		px = x;
		py = y;
	}
}

static void spectrum_render(void *data, const struct bg_ctx *ctx)
{
	struct spectrum_state *s = data;
	const struct bg_audio_fft *fft = ctx->fft;
	float w = (float)ctx->width, h = (float)ctx->height;
	float cx = w * 0.5f, cy = h * 0.5f;

	/* Geometry (premultiplied alpha over any background). */
	if (s->viz && fft && fft->valid && s->mode != SPEC_BEAT) {
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		while (gs_effect_loop(s->viz, "Draw")) {
			gs_render_start(true);
			if (s->mode == SPEC_CIRCULAR)
				draw_circular(s, fft, cx, cy);
			else if (s->mode == SPEC_BARS)
				draw_bars(s, fft, w, h);
			else if (s->mode == SPEC_DOTS)
				draw_dots(s, fft, w, h);
			else if (s->mode == SPEC_WAVE)
				draw_wave(s, fft, cx, cy);
			gs_render_stop(GS_TRIS);
		}
		gs_blend_state_pop();
	}

	/* Beat-burst particles (circular & beat modes), additive glow. */
	if (s->sprite && (s->mode == SPEC_CIRCULAR || s->mode == SPEC_BEAT)) {
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ONE);
		bg_particles_render(s->sys, s->sprite, BG_SHAPE_SOFT, &s->post,
				    &ctx->audio);
		gs_blend_state_pop();
	}
}

/* ---- properties ---------------------------------------------------------- */

/* Show only the parameters the chosen display type actually uses. */
static void spectrum_apply_vis(obs_properties_t *props, int mode)
{
	char k[96];
	obs_property_t *p;
	bool circ = mode == SPEC_CIRCULAR, bars = mode == SPEC_BARS;
	bool wave = mode == SPEC_WAVE, beat = mode == SPEC_BEAT;
	bool dots = mode == SPEC_DOTS;
#define VIS(name, cond)                                                       \
	if ((p = obs_properties_get(props, bg_key(k, sizeof(k), PRE, name))))  \
	obs_property_set_visible(p, (cond))
	VIS("radius", circ || wave);
	VIS("bar_len", circ || bars || wave);
	VIS("bar_width", circ || bars);
	VIS("rotate", circ || wave);
	VIS("mirror", circ);
	VIS("dot_cols", dots);
	VIS("dot_size", dots);
	VIS("burst", circ || beat);
	VIS("burst_speed", circ || beat);
	/* The post (glow/bloom/…) only affects the burst particles. */
	VIS("glow", circ || beat);
	VIS("bloom", circ || beat);
	VIS("emissive", circ || beat);
	VIS("flare", circ || beat);
#undef VIS
}

static bool on_spectrum_mode(void *priv, obs_properties_t *props,
			     obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(priv);
	UNUSED_PARAMETER(prop);
	char k[96];
	spectrum_apply_vis(props, (int)obs_data_get_int(settings,
		bg_key(k, sizeof(k), PRE, "mode")));
	return true;
}

static void spectrum_properties(obs_properties_t *g, obs_data_t *settings)
{
	char k[96];
	obs_property_t *mode = obs_properties_add_list(g,
		bg_key(k, sizeof(k), PRE, "mode"),
		obs_module_text("SpectrumMode"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, obs_module_text("SpectrumModeCircular"),
				  SPEC_CIRCULAR);
	obs_property_list_add_int(mode, obs_module_text("SpectrumModeBars"),
				  SPEC_BARS);
	obs_property_list_add_int(mode, obs_module_text("SpectrumModeWave"),
				  SPEC_WAVE);
	obs_property_list_add_int(mode, obs_module_text("SpectrumModeBeat"),
				  SPEC_BEAT);
	obs_property_list_add_int(mode, obs_module_text("SpectrumModeDots"),
				  SPEC_DOTS);
	obs_property_set_modified_callback2(mode, on_spectrum_mode, NULL);

	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "radius"),
		obs_module_text("SpectrumRadius"), 20.0, 800.0, 1.0);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "bar_len"),
		obs_module_text("SpectrumBarLen"), 20.0, 1000.0, 1.0);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "bar_width"),
		obs_module_text("SpectrumBarWidth"), 0.1, 1.0, 0.01);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "rotate"),
		obs_module_text("SpectrumRotate"), -90.0, 90.0, 1.0);
	obs_properties_add_bool(g, bg_key(k, sizeof(k), PRE, "mirror"),
		obs_module_text("SpectrumMirror"));
	obs_properties_add_int_slider(g, bg_key(k, sizeof(k), PRE, "dot_cols"),
		obs_module_text("SpectrumDotCols"), 4, 128, 1);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "dot_size"),
		obs_module_text("SpectrumDotSize"), 4.0, 80.0, 1.0);

	bg_common_props(g, PRE, &k_spec);

	obs_properties_add_color(g, bg_key(k, sizeof(k), PRE, "color2"),
		obs_module_text("SpectrumColor2"));
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "grad"),
		obs_module_text("SpectrumGrad"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "burst"),
		obs_module_text("SpectrumBurst"), 0.0, 300.0, 1.0);
	obs_properties_add_float_slider(g,
		bg_key(k, sizeof(k), PRE, "burst_speed"),
		obs_module_text("SpectrumBurstSpeed"), 50.0, 1500.0, 10.0);

	bg_post_props(g, PRE);

	spectrum_apply_vis(g, settings ? (int)obs_data_get_int(settings,
		bg_key(k, sizeof(k), PRE, "mode")) : SPEC_CIRCULAR);
}

static void spectrum_defaults(obs_data_t *settings)
{
	char k[96];
	bg_common_defaults(settings, PRE, &k_spec);
	obs_data_set_default_int(settings, bg_key(k, sizeof(k), PRE, "mode"),
				 SPEC_CIRCULAR);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "radius"), 220.0);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "bar_len"), 260.0);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "bar_width"), 0.6);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "rotate"), 0.0);
	obs_data_set_default_bool(settings, bg_key(k, sizeof(k), PRE, "mirror"),
				  true);
	obs_data_set_default_int(settings, bg_key(k, sizeof(k), PRE, "dot_cols"),
				 32);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "dot_size"), 22.0);
	obs_data_set_default_int(settings, bg_key(k, sizeof(k), PRE, "color2"),
				 (long long)0xFFC864FF); /* pink (#FF64C8) */
	obs_data_set_default_double(settings, bg_key(k, sizeof(k), PRE, "grad"),
				    1.0);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "burst"), 80.0);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "burst_speed"), 500.0);
	bg_post_defaults(settings, PRE, 0.5, 0.4, 0.4, 0.0);
}

const struct bg_effect bgfx_spectrum = {
	.id             = "spectrum",
	.name_key       = "EffectSpectrum",
	.create         = spectrum_create,
	.destroy        = spectrum_destroy,
	.load_graphics  = spectrum_load_graphics,
	.update         = spectrum_update,
	.tick           = spectrum_tick,
	.render         = spectrum_render,
	.reset          = spectrum_reset,
	.get_properties = spectrum_properties,
	.get_defaults   = spectrum_defaults,
};
