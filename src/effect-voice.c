#include "effect-voice.h"
#include "bgfx-effect.h"
#include "bgfx-particles.h"
#include "bgfx-props.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>

#include <math.h>

#define PRE "voice"
#define CAPACITY 6000
#define BG_TAU 6.28318530718f
#define DEG2RAD (BG_TAU / 360.0f)

static const struct bg_common_spec k_spec = {
	.size_min = 1.0, .size_max = 60.0, .size_step = 0.5, .size_def = 6.0,
	.life_min = 0.3, .life_max = 8.0, .life_def = 2.0,
	.rate_max = 3000.0, .rate_def = 900.0,
	.max_cap = CAPACITY, .max_def = 3000,
	.color_def = 0xFF66E0FF, /* warm yellow (#FFE066) */
	.alpha_def = 1.0,
	.size_var_max = 100,
};

struct voice_state {
	gs_effect_t *sprite;
	struct bg_particle_system *sys;

	struct bg_common common;
	struct bg_post post;

	float threshold; /* 0..1 erupt level                          */
	float angle;     /* jet centre, deg (0 right, 90 up, 180 left) */
	float spread;    /* half cone, deg                            */
	float speed;     /* launch speed, px/s                        */
	float gravity;   /* downward accel, px/s²                     */
	bool  band_on;   /* restrict reaction to [band_lo, band_hi]   */
	float band_lo;   /* Hz                                        */
	float band_hi;   /* Hz                                        */

	float prev_lvl;  /* last frame's gated level (edge detect)    */
};

static void *voice_create(void)
{
	struct voice_state *s = bzalloc(sizeof(*s));
	s->sys = bg_particles_create(CAPACITY);
	s->sys->fade_in = 0.04f;
	s->sys->fade_out = 0.3f;
	return s;
}

static void voice_destroy(void *data)
{
	struct voice_state *s = data;
	if (!s)
		return;
	if (s->sprite)
		gs_effect_destroy(s->sprite);
	bg_particles_destroy(s->sys);
	bfree(s);
}

static void voice_load_graphics(void *data)
{
	struct voice_state *s = data;
	s->sprite = bg_particles_load_effect();
}

static void voice_update(void *data, obs_data_t *settings)
{
	struct voice_state *s = data;
	char k[96];
	bg_common_update(&s->common, settings, PRE);
	bg_post_update(&s->post, settings, PRE);
	s->threshold = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "threshold"));
	s->angle = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "angle"));
	s->spread = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "spread"));
	s->speed = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "speed"));
	s->gravity = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "gravity"));
	s->band_on = obs_data_get_bool(settings,
		bg_key(k, sizeof(k), PRE, "band_on"));
	s->band_lo = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "band_lo"));
	s->band_hi = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "band_hi"));
}

static void voice_reset(void *data, uint32_t seed)
{
	struct voice_state *s = data;
	bg_particles_reset(s->sys, seed);
	s->prev_lvl = 0.0f;
}

static void voice_spawn(struct voice_state *s, const struct bg_ctx *ctx,
			float drive)
{
	bg_particle_t *p = bg_particles_spawn(s->sys);
	if (!p)
		return;
	struct bg_particle_system *sys = s->sys;
	const struct bg_common *c = &s->common;

	float rgba[4];
	bg_unpack_color(c->color, rgba);

	float w = (float)ctx->width, h = (float)ctx->height;
	p->size = bg_vary(sys, c->size, c->size_var) * 0.5f;
	p->x = bg_frand(sys) * w;
	p->y = h - bg_frand(sys) * h * 0.02f; /* hug the bottom edge */

	/* Jet direction: angle ± spread, screen y grows down so up is -sin. */
	float ang = (s->angle + bg_frand_range(sys, -s->spread, s->spread)) *
		    DEG2RAD;
	/* Louder moments launch faster, for a punchier eruption. */
	float spd = s->speed * bg_frand_range(sys, 0.7f, 1.15f) *
		    (0.6f + 0.6f * drive);
	p->vx = cosf(ang) * spd;
	p->vy = -sinf(ang) * spd;

	p->max_life = p->life = bg_vary(sys, c->lifetime, c->life_var);
	p->grow = 0.7f;
	p->seed = bg_frand(sys);
	/* A share of the spray flares white-hot at the loudest hits. */
	float hot = bg_frand(sys) * 0.3f * drive;
	p->r = rgba[0] + hot;
	p->g = rgba[1] + hot * 0.7f;
	p->b = rgba[2] + hot * 0.4f;
	p->a = c->alpha * bg_frand_range(sys, 0.7f, 1.0f);
}

