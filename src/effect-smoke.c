#include "effect-smoke.h"
#include "bgfx-effect.h"
#include "bgfx-particles.h"
#include "bgfx-props.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>

#include <math.h>

#define PRE "smoke"
#define CAPACITY 4096
#define BG_TAU 6.28318530718f

static const struct bg_common_spec k_spec = {
	.size_min = 10.0, .size_max = 400.0, .size_step = 1.0, .size_def = 120.0,
	.life_min = 1.0, .life_max = 15.0, .life_def = 6.0,
	.rate_max = 200.0, .rate_def = 25.0,
	.max_cap = CAPACITY, .max_def = 800,
	.color_def = 0xFFFFFFFF, /* white smoke */
	.alpha_def = 0.30,
};

struct smoke_state {
	gs_effect_t *sprite;
	struct bg_particle_system *sys;

	struct bg_common common;
	struct bg_wind wind;
	float rise;  /* base drift speed, px/s  */
	float swirl; /* 0..1 curl-like sway     */
	int   flow;  /* enum bg_flow            */
	float fan;   /* 0..1 spread for BG_FLOW_UP_FAN */
};

static void *smoke_create(void)
{
	struct smoke_state *s = bzalloc(sizeof(*s));
	s->sys = bg_particles_create(CAPACITY);
	s->sys->fade_in = 0.15f;
	s->sys->fade_out = 0.35f;
	return s;
}

static void smoke_destroy(void *data)
{
	struct smoke_state *s = data;
	if (!s)
		return;
	/* Graphics lock held by the host. */
	if (s->sprite)
		gs_effect_destroy(s->sprite);
	bg_particles_destroy(s->sys);
	bfree(s);
}

static void smoke_load_graphics(void *data)
{
	struct smoke_state *s = data;
	s->sprite = bg_particles_load_effect();
}

static void smoke_update(void *data, obs_data_t *settings)
{
	struct smoke_state *s = data;
	char k[96];
	bg_common_update(&s->common, settings, PRE);
	bg_wind_update(&s->wind, settings, PRE);
	s->rise = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "rise"));
	s->swirl = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "swirl"));
	s->flow = bg_flow_update(settings, PRE);
	s->fan = bg_flow_fan_update(settings, PRE);
}

static void smoke_reset(void *data, uint32_t seed)
{
	struct smoke_state *s = data;
	bg_particles_reset(s->sys, seed);
}

static void smoke_spawn(struct smoke_state *s, const struct bg_ctx *ctx)
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

	/* Spawn on the edge the flow leaves from; drift speed along the flow,
	 * a small jitter across it. */
	float spd = s->rise * bg_frand_range(sys, 0.7f, 1.3f);
	float jit = bg_frand_range(sys, -20.0f, 20.0f);
	switch (s->flow) {
	case BG_FLOW_DOWN:
		p->x = bg_frand(sys) * w;
		p->y = -p->size * 0.5f;
		p->vx = jit;
		p->vy = spd;
		break;
	case BG_FLOW_LEFT:
		p->x = w + p->size * 0.5f;
		p->y = bg_frand(sys) * h;
		p->vx = -spd;
		p->vy = jit;
		break;
	case BG_FLOW_RIGHT:
		p->x = -p->size * 0.5f;
		p->y = bg_frand(sys) * h;
		p->vx = spd;
		p->vy = jit;
		break;
	case BG_FLOW_LR:
		p->x = w * 0.5f + w * 0.04f * (bg_frand(sys) * 2.0f - 1.0f);
		p->y = bg_frand(sys) * h;
		p->vx = bg_frand(sys) < 0.5f ? -spd : spd;
		p->vy = jit;
		break;
	case BG_FLOW_UP_FAN: {
		/* Rise from the whole bottom edge, leaning away from the
		 * canvas centre: puffs near the middle go straight up, the
		 * outer ones tilt outward up to ~75° at full spread. */
		p->x = bg_frand(sys) * w;
		p->y = h + p->size * 0.5f;
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
		/* Launch sideways from the whole bottom edge — away from the
		 * canvas centre — then curve into a climb (see tick). The
		 * spread setting scales the sideways launch speed, i.e. how
		 * far a puff flies before it turns upward. */
		p->x = bg_frand(sys) * w;
		p->y = h + p->size * 0.5f;
		p->dirsign = (p->x >= w * 0.5f) ? 1.0f : -1.0f;
		p->vx = p->dirsign * s->fan * 150.0f *
			bg_frand_range(sys, 0.6f, 1.3f);
		p->vy = -spd * bg_frand_range(sys, 0.1f, 0.3f);
		break;
	}
	case BG_FLOW_UP:
	default:
		p->x = bg_frand(sys) * w;
		p->y = h + p->size * 0.5f;
		p->vx = jit;
		p->vy = -spd;
		break;
	}
	p->max_life = p->life = bg_vary(sys, c->lifetime, c->life_var);
	p->rot = bg_frand(sys) * BG_TAU;
	p->vrot = bg_frand_range(sys, -0.5f, 0.5f);
	p->grow = bg_frand_range(sys, 1.6f, 2.4f); /* puffs expand as they rise */
	p->seed = bg_frand(sys);
	p->r = rgba[0];
	p->g = rgba[1];
	p->b = rgba[2];
	p->a = c->alpha * bg_frand_range(sys, 0.7f, 1.0f);
}

