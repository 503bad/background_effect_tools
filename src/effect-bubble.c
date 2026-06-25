#include "effect-bubble.h"
#include "bgfx-effect.h"
#include "bgfx-particles.h"
#include "bgfx-props.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>

#include <math.h>

#define PRE "bubble"
#define CAPACITY 1024
#define BG_TAU 6.28318530718f

static const struct bg_common_spec k_spec = {
	.size_min = 8.0, .size_max = 320.0, .size_step = 1.0, .size_def = 70.0,
	.life_min = 1.0, .life_max = 20.0, .life_def = 8.0,
	.rate_max = 120.0, .rate_def = 14.0,
	.max_cap = CAPACITY, .max_def = 120,
	.color_def = 0xFFC896FF, /* pink (#FF96C8) */
	.alpha_def = 0.85,
	.size_var_max = 100,
};

struct bubble_state {
	gs_effect_t *sprite;
	struct bg_particle_system *sys;

	struct bg_common common;
	struct bg_post post;

	uint32_t color2;    /* second tint, bubbles lerp between A and B */
	float    rise;      /* upward drift, px/s                        */
	float    wobble;    /* sideways sway amplitude, px/s             */
	float    wob_speed; /* sway cycles per second                    */
	float    gloss;     /* specular highlight strength 0..1          */
	float    rim_width; /* outline thickness, fraction of radius     */
};

static void *bubble_create(void)
{
	struct bubble_state *s = bzalloc(sizeof(*s));
	s->sys = bg_particles_create(CAPACITY);
	s->sys->fade_in = 0.15f;
	s->sys->fade_out = 0.2f;
	return s;
}

static void bubble_destroy(void *data)
{
	struct bubble_state *s = data;
	if (!s)
		return;
	if (s->sprite)
		gs_effect_destroy(s->sprite);
	bg_particles_destroy(s->sys);
	bfree(s);
}

static void bubble_load_graphics(void *data)
{
	struct bubble_state *s = data;
	s->sprite = bg_particles_load_effect();
}

static void bubble_update(void *data, obs_data_t *settings)
{
	struct bubble_state *s = data;
	char k[96];
	bg_common_update(&s->common, settings, PRE);
	bg_post_update(&s->post, settings, PRE);
	s->color2 = (uint32_t)obs_data_get_int(settings,
		bg_key(k, sizeof(k), PRE, "color2"));
	s->rise = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "rise"));
	s->wobble = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "wobble"));
	s->wob_speed = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "wob_speed"));
	s->gloss = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "gloss"));
	s->sys->gloss = s->gloss;
	s->rim_width = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "rim_width"));
	s->sys->rim_width = s->rim_width;
}

static void bubble_reset(void *data, uint32_t seed)
{
	struct bubble_state *s = data;
	bg_particles_reset(s->sys, seed);
}

static void bubble_spawn(struct bubble_state *s, const struct bg_ctx *ctx)
{
	bg_particle_t *p = bg_particles_spawn(s->sys);
	if (!p)
		return;
	struct bg_particle_system *sys = s->sys;
	const struct bg_common *c = &s->common;

	float ca[4], cb[4];
	bg_unpack_color(c->color, ca);
	bg_unpack_color(s->color2, cb);

	float w = (float)ctx->width, h = (float)ctx->height;
	p->size = bg_vary(sys, c->size, c->size_var) * 0.5f;
	p->x = bg_frand(sys) * w;
	p->y = h + p->size; /* start just below the bottom edge */

	/* Gentle ascent with a little spread in speed; a faint sideways drift. */
	p->vy = -s->rise * bg_frand_range(sys, 0.7f, 1.2f);
	p->vx = bg_frand_range(sys, -8.0f, 8.0f);

	p->max_life = p->life = bg_vary(sys, c->lifetime, c->life_var);
	p->grow = 1.06f; /* swell slightly as they rise */
	p->seed = bg_frand(sys);
	p->aux0 = bg_frand(sys) * BG_TAU; /* sway phase offset */

	/* Tint each bubble somewhere between the two colours. */
	float t = bg_frand(sys);
	p->r = ca[0] + (cb[0] - ca[0]) * t;
	p->g = ca[1] + (cb[1] - ca[1]) * t;
	p->b = ca[2] + (cb[2] - ca[2]) * t;
	p->a = c->alpha * bg_frand_range(sys, 0.8f, 1.0f);
}