static void voice_tick(void *data, const struct bg_ctx *ctx, float dt)
{
	struct voice_state *s = data;
	struct bg_particle_system *sys = s->sys;
	sys->clock += dt;

	/* Gated drive: how far the chosen band pushes past the threshold. */
	const struct bg_audio_fft *fft = ctx->fft;
	float lvl;
	if (s->band_on)
		lvl = bg_fft_band(fft, s->band_lo, s->band_hi);
	else
		lvl = (fft && fft->valid) ? fft->level : ctx->audio.level;

	float thr = s->threshold;
	float drive = 0.0f;
	if (lvl > thr) {
		float head = 1.0f - thr;
		drive = head > 1e-3f ? (lvl - thr) / head : 1.0f;
		if (drive > 1.0f)
			drive = 1.0f;
	}

	size_t cap = (size_t)(s->common.max_count < (int)sys->capacity
				      ? s->common.max_count
				      : (int)sys->capacity);

	/* Continuous spray while above the threshold, scaled by the drive. */
	if (drive > 0.0f) {
		sys->emit_accum += s->common.rate * drive * dt;
		/* Punchy onset: an extra burst on the frame we cross up. */
		if (s->prev_lvl <= thr)
			sys->emit_accum += s->common.rate * 0.05f * (0.5f + drive);
	}
	while (sys->emit_accum >= 1.0f && sys->live < cap) {
		sys->emit_accum -= 1.0f;
		voice_spawn(s, ctx, drive);
	}
	if (sys->emit_accum > 8.0f)
		sys->emit_accum = 8.0f;
	s->prev_lvl = lvl;

	float g = s->gravity;
	for (size_t i = 0; i < sys->live; ++i) {
		bg_particle_t *p = &sys->items[i];
		p->vy += g * dt;
		p->x += p->vx * dt;
		p->y += p->vy * dt;
		p->life -= dt;
	}
	bg_particles_compact(sys);
}

static void voice_render(void *data, const struct bg_ctx *ctx)
{
	struct voice_state *s = data;
	if (!s->sprite)
		return;
	/* Additive: the spray reads as light. */
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ONE);
	bg_particles_render(s->sys, s->sprite, BG_SHAPE_SOFT, &s->post,
			    &ctx->audio);
	gs_blend_state_pop();
}

/* The band min/max sliders only matter when the band filter is on. */
static bool on_band_changed(void *priv, obs_properties_t *props,
			    obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(priv);
	UNUSED_PARAMETER(prop);
	char k[96];
	bool on = obs_data_get_bool(settings, bg_key(k, sizeof(k), PRE,
						     "band_on"));
	obs_property_t *lo = obs_properties_get(props,
		bg_key(k, sizeof(k), PRE, "band_lo"));
	obs_property_t *hi = obs_properties_get(props,
		bg_key(k, sizeof(k), PRE, "band_hi"));
	if (lo)
		obs_property_set_visible(lo, on);
	if (hi)
		obs_property_set_visible(hi, on);
	return true;
}

static void voice_properties(obs_properties_t *g, obs_data_t *settings)
{
	char k[96];
	bg_common_props(g, PRE, &k_spec);

	obs_properties_add_float_slider(g,
		bg_key(k, sizeof(k), PRE, "threshold"),
		obs_module_text("VoiceThreshold"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "angle"),
		obs_module_text("VoiceAngle"), 0.0, 180.0, 1.0);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "spread"),
		obs_module_text("VoiceSpread"), 0.0, 90.0, 1.0);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "speed"),
		obs_module_text("VoiceSpeed"), 50.0, 2500.0, 10.0);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "gravity"),
		obs_module_text("VoiceGravity"), 0.0, 3000.0, 10.0);

	obs_property_t *bon = obs_properties_add_bool(g,
		bg_key(k, sizeof(k), PRE, "band_on"),
		obs_module_text("VoiceBandOn"));
	obs_property_set_modified_callback2(bon, on_band_changed, NULL);
	obs_property_t *lo = obs_properties_add_float_slider(g,
		bg_key(k, sizeof(k), PRE, "band_lo"),
		obs_module_text("VoiceBandLo"), 0.0, 20000.0, 5.0);
	obs_property_t *hi = obs_properties_add_float_slider(g,
		bg_key(k, sizeof(k), PRE, "band_hi"),
		obs_module_text("VoiceBandHi"), 0.0, 20000.0, 5.0);

	bool on = settings && obs_data_get_bool(settings,
		bg_key(k, sizeof(k), PRE, "band_on"));
	obs_property_set_visible(lo, on);
	obs_property_set_visible(hi, on);

	bg_post_props(g, PRE);
}

static void voice_defaults(obs_data_t *settings)
{
	char k[96];
	bg_common_defaults(settings, PRE, &k_spec);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "threshold"), 0.18);
	obs_data_set_default_double(settings, bg_key(k, sizeof(k), PRE, "angle"),
				    90.0);
	obs_data_set_default_double(settings, bg_key(k, sizeof(k), PRE, "spread"),
				    25.0);
	obs_data_set_default_double(settings, bg_key(k, sizeof(k), PRE, "speed"),
				    900.0);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "gravity"), 900.0);
	obs_data_set_default_bool(settings, bg_key(k, sizeof(k), PRE, "band_on"),
				  false);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "band_lo"), 40.0);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "band_hi"), 150.0);
	bg_post_defaults(settings, PRE, 0.5, 0.4, 0.4, 0.0);
}

const struct bg_effect bgfx_voice = {
	.id             = "voice",
	.name_key       = "EffectVoice",
	.create         = voice_create,
	.destroy        = voice_destroy,
	.load_graphics  = voice_load_graphics,
	.update         = voice_update,
	.tick           = voice_tick,
	.render         = voice_render,
	.reset          = voice_reset,
	.get_properties = voice_properties,
	.get_defaults   = voice_defaults,
};
