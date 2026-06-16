#include "effect-embers.h"
#include "bgfx-effect.h"
#include "bgfx-particles.h"
#include "bgfx-props.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>

#include <math.h>

#define PRE "embers"
#define CAPACITY 4096
#define BG_TAU 6.28318530718f

static const struct bg_common_spec k_spec = {
	.size_min = 1.0, .size_max = 30.0, .size_step = 0.5, .size_def = 4.0,
	.life_min = 0.5, .life_max = 10.0, .life_def = 4.0,
	.rate_max = 500.0, .rate_def = 80.0,
	.max_cap = CAPACITY, .max_def = 1500,
	.color_def = 0xFF2D7CFF, /* ember orange (#FF7C2D)  */
	.alpha_def = 1.0,
};

struct embers_state {
	gs_effect_t *sprite;
	struct bg_particle_system *sys;

	struct bg_common common;
	struct bg_wind wind;
	struct bg_post post;
	float rise;          /* base drift speed, px/s  */
	float flicker_depth; /* 0..1                     */
	float flicker_speed; /* cycles per second        */
	int   flow;          /* enum bg_flow             */
	float fan;           /* 0..1 spread for BG_FLOW_UP_FAN */
};

static void *embers_create(void)
{
	struct embers_state *s = bzalloc(sizeof(*s));
	s->sys = bg_particles_create(CAPACITY);
	s->sys->fade_in = 0.08f;
	s->sys->fade_out = 0.35f;
	return s;
}

static void embers_destroy(void *data)
{
	struct embers_state *s = data;
	if (!s)
		return;
	/* Graphics lock held by the host. */
	if (s->sprite)
		gs_effect_destroy(s->sprite);
	bg_particles_destroy(s->sys);
	bfree(s);
}

static void embers_load_graphics(void *data)
{
	struct embers_state *s = data;
	s->sprite = bg_particles_load_effect();
}

static void embers_update(void *data, obs_data_t *settings)
{
	struct embers_state *s = data;
	char k[96];
	bg_common_update(&s->common, settings, PRE);
	bg_wind_update(&s->wind, settings, PRE);
	bg_post_update(&s->post, settings, PRE);
	s->rise = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "rise"));
	s->flicker_depth = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "flicker"));
	s->flicker_speed = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "flicker_speed"));
	s->sys->flicker_speed = s->flicker_speed;
	s->flow = bg_flow_update(settings, PRE);
	s->fan = bg_flow_fan_update(settings, PRE);
}

static void embers_reset(void *data, uint32_t seed)
{
	struct embers_state *s = data;
	bg_particles_reset(s->sys, seed);
}

static void embers_spawn(struct embers_state *s, const struct bg_ctx *ctx)
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

	/* Lift off from a shallow band hugging the edge the flow leaves
	 * from; drift speed along the flow, a small jitter across it. */
	float spd = s->rise * bg_frand_range(sys, 0.6f, 1.4f);
	float jit = bg_frand_range(sys, -30.0f, 30.0f);
	switch (s->flow) {
	case BG_FLOW_DOWN:
		p->x = bg_frand(sys) * w;
		p->y = bg_frand(sys) * h * 0.06f;
		p->vx = jit;
		p->vy = spd;
		break;
	case BG_FLOW_LEFT:
		p->x = w - bg_frand(sys) * w * 0.06f;
		p->y = bg_frand(sys) * h;
		p->vx = -spd;
		p->vy = jit;
		break;
	case BG_FLOW_RIGHT:
		p->x = bg_frand(sys) * w * 0.06f;
		p->y = bg_frand(sys) * h;
		p->vx = spd;
		p->vy = jit;
		break;
	case BG_FLOW_LR:
		p->x = w * 0.5f + w * 0.03f * (bg_frand(sys) * 2.0f - 1.0f);
		p->y = bg_frand(sys) * h;
		p->vx = bg_frand(sys) < 0.5f ? -spd : spd;
		p->vy = jit;
		break;
	case BG_FLOW_UP_FAN: {
		/* Rise from the whole bottom band, leaning away from the
		 * canvas centre: embers near the middle climb straight up,
		 * the outer ones tilt outward up to ~75° at full spread. */
		p->x = bg_frand(sys) * w;
		p->y = h - bg_frand(sys) * h * 0.06f;
		float off = (p->x / w) * 2.0f - 1.0f; /* -1..1 from centre */
		p->dirsign = off; /* outward push scales with the offset */
		/* The launch angle saturates near horizontal (~86°); spread
		 * beyond 1 keeps acting through the outward push in tick. */
		float lean = s->fan * 1.3f;
		if (lean > 1.5f)
			lean = 1.5f;
		float theta = lean * off * bg_frand_range(sys, 0.5f, 1.0f);
		p->vx = sinf(theta) * spd;
		p->vy = -cosf(theta) * spd;
		break;
	}
	case BG_FLOW_UP_CURVE: {
		/* Launch sideways from the whole bottom band — away from the
		 * canvas centre — then curve into a climb (see tick). The
		 * spread setting scales the sideways launch speed. */
		p->x = bg_frand(sys) * w;
		p->y = h - bg_frand(sys) * h * 0.06f;
		p->dirsign = (p->x >= w * 0.5f) ? 1.0f : -1.0f;
		p->vx = p->dirsign * s->fan * 150.0f *
			bg_frand_range(sys, 0.6f, 1.3f);
		p->vy = -spd * bg_frand_range(sys, 0.1f, 0.3f);
		break;
	}
	case BG_FLOW_UP:
	default:
		p->x = bg_frand(sys) * w;
		p->y = h - bg_frand(sys) * h * 0.06f;
		p->vx = jit;
		p->vy = -spd;
		break;
	}
	p->max_life = p->life = bg_vary(sys, c->lifetime, c->life_var);
	p->grow = 0.75f; /* embers shrink as they cool */
	p->seed = bg_frand(sys);
	/* Nudge a random share of embers toward white-hot. */
	float hot = bg_frand(sys) * 0.25f;
	p->r = rgba[0] + hot;
	p->g = rgba[1] + hot * 0.6f;
	p->b = rgba[2] + hot * 0.3f;
	p->a = c->alpha * bg_frand_range(sys, 0.6f, 1.0f);
	p->flick = s->flicker_depth;
}

