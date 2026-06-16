#include "effect-gravity.h"
#include "bgfx-effect.h"
#include "bgfx-particles.h"
#include "bgfx-props.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>

#include <math.h>

#define PRE "gravity"
#define CAPACITY 8192
#define BG_TAU 6.28318530718f

/* Spawn regions. */
enum {
	SPAWN_TOP = 0,
	SPAWN_FULL = 1,
	SPAWN_LEFT = 2,
	SPAWN_RIGHT = 3,
	SPAWN_BOTTOM = 4,
	SPAWN_CENTER_V = 5, /* vertical line through the canvas centre   */
	SPAWN_CENTER_H = 6, /* horizontal line through the canvas centre */
};

static const struct bg_common_spec k_spec = {
	.size_min = 1.0, .size_max = 60.0, .size_step = 0.5, .size_def = 3.0,
	.life_min = 0.5, .life_max = 20.0, .life_def = 5.0,
	.rate_max = 2000.0, .rate_def = 200.0,
	.max_cap = CAPACITY, .max_def = 2000,
	.color_def = 0xFFE8D8C8, /* rainy pale blue (#C8D8E8) */
	.alpha_def = 0.8,
};

struct gravity_state {
	gs_effect_t *sprite;
	struct bg_particle_system *sys;

	struct bg_common common;
	struct bg_post post;
	float grav_dir;   /* degrees; 0 = right, 90 = up, 270 = down */
	float grav_accel; /* px/s²                                    */
	float init_speed; /* px/s                                     */
	float init_dir;   /* degrees                                  */
	float init_spread;/* 0..1 angular randomness                  */
	float stretch;    /* 0..1 velocity stretch (rain streaks)     */
	int   spawn_area;
	bool  mirror_lr;  /* mirror half the particles horizontally,
			   * e.g. centre spawn flowing out to both sides */
};

static void *gravity_create(void)
{
	struct gravity_state *s = bzalloc(sizeof(*s));
	s->sys = bg_particles_create(CAPACITY);
	s->sys->fade_in = 0.05f;
	s->sys->fade_out = 0.15f;
	return s;
}

static void gravity_destroy(void *data)
{
	struct gravity_state *s = data;
	if (!s)
		return;
	/* Graphics lock held by the host. */
	if (s->sprite)
		gs_effect_destroy(s->sprite);
	bg_particles_destroy(s->sys);
	bfree(s);
}

static void gravity_load_graphics(void *data)
{
	struct gravity_state *s = data;
	s->sprite = bg_particles_load_effect();
}

static void gravity_update(void *data, obs_data_t *settings)
{
	struct gravity_state *s = data;
	char k[96];
	bg_common_update(&s->common, settings, PRE);
	bg_post_update(&s->post, settings, PRE);
	s->grav_dir = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "dir"));
	s->grav_accel = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "accel"));
	s->init_speed = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "init_speed"));
	s->init_dir = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "init_dir"));
	s->init_spread = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "init_spread"));
	s->stretch = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "stretch"));
	s->spawn_area = (int)obs_data_get_int(settings,
		bg_key(k, sizeof(k), PRE, "spawn_area"));
	s->mirror_lr = obs_data_get_bool(settings,
		bg_key(k, sizeof(k), PRE, "mirror_lr"));
}

static void gravity_reset(void *data, uint32_t seed)
{
	struct gravity_state *s = data;
	bg_particles_reset(s->sys, seed);
}

static void gravity_spawn(struct gravity_state *s, const struct bg_ctx *ctx)
{
	bg_particle_t *p = bg_particles_spawn(s->sys);
	if (!p)
		return;
	struct bg_particle_system *sys = s->sys;
	const struct bg_common *c = &s->common;
	float w = (float)ctx->width, h = (float)ctx->height;

	p->size = bg_vary(sys, c->size, c->size_var) * 0.5f;

	float m = p->size + 4.0f; /* just outside the canvas */
	switch (s->spawn_area) {
	case SPAWN_FULL:
		p->x = bg_frand(sys) * w;
		p->y = bg_frand(sys) * h;
		break;
	case SPAWN_LEFT:
		p->x = -m;
		p->y = bg_frand(sys) * h;
		break;
	case SPAWN_RIGHT:
		p->x = w + m;
		p->y = bg_frand(sys) * h;
		break;
	case SPAWN_BOTTOM:
		p->x = bg_frand(sys) * w;
		p->y = h + m;
		break;
	case SPAWN_CENTER_V:
		p->x = w * 0.5f + w * 0.02f * (bg_frand(sys) * 2.0f - 1.0f);
		p->y = bg_frand(sys) * h;
		break;
	case SPAWN_CENTER_H:
		p->x = bg_frand(sys) * w;
		p->y = h * 0.5f + h * 0.02f * (bg_frand(sys) * 2.0f - 1.0f);
		break;
	case SPAWN_TOP:
	default:
		p->x = bg_frand(sys) * w;
		p->y = -m;
		break;
	}

	/* Mirroring flips the horizontal component of the initial velocity
	 * and (in tick) of the gravity pull for half the particles, so a
	 * centre spawn streams out to both sides. */
	if (s->mirror_lr && bg_frand(sys) < 0.5f)
		p->dirsign = -1.0f;

	float rad = s->init_dir * (BG_TAU / 360.0f) +
		    s->init_spread * 3.14159265f *
			    (bg_frand(sys) * 2.0f - 1.0f);
	float spd = s->init_speed * bg_frand_range(sys, 0.7f, 1.3f);
	p->vx = cosf(rad) * spd * p->dirsign;
	p->vy = -sinf(rad) * spd; /* 90° = up on screen */

	p->max_life = p->life = bg_vary(sys, c->lifetime, c->life_var);
	p->seed = bg_frand(sys);

	float rgba[4];
	bg_unpack_color(c->color, rgba);
	p->r = rgba[0];
	p->g = rgba[1];
	p->b = rgba[2];
	p->a = c->alpha * bg_frand_range(sys, 0.7f, 1.0f);
}

