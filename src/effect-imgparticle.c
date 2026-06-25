/* Image particles: spawn billboards of a user-supplied image that drift in a
 * chosen flow direction. Exposes rotation speed, size, opacity, glow/bloom/
 * bleed(にじみ)/lens-flare, launch speed, fade in/out, gravity and a velocity
 * decay (positive = slow down, negative = speed up). Built on the shared
 * particle system + particle.effect's image shape (BG_SHAPE_IMAGE). */

#include "effect-imgparticle.h"
#include "bgfx-effect.h"
#include "bgfx-particles.h"
#include "bgfx-props.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/image-file.h>
#include <util/bmem.h>

#include <math.h>
#include <string.h>

#define PRE "imgparticle"
#define CAPACITY 2048
#define BG_TAU 6.28318530718f

static const struct bg_common_spec k_spec = {
	.size_min = 4.0, .size_max = 1024.0, .size_step = 1.0, .size_def = 96.0,
	.life_min = 0.5, .life_max = 20.0, .life_def = 5.0,
	.rate_max = 200.0, .rate_def = 16.0,
	.max_cap = CAPACITY, .max_def = 150,
	.color_def = 0xFFFFFFFF, /* white = image's own colours */
	.alpha_def = 1.0,
	.size_var_max = 200,
};

struct img_state {
	gs_effect_t *sprite;
	struct bg_particle_system *sys;

	struct bg_common common;
	struct bg_post   post;

	int   flow;     /* bg_flow direction                       */
	float fan;      /* fan spread (fan flows)                  */
	float speed;    /* base launch speed, px/s (0 = drift)     */
	float spread;   /* 0..1 random velocity scatter            */
	float vrot;     /* rotation speed, rad/s                   */
	float gravity;  /* px/s^2, + pulls downward                */
	float drag;     /* per-second velocity decay; + slows, - speeds */
	float blur;     /* にじみ 0..1 (drives sprite softness)     */

	/* Image: loaded lazily under the graphics lock. */
	char            *img_path;
	char            *img_loaded;
	gs_image_file_t  img;
	bool             has_img;
};

static void *img_create(void)
{
	struct img_state *s = bzalloc(sizeof(*s));
	s->sys = bg_particles_create(CAPACITY);
	s->sys->fade_in = 0.15f;
	s->sys->fade_out = 0.2f;
	return s;
}

static void img_destroy(void *data)
{
	struct img_state *s = data;
	if (!s)
		return;
	/* Graphics lock held by the host. */
	if (s->sprite)
		gs_effect_destroy(s->sprite);
	if (s->has_img)
		gs_image_file_free(&s->img);
	bfree(s->img_path);
	bfree(s->img_loaded);
	bg_particles_destroy(s->sys);
	bfree(s);
}

static void img_load_graphics(void *data)
{
	struct img_state *s = data;
	s->sprite = bg_particles_load_effect();
}

static double getd(obs_data_t *s, const char *n)
{
	char k[96];
	return obs_data_get_double(s, bg_key(k, sizeof(k), PRE, n));
}

static void img_update(void *data, obs_data_t *settings)
{
	struct img_state *s = data;
	char k[96];
	bg_common_update(&s->common, settings, PRE);
	bg_post_update(&s->post, settings, PRE);

	s->flow = bg_flow_update(settings, PRE);
	s->fan = bg_flow_fan_update(settings, PRE);
	s->speed = (float)getd(settings, "speed");
	s->spread = (float)getd(settings, "spread");
	s->vrot = (float)getd(settings, "rotate") * (BG_TAU / 360.0f);
	s->gravity = (float)getd(settings, "gravity");
	s->drag = (float)getd(settings, "drag");
	s->blur = (float)getd(settings, "blur");

	s->sys->softness = s->blur;
	s->sys->fade_in = (float)getd(settings, "fade_in");
	s->sys->fade_out = (float)getd(settings, "fade_out");

	const char *path = obs_data_get_string(settings,
		bg_key(k, sizeof(k), PRE, "image"));
	bfree(s->img_path);
	s->img_path = (path && path[0]) ? bstrdup(path) : NULL;
}