static void embers_tick(void *data, const struct bg_ctx *ctx, float dt)
{
	struct embers_state *s = data;
	struct bg_particle_system *sys = s->sys;
	sys->clock += dt;

	size_t cap = (size_t)(s->common.max_count < (int)sys->capacity
				      ? s->common.max_count
				      : (int)sys->capacity);
	sys->emit_accum += s->common.rate * dt;
	while (sys->emit_accum >= 1.0f && sys->live < cap) {
		sys->emit_accum -= 1.0f;
		embers_spawn(s, ctx);
	}
	if (sys->emit_accum > 4.0f)
		sys->emit_accum = 4.0f;

	float t = ctx->time;
	bool horiz = bg_flow_horizontal(s->flow);
	for (size_t i = 0; i < sys->live; ++i) {
		bg_particle_t *p = &sys->items[i];

		float ax, ay;
		bg_wind_accel(&s->wind, t, p->seed, &ax, &ay);

		/* Hot air wobble across the flow so embers never drift in
		 * straight lines. */
		float along = horiz ? p->x : p->y;
		float ph = p->seed * BG_TAU + t * 1.6f;
		float wob = 50.0f * sinf(ph + along * 0.01f);
		if (horiz)
			ay += wob;
		else
			ax += wob;

		/* Fanning flow keeps pushing embers outward as they climb. */
		if (s->flow == BG_FLOW_UP_FAN)
			ax += p->dirsign * s->fan * 50.0f;

		p->vx += ax * dt;
		p->vy += ay * dt;

		if (s->flow == BG_FLOW_UP_CURVE) {
			/* The sideways launch bleeds off while the rising heat
			 * eases each ember into a climb toward the flow speed,
			 * bending the path from horizontal to vertical. */
			float decay = 1.0f - (0.9f + 0.8f * p->seed) * dt;
			if (decay < 0.0f)
				decay = 0.0f;
			p->vx *= decay;
			float vtop = -s->rise * (0.8f + 0.4f * p->seed);
			if (p->vy > vtop)
				p->vy -= s->rise * 0.7f * dt;
		} else {
			float drag = 1.0f - 0.4f * dt;
			if (drag < 0.0f)
				drag = 0.0f;
			if (horiz)
				p->vy *= drag;
			else
				p->vx *= drag;
		}

		p->x += p->vx * dt;
		p->y += p->vy * dt;
		p->life -= dt;
	}
	bg_particles_compact(sys);
}

static void embers_render(void *data, const struct bg_ctx *ctx)
{
	struct embers_state *s = data;
	if (!s->sprite)
		return;
	/* Additive: embers are light sources. */
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ONE);
	bg_particles_render(s->sys, s->sprite, BG_SHAPE_SOFT, &s->post,
			    &ctx->audio);
	gs_blend_state_pop();
}

static void embers_properties(obs_properties_t *g, obs_data_t *settings)
{
	char k[96];
	bg_common_props(g, PRE, &k_spec);
	bg_flow_props(g, PRE, settings);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "rise"),
		obs_module_text("FlowSpeed"), 0.0, 500.0, 1.0);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "flicker"),
		obs_module_text("Flicker"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g,
		bg_key(k, sizeof(k), PRE, "flicker_speed"),
		obs_module_text("FlickerSpeed"), 0.5, 20.0, 0.1);
	bg_wind_props(g, PRE);
	bg_post_props(g, PRE);
}

static void embers_defaults(obs_data_t *settings)
{
	char k[96];
	bg_common_defaults(settings, PRE, &k_spec);
	bg_flow_defaults(settings, PRE);
	obs_data_set_default_double(settings, bg_key(k, sizeof(k), PRE, "rise"),
				    120.0);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "flicker"), 0.5);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "flicker_speed"), 8.0);
	bg_wind_defaults(settings, PRE);
	bg_post_defaults(settings, PRE, 0.4, 0.3, 0.3, 0.0);
}

const struct bg_effect bgfx_embers = {
	.id             = "embers",
	.name_key       = "EffectEmbers",
	.create         = embers_create,
	.destroy        = embers_destroy,
	.load_graphics  = embers_load_graphics,
	.update         = embers_update,
	.tick           = embers_tick,
	.render         = embers_render,
	.reset          = embers_reset,
	.get_properties = embers_properties,
	.get_defaults   = embers_defaults,
};