static void smoke_tick(void *data, const struct bg_ctx *ctx, float dt)
{
	struct smoke_state *s = data;
	struct bg_particle_system *sys = s->sys;
	sys->clock += dt;

	/* Emission (frame-rate independent spawn carry). */
	size_t cap = (size_t)(s->common.max_count < (int)sys->capacity
				      ? s->common.max_count
				      : (int)sys->capacity);
	sys->emit_accum += s->common.rate * dt;
	while (sys->emit_accum >= 1.0f && sys->live < cap) {
		sys->emit_accum -= 1.0f;
		smoke_spawn(s, ctx);
	}
	if (sys->emit_accum > 4.0f)
		sys->emit_accum = 4.0f;

	float t = ctx->time;
	bool horiz = bg_flow_horizontal(s->flow);
	for (size_t i = 0; i < sys->live; ++i) {
		bg_particle_t *p = &sys->items[i];

		float ax, ay;
		bg_wind_accel(&s->wind, t, p->seed, &ax, &ay);

		/* Curl-like sway across the flow direction, phased by the
		 * position along it, so columns of smoke wind as they drift. */
		if (s->swirl > 0.0f) {
			float along = horiz ? p->x : p->y;
			float ph = p->seed * BG_TAU + t * 0.8f +
				   along * 0.006f;
			float sway = s->swirl * 90.0f *
				     (sinf(ph) + 0.5f * sinf(ph * 2.7f));
			float push = s->swirl * 30.0f * cosf(ph * 1.3f);
			if (horiz) {
				ay += sway;
				ax += push;
			} else {
				ax += sway;
				ay += push;
			}
		}

		/* Fanning flow keeps pushing puffs outward as they rise, so
		 * the plume widens with height (drag below sets the limit). */
		if (s->flow == BG_FLOW_UP_FAN)
			ax += p->dirsign * s->fan * 60.0f;

		p->vx += ax * dt;
		p->vy += ay * dt;

		if (s->flow == BG_FLOW_UP_CURVE) {
			/* The sideways launch bleeds off while buoyancy eases
			 * the puff into a climb toward the flow speed, so each
			 * path bends from horizontal to vertical. Per-particle
			 * rates keep the silhouette from looking stamped. */
			float decay = 1.0f - (0.9f + 0.8f * p->seed) * dt;
			if (decay < 0.0f)
				decay = 0.0f;
			p->vx *= decay;
			float vtop = -s->rise * (0.8f + 0.4f * p->seed);
			if (p->vy > vtop)
				p->vy -= s->rise * 0.7f * dt;
		} else {
			/* Mild drag across the flow keeps gusts from
			 * accelerating the sideways wander forever. */
			float drag = 1.0f - 0.6f * dt;
			if (drag < 0.0f)
				drag = 0.0f;
			if (horiz)
				p->vy *= drag;
			else
				p->vx *= drag;
		}

		p->x += p->vx * dt;
		p->y += p->vy * dt;
		p->rot += p->vrot * dt;
		p->life -= dt;
	}
	bg_particles_compact(sys);
}

static void smoke_render(void *data, const struct bg_ctx *ctx)
{
	struct smoke_state *s = data;
	if (!s->sprite)
		return;
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	bg_particles_render(s->sys, s->sprite, BG_SHAPE_PUFF, NULL, &ctx->audio);
	gs_blend_state_pop();
}

static void smoke_properties(obs_properties_t *g, obs_data_t *settings)
{
	char k[96];
	bg_common_props(g, PRE, &k_spec);
	bg_flow_props(g, PRE, settings);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "rise"),
		obs_module_text("FlowSpeed"), 0.0, 400.0, 1.0);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "swirl"),
		obs_module_text("Swirl"), 0.0, 1.0, 0.01);
	bg_wind_props(g, PRE);
}

static void smoke_defaults(obs_data_t *settings)
{
	char k[96];
	bg_common_defaults(settings, PRE, &k_spec);
	bg_flow_defaults(settings, PRE);
	obs_data_set_default_double(settings, bg_key(k, sizeof(k), PRE, "rise"),
				    90.0);
	obs_data_set_default_double(settings, bg_key(k, sizeof(k), PRE, "swirl"),
				    0.5);
	bg_wind_defaults(settings, PRE);
}

const struct bg_effect bgfx_smoke = {
	.id             = "smoke",
	.name_key       = "EffectSmoke",
	.create         = smoke_create,
	.destroy        = smoke_destroy,
	.load_graphics  = smoke_load_graphics,
	.update         = smoke_update,
	.tick           = smoke_tick,
	.render         = smoke_render,
	.reset          = smoke_reset,
	.get_properties = smoke_properties,
	.get_defaults   = smoke_defaults,
};