static void img_reset(void *data, uint32_t seed)
{
	struct img_state *s = data;
	bg_particles_reset(s->sys, seed);
}

static void img_spawn(struct img_state *s, const struct bg_ctx *ctx)
{
	bg_particle_t *p = bg_particles_spawn(s->sys);
	if (!p)
		return;
	struct bg_particle_system *sys = s->sys;
	const struct bg_common *c = &s->common;

	float W = (float)ctx->width, H = (float)ctx->height;
	float m = c->size + 4.0f; /* spawn just off-canvas */
	float sp = s->speed;
	float x, y, vx = 0.0f, vy = 0.0f, dirsign = 1.0f;

	switch (s->flow) {
	case BG_FLOW_DOWN:
		x = bg_frand(sys) * W; y = -m; vy = sp; break;
	case BG_FLOW_LEFT:
		x = W + m; y = bg_frand(sys) * H; vx = -sp; break;
	case BG_FLOW_RIGHT:
		x = -m; y = bg_frand(sys) * H; vx = sp; break;
	case BG_FLOW_LR:
		dirsign = (bg_frand(sys) < 0.5f) ? -1.0f : 1.0f;
		x = W * 0.5f; y = bg_frand(sys) * H; vx = dirsign * sp; break;
	case BG_FLOW_UP_FAN: {
		x = W * 0.5f + bg_frand_range(sys, -1.0f, 1.0f) * W * 0.1f;
		y = H + m;
		float ang = bg_frand_range(sys, -1.0f, 1.0f) * s->fan;
		vx = sinf(ang) * sp; vy = -cosf(ang) * sp; break;
	}
	case BG_FLOW_UP_CURVE:
		dirsign = (bg_frand(sys) < 0.5f) ? -1.0f : 1.0f;
		x = W * 0.5f; y = H + m;
		vx = dirsign * sp * (0.5f + s->fan); vy = -sp * 0.4f; break;
	case BG_FLOW_UP:
	default:
		x = bg_frand(sys) * W; y = H + m; vy = -sp; break;
	}

	if (s->spread > 0.0f && sp > 0.0f) {
		vx += bg_frand_range(sys, -1.0f, 1.0f) * sp * s->spread;
		vy += bg_frand_range(sys, -1.0f, 1.0f) * sp * s->spread;
	}

	float rgba[4];
	bg_unpack_color(c->color, rgba);

	p->x = x;
	p->y = y;
	p->vx = vx;
	p->vy = vy;
	p->size = bg_vary(sys, c->size, c->size_var) * 0.5f;
	p->len = 0.0f; /* square quad → rotation follows rot, not velocity */
	p->grow = 1.0f;
	p->max_life = p->life = bg_vary(sys, c->lifetime, c->life_var);
	p->rot = bg_frand(sys) * BG_TAU;
	/* Per-particle sign-preserving rotation-speed variation (±20%). */
	p->vrot = s->vrot * (1.0f + 0.2f * bg_frand_range(sys, -1.0f, 1.0f));
	p->seed = bg_frand(sys);
	p->r = rgba[0];
	p->g = rgba[1];
	p->b = rgba[2];
	p->a = c->alpha;
	p->flick = 0.0f;
	p->dirsign = dirsign;
}

static void img_tick(void *data, const struct bg_ctx *ctx, float dt)
{
	struct img_state *s = data;
	struct bg_particle_system *sys = s->sys;
	sys->clock += dt;

	size_t cap = (size_t)(s->common.max_count < (int)sys->capacity
				      ? s->common.max_count
				      : (int)sys->capacity);
	sys->emit_accum += s->common.rate * dt;
	while (sys->emit_accum >= 1.0f && sys->live < cap) {
		sys->emit_accum -= 1.0f;
		img_spawn(s, ctx);
	}
	if (sys->emit_accum > 4.0f)
		sys->emit_accum = 4.0f;

	/* Velocity decay: + slows particles, - accelerates them. */
	float dragf = 1.0f - s->drag * dt;
	if (dragf < 0.0f)
		dragf = 0.0f;

	for (size_t i = 0; i < sys->live; ++i) {
		bg_particle_t *p = &sys->items[i];
		p->vy += s->gravity * dt;
		if (s->drag != 0.0f) {
			p->vx *= dragf;
			p->vy *= dragf;
		}
		p->x += p->vx * dt;
		p->y += p->vy * dt;
		p->rot += p->vrot * dt;
		p->life -= dt;
	}
	bg_particles_compact(sys);
}