static void bubble_tick(void *data, const struct bg_ctx *ctx, float dt)
{
	struct bubble_state *s = data;
	struct bg_particle_system *sys = s->sys;
	sys->clock += dt;

	size_t cap = (size_t)(s->common.max_count < (int)sys->capacity
				      ? s->common.max_count
				      : (int)sys->capacity);
	sys->emit_accum += s->common.rate * dt;
	while (sys->emit_accum >= 1.0f && sys->live < cap) {
		sys->emit_accum -= 1.0f;
		bubble_spawn(s, ctx);
	}
	if (sys->emit_accum > 4.0f)
		sys->emit_accum = 4.0f;

	float t = ctx->time;
	for (size_t i = 0; i < sys->live; ++i) {
		bg_particle_t *p = &sys->items[i];
		/* Soft side-to-side sway plus a slow buoyancy bob: ふわふわ. */
		float ph = t * s->wob_speed + p->aux0;
		float sway = s->wobble * sinf(ph);
		float bob = 0.15f * s->rise * sinf(ph * 0.5f + p->seed);
		p->x += (p->vx + sway) * dt;
		p->y += (p->vy + bob) * dt;
		p->rot += 0.2f * dt;
		p->life -= dt;
	}
	bg_particles_compact(sys);
}

static void bubble_render(void *data, const struct bg_ctx *ctx)
{
	struct bubble_state *s = data;
	if (!s->sprite)
		return;
	/* Translucent: composite the premultiplied bubbles over the scene. */
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	bg_particles_render(s->sys, s->sprite, BG_SHAPE_BUBBLE, &s->post,
			    &ctx->audio);
	gs_blend_state_pop();
}

static void bubble_properties(obs_properties_t *g, obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);
	char k[96];
	bg_common_props(g, PRE, &k_spec);

	obs_properties_add_color(g, bg_key(k, sizeof(k), PRE, "color2"),
		obs_module_text("BubbleColor2"));
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "rise"),
		obs_module_text("BubbleRise"), 0.0, 300.0, 1.0);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "wobble"),
		obs_module_text("BubbleWobble"), 0.0, 120.0, 1.0);
	obs_properties_add_float_slider(g,
		bg_key(k, sizeof(k), PRE, "wob_speed"),
		obs_module_text("BubbleWobbleSpeed"), 0.1, 5.0, 0.05);
	obs_properties_add_float_slider(g,
		bg_key(k, sizeof(k), PRE, "rim_width"),
		obs_module_text("BubbleRimWidth"), 0.02, 0.6, 0.01);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "gloss"),
		obs_module_text("BubbleGloss"), 0.0, 1.0, 0.01);

	bg_post_props(g, PRE);
}

static void bubble_defaults(obs_data_t *settings)
{
	char k[96];
	bg_common_defaults(settings, PRE, &k_spec);
	/* Larger default variation suits the big bubbles. */
	obs_data_set_default_int(settings, bg_key(k, sizeof(k), PRE, "size_var"),
				 55);
	obs_data_set_default_int(settings, bg_key(k, sizeof(k), PRE, "color2"),
				 (long long)0xFFE678AA); /* purple (#AA78E6) */
	obs_data_set_default_double(settings, bg_key(k, sizeof(k), PRE, "rise"),
				    60.0);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "wobble"), 30.0);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "wob_speed"), 1.0);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "rim_width"), 0.18);
	obs_data_set_default_double(settings, bg_key(k, sizeof(k), PRE, "gloss"),
				    0.8);
	bg_post_defaults(settings, PRE, 0.35, 0.2, 0.0, 0.0);
}

const struct bg_effect bgfx_bubble = {
	.id             = "bubble",
	.name_key       = "EffectBubble",
	.create         = bubble_create,
	.destroy        = bubble_destroy,
	.load_graphics  = bubble_load_graphics,
	.update         = bubble_update,
	.tick           = bubble_tick,
	.render         = bubble_render,
	.reset          = bubble_reset,
	.get_properties = bubble_properties,
	.get_defaults   = bubble_defaults,
};
