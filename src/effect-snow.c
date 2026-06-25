/* Falling snow: soft circular particles drifting down from above the top edge,
 * fluttering side to side. Reuses the shared particle system (BG_SHAPE_CIRCLE
 * with an adjustable edge softness), wind, and post-light (glow/bloom/emissive/
 * lens-flare). Audio reactivity is applied centrally by bg_particles_render. */

#include "effect-snow.h"
#include "bgfx-particles.h"
#include "bgfx-props.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>

#include <math.h>

#define PRE "snow"
#define CAPACITY 4096
#define BG_TAU 6.28318530718f

static const struct bg_common_spec k_spec = {
	.size_min = 1.0, .size_max = 40.0, .size_step = 0.5, .size_def = 6.0,
	.life_min = 2.0, .life_max = 30.0, .life_def = 12.0,
	.rate_max = 600.0, .rate_def = 120.0,
	.max_cap = CAPACITY, .max_def = 1200,
	.color_def = 0xFFFFFFFF, /* white */
	.alpha_def = 0.9,
};

struct snow_state {
	gs_effect_t *sprite;
	struct bg_particle_system *sys;

	struct bg_common common;
	struct bg_wind wind;
	struct bg_post post;

	float fall;       /* base downward speed, px/s      */
	float sway;       /* flutter amplitude (px/s)        */
	float sway_speed; /* flutter cycles per second        */
	float scatter;    /* 0..1 randomness of flutter/fall  */
	float softness;   /* 0 crisp .. 1 soft round flake    */
};

static void *snow_create(void)
{
	struct snow_state *s = bzalloc(sizeof(*s));
	s->sys = bg_particles_create(CAPACITY);
	s->sys->fade_in = 0.05f;
	s->sys->fade_out = 0.15f;
	return s;
}

static void snow_destroy(void *data)
{
	struct snow_state *s = data;
	if (!s)
		return;
	if (s->sprite)
		gs_effect_destroy(s->sprite);
	bg_particles_destroy(s->sys);
	bfree(s);
}

static void snow_load_graphics(void *data)
{
	struct snow_state *s = data;
	s->sprite = bg_particles_load_effect();
}

static void snow_update(void *data, obs_data_t *settings)
{
	struct snow_state *s = data;
	char k[96];
	bg_common_update(&s->common, settings, PRE);
	bg_wind_update(&s->wind, settings, PRE);
	bg_post_update(&s->post, settings, PRE);
	s->fall = (float)obs_data_get_double(settings,
					     bg_key(k, sizeof(k), PRE, "fall"));
	s->sway = (float)obs_data_get_double(settings,
					     bg_key(k, sizeof(k), PRE, "sway"));
	s->sway_speed = (float)obs_data_get_double(
		settings, bg_key(k, sizeof(k), PRE, "sway_speed"));
	s->scatter = (float)obs_data_get_double(
		settings, bg_key(k, sizeof(k), PRE, "scatter"));
	s->softness = (float)obs_data_get_double(
		settings, bg_key(k, sizeof(k), PRE, "softness"));
	s->sys->softness = s->softness;
}

static void snow_reset(void *data, uint32_t seed)
{
	struct snow_state *s = data;
	bg_particles_reset(s->sys, seed);
}

static void snow_spawn(struct snow_state *s, const struct bg_ctx *ctx)
{
	bg_particle_t *p = bg_particles_spawn(s->sys);
	if (!p)
		return;
	struct bg_particle_system *sys = s->sys;
	const struct bg_common *c = &s->common;

	float rgba[4];
	bg_unpack_color(c->color, rgba);

	/* Spawn across a band just above the top edge so flakes drift in. */
	p->x = bg_frand(sys) * (float)ctx->width;
	p->y = -bg_frand_range(sys, 0.0f, 64.0f);
	p->size = bg_vary(sys, c->size, c->size_var) * 0.5f;

	float fv = 1.0f + s->scatter * (bg_frand(sys) * 0.8f - 0.4f);
	p->vy = s->fall * fv;
	p->vx = 0.0f;

	p->max_life = p->life = bg_vary(sys, c->lifetime, c->life_var);
	p->seed = bg_frand(sys);
	/* Per-particle flutter speed / amplitude factors widened by scatter. */
	p->aux0 = 1.0f + s->scatter * (bg_frand(sys) * 1.2f - 0.6f);
	p->aux1 = 1.0f + s->scatter * (bg_frand(sys) * 1.4f - 0.7f);

	p->r = rgba[0];
	p->g = rgba[1];
	p->b = rgba[2];
	p->a = c->alpha * bg_frand_range(sys, 0.6f, 1.0f);
}