/* (Re)load the image when the path changes. Runs under the graphics lock. */
static void img_sync_image(struct img_state *s)
{
	bool same = (!s->img_path && !s->img_loaded) ||
		    (s->img_path && s->img_loaded &&
		     strcmp(s->img_path, s->img_loaded) == 0);
	if (!same) {
		if (s->has_img) {
			gs_image_file_free(&s->img);
			s->has_img = false;
		}
		if (s->img_path) {
			gs_image_file_init(&s->img, s->img_path);
			gs_image_file_init_texture(&s->img);
			s->has_img = true;
		}
		bfree(s->img_loaded);
		s->img_loaded = s->img_path ? bstrdup(s->img_path) : NULL;
	}
	s->sys->image = s->has_img ? s->img.texture : NULL;
}

static void img_render(void *data, const struct bg_ctx *ctx)
{
	struct img_state *s = data;
	if (!s->sprite)
		return;

	img_sync_image(s);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	bg_particles_render(s->sys, s->sprite, BG_SHAPE_IMAGE, &s->post,
			    &ctx->audio);
	gs_blend_state_pop();
}

static void img_properties(obs_properties_t *g, obs_data_t *settings)
{
	char k[96];

	obs_properties_add_path(g, bg_key(k, sizeof(k), PRE, "image"),
		obs_module_text("ImgParticleImage"), OBS_PATH_FILE,
		"Images (*.png *.jpg *.jpeg *.bmp *.gif *.tga *.webp);;All (*.*)",
		NULL);

	bg_common_props(g, PRE, &k_spec);

	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "rotate"),
		obs_module_text("ImgParticleRotate"), -720.0, 720.0, 1.0);

	bg_flow_props(g, PRE, settings);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "speed"),
		obs_module_text("ImgParticleSpeed"), 0.0, 1200.0, 1.0);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "spread"),
		obs_module_text("ImgParticleSpread"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "gravity"),
		obs_module_text("ImgParticleGravity"), -2000.0, 2000.0, 5.0);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "drag"),
		obs_module_text("ImgParticleDrag"), -2.0, 4.0, 0.01);

	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "fade_in"),
		obs_module_text("ImgParticleFadeIn"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "fade_out"),
		obs_module_text("ImgParticleFadeOut"), 0.0, 1.0, 0.01);

	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "blur"),
		obs_module_text("ImgParticleBlur"), 0.0, 1.0, 0.01);

	bg_post_props(g, PRE);
}

static void img_defaults(obs_data_t *settings)
{
	char k[96];
	bg_common_defaults(settings, PRE, &k_spec);
	bg_flow_defaults(settings, PRE);

	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "rotate"), 30.0);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "speed"), 140.0);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "spread"), 0.2);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "gravity"), 0.0);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "drag"), 0.1);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "fade_in"), 0.15);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "fade_out"), 0.2);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "blur"), 0.0);

	bg_post_defaults(settings, PRE, 0.0, 0.0, 0.0, 0.0);
}

const struct bg_effect bgfx_imgparticle = {
	.id             = "imgparticle",
	.name_key       = "EffectImgParticle",
	.create         = img_create,
	.destroy        = img_destroy,
	.load_graphics  = img_load_graphics,
	.update         = img_update,
	.tick           = img_tick,
	.render         = img_render,
	.reset          = img_reset,
	.get_properties = img_properties,
	.get_defaults   = img_defaults,
};