static void gravity_tick(void *data, const struct bg_ctx *ctx, float dt)
{
	struct gravity_state *s = data;
	struct bg_particle_system *sys = s->sys;
	sys->clock += dt;

	size_t cap = (size_t)(s->common.max_count < (int)sys->capacity
				      ? s->common.max_count
				      : (int)sys->capacity);
	sys->emit_accum += s->common.rate * dt;
	while (sys->emit_accum >= 1.0f && sys->live < cap) {
		sys->emit_accum -= 1.0f;
		gravity_spawn(s, ctx);
	}
	if (sys->emit_accum > 8.0f)
		sys->emit_accum = 8.0f;

	float rad = s->grav_dir * (BG_TAU / 360.0f);
	float gx = cosf(rad) * s->grav_accel;
	float gy = -sinf(rad) * s->grav_accel; /* 90° = up on screen */

	float w = (float)ctx->width, h = (float)ctx->height;
	const float margin = 250.0f;

	for (size_t i = 0; i < sys->live; ++i) {
		bg_particle_t *p = &sys->items[i];
		p->vx += gx * p->dirsign * dt;
		p->vy += gy * dt;
		p->x += p->vx * dt;
		p->y += p->vy * dt;

		if (s->stretch > 0.0f) {
			float spd = sqrtf(p->vx * p->vx + p->vy * p->vy);
			p->len = p->size * (1.0f + s->stretch * spd * 0.02f);
		} else {
			p->len = 0.0f;
		}

		/* Retire anything well past the canvas. */
		if (p->x < -margin || p->x > w + margin || p->y < -margin ||
		    p->y > h + margin)
			p->life = 0.0f;
		else
			p->life -= dt;
	}
	bg_particles_compact(sys);
}

static void gravity_render(void *data, const struct bg_ctx *ctx)
{
	struct gravity_state *s = data;
	if (!s->sprite)
		return;
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	bg_particles_render(s->sys, s->sprite, BG_SHAPE_SOFT, &s->post,
			    &ctx->audio);
	gs_blend_state_pop();
}

static void gravity_properties(obs_properties_t *g, obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);
	char k[96];
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "dir"),
		obs_module_text("GravityDir"), 0.0, 360.0, 1.0);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "accel"),
		obs_module_text("GravityAccel"), 0.0, 3000.0, 10.0);
	obs_properties_add_float_slider(g,
		bg_key(k, sizeof(k), PRE, "init_speed"),
		obs_module_text("InitSpeed"), 0.0, 1500.0, 5.0);
	obs_properties_add_float_slider(g,
		bg_key(k, sizeof(k), PRE, "init_dir"),
		obs_module_text("InitDir"), 0.0, 360.0, 1.0);
	obs_properties_add_float_slider(g,
		bg_key(k, sizeof(k), PRE, "init_spread"),
		obs_module_text("InitSpread"), 0.0, 1.0, 0.01);

	bg_common_props(g, PRE, &k_spec);

	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "stretch"),
		obs_module_text("Stretch"), 0.0, 1.0, 0.01);

	obs_property_t *area = obs_properties_add_list(g,
		bg_key(k, sizeof(k), PRE, "spawn_area"),
		obs_module_text("SpawnArea"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(area, obs_module_text("SpawnTop"), SPAWN_TOP);
	obs_property_list_add_int(area, obs_module_text("SpawnFull"),
				  SPAWN_FULL);
	obs_property_list_add_int(area, obs_module_text("SpawnLeft"),
				  SPAWN_LEFT);
	obs_property_list_add_int(area, obs_module_text("SpawnRight"),
				  SPAWN_RIGHT);
	obs_property_list_add_int(area, obs_module_text("SpawnBottom"),
				  SPAWN_BOTTOM);
	obs_property_list_add_int(area, obs_module_text("SpawnCenterV"),
				  SPAWN_CENTER_V);
	obs_property_list_add_int(area, obs_module_text("SpawnCenterH"),
				  SPAWN_CENTER_H);

	obs_properties_add_bool(g, bg_key(k, sizeof(k), PRE, "mirror_lr"),
		obs_module_text("MirrorLR"));

	bg_post_props(g, PRE);
}

static void gravity_defaults(obs_data_t *settings)
{
	char k[96];
	bg_common_defaults(settings, PRE, &k_spec);
	obs_data_set_default_double(settings, bg_key(k, sizeof(k), PRE, "dir"),
				    270.0); /* down */
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "accel"), 700.0);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "init_speed"), 300.0);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "init_dir"), 270.0);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "init_spread"), 0.05);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "stretch"), 0.5);
	obs_data_set_default_int(settings,
		bg_key(k, sizeof(k), PRE, "spawn_area"), SPAWN_TOP);
	obs_data_set_default_bool(settings,
		bg_key(k, sizeof(k), PRE, "mirror_lr"), false);
	bg_post_defaults(settings, PRE, 0.0, 0.0, 0.0, 0.0);
}

const struct bg_effect bgfx_gravity = {
	.id             = "gravity",
	.name_key       = "EffectGravity",
	.create         = gravity_create,
	.destroy        = gravity_destroy,
	.load_graphics  = gravity_load_graphics,
	.update         = gravity_update,
	.tick           = gravity_tick,
	.render         = gravity_render,
	.reset          = gravity_reset,
	.get_properties = gravity_properties,
	.get_defaults   = gravity_defaults,
};