static void snow_tick(void *data, const struct bg_ctx *ctx, float dt)
{
	struct snow_state *s = data;
	struct bg_particle_system *sys = s->sys;
	sys->clock += dt;

	size_t cap = (size_t)(s->common.max_count < (int)sys->capacity
				      ? s->common.max_count
				      : (int)sys->capacity);
	sys->emit_accum += s->common.rate * dt;
	while (sys->emit_accum >= 1.0f && sys->live < cap) {
		sys->emit_accum -= 1.0f;
		snow_spawn(s, ctx);
	}
	if (sys->emit_accum > 4.0f)
		sys->emit_accum = 4.0f;

	float w = (float)ctx->width;
	float t = ctx->time;
	for (size_t i = 0; i < sys->live; ++i) {
		bg_particle_t *p = &sys->items[i];

		float ax, ay;
		bg_wind_accel(&s->wind, t, p->seed, &ax, &ay);
		p->vx += ax * dt;
		p->vy += ay * dt;

		/* Side-to-side flutter: an oscillating horizontal velocity whose
		 * phase/speed/amplitude vary per flake (scatter widens both). */
		float ph = p->seed * BG_TAU + t * s->sway_speed * p->aux0;
		float flutter = s->sway * p->aux1 * sinf(ph);

		p->x += (p->vx + flutter) * dt;
		p->y += p->vy * dt;
		p->life -= dt;

		/* Wrap horizontally so wind never sweeps the field off one side. */
		if (w > 1.0f) {
			if (p->x < -64.0f)
				p->x += w + 128.0f;
			else if (p->x > w + 64.0f)
				p->x -= w + 128.0f;
		}
		/* Retire flakes that fall below the bottom edge. */
		if (p->y > (float)ctx->height + 64.0f)
			p->life = 0.0f;
	}
	bg_particles_compact(sys);
}

static void snow_render(void *data, const struct bg_ctx *ctx)
{
	struct snow_state *s = data;
	if (!s->sprite)
		return;
	/* Premultiplied alpha over the background: white flakes stay visible on
	 * any backdrop while the glow/bloom still add light. */
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	bg_particles_render(s->sys, s->sprite, BG_SHAPE_CIRCLE, &s->post,
			    &ctx->audio);
	gs_blend_state_pop();
}

static void snow_properties(obs_properties_t *g, obs_data_t *settings)
{
	char k[96];
	UNUSED_PARAMETER(settings);
	bg_common_props(g, PRE, &k_spec);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "fall"),
					obs_module_text("SnowFall"), 0.0, 600.0,
					1.0);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "sway"),
					obs_module_text("SnowSway"), 0.0, 300.0,
					1.0);
	obs_properties_add_float_slider(g,
					bg_key(k, sizeof(k), PRE, "sway_speed"),
					obs_module_text("SnowSwaySpeed"), 0.0,
					6.0, 0.05);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "scatter"),
					obs_module_text("SnowScatter"), 0.0, 1.0,
					0.01);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "softness"),
					obs_module_text("SnowSoftness"), 0.0,
					1.0, 0.01);
	bg_wind_props(g, PRE);
	bg_post_props(g, PRE);
}

static void snow_defaults(obs_data_t *settings)
{
	char k[96];
	bg_common_defaults(settings, PRE, &k_spec);
	obs_data_set_default_double(settings, bg_key(k, sizeof(k), PRE, "fall"),
				    90.0);
	obs_data_set_default_double(settings, bg_key(k, sizeof(k), PRE, "sway"),
				    40.0);
	obs_data_set_default_double(
		settings, bg_key(k, sizeof(k), PRE, "sway_speed"), 1.2);
	obs_data_set_default_double(settings,
				    bg_key(k, sizeof(k), PRE, "scatter"), 0.5);
	obs_data_set_default_double(settings,
				    bg_key(k, sizeof(k), PRE, "softness"), 0.4);
	bg_wind_defaults(settings, PRE);
	bg_post_defaults(settings, PRE, 0.3, 0.15, 0.1, 0.0);
}

const struct bg_effect bgfx_snow = {
	.id             = "snow",
	.name_key       = "EffectSnow",
	.create         = snow_create,
	.destroy        = snow_destroy,
	.load_graphics  = snow_load_graphics,
	.update         = snow_update,
	.tick           = snow_tick,
	.render         = snow_render,
	.reset          = snow_reset,
	.get_properties = snow_properties,
	.get_defaults   = snow_defaults,
};
