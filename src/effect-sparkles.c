#include "effect-sparkles.h"
#include "bgfx-effect.h"
#include "bgfx-particles.h"
#include "bgfx-props.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/image-file.h>
#include <util/bmem.h>

#include <math.h>
#include <string.h>

#define PRE "sparkles"
#define CAPACITY 2048
#define BG_TAU 6.28318530718f

/* Shape selector (UI order). */
enum {
	SP_ROUND = 0,  /* soft round glow            */
	SP_CIRCLE = 1, /* hard disc                  */
	SP_CROSS = 2,  /* cross-filter sparkle       */
	SP_STAR = 3,   /* 5-point star               */
	SP_IMAGE = 4,  /* user-supplied image        */
};

static const struct bg_common_spec k_spec = {
	/* Particle size now reaches 10× the old 80 px ceiling, with a wider
	 * size-variation range so a single field can mix tiny and huge glints. */
	.size_min = 2.0, .size_max = 800.0, .size_step = 1.0, .size_def = 12.0,
	.life_min = 0.5, .life_max = 10.0, .life_def = 3.0,
	.rate_max = 200.0, .rate_def = 30.0,
	.max_cap = CAPACITY, .max_def = 400,
	.color_def = 0xFFFFFFFF, /* white */
	.alpha_def = 1.0,
	.size_var_max = 300,
};

struct sparkles_state {
	gs_effect_t *sprite;
	struct bg_particle_system *sys;

	struct bg_common common;
	struct bg_post post;
	int   shape;         /* enum above                */
	float twinkle_depth; /* 0..1                      */
	float twinkle_speed; /* cycles per second         */

	/* Image shape: loaded lazily under the graphics lock. */
	char            *img_path;   /* desired (from update)        */
	char            *img_loaded; /* path currently in `img`       */
	gs_image_file_t  img;
	bool             has_img;
};

static void *sparkles_create(void)
{
	struct sparkles_state *s = bzalloc(sizeof(*s));
	s->sys = bg_particles_create(CAPACITY);
	/* Fade in → linger → fade out. */
	s->sys->fade_in = 0.25f;
	s->sys->fade_out = 0.25f;
	return s;
}

static void sparkles_destroy(void *data)
{
	struct sparkles_state *s = data;
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

static void sparkles_load_graphics(void *data)
{
	struct sparkles_state *s = data;
	s->sprite = bg_particles_load_effect();
}

static void sparkles_update(void *data, obs_data_t *settings)
{
	struct sparkles_state *s = data;
	char k[96];
	bg_common_update(&s->common, settings, PRE);
	bg_post_update(&s->post, settings, PRE);
	s->shape = (int)obs_data_get_int(settings,
		bg_key(k, sizeof(k), PRE, "shape"));
	s->twinkle_depth = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "twinkle"));
	s->twinkle_speed = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "twinkle_speed"));
	s->sys->flicker_speed = s->twinkle_speed;

	/* Stash the image path; the texture is (re)loaded in render under the
	 * graphics lock when it changes. */
	const char *path = obs_data_get_string(settings,
		bg_key(k, sizeof(k), PRE, "image"));
	bfree(s->img_path);
	s->img_path = (path && path[0]) ? bstrdup(path) : NULL;
}

static void sparkles_reset(void *data, uint32_t seed)
{
	struct sparkles_state *s = data;
	bg_particles_reset(s->sys, seed);
}

static void sparkles_spawn(struct sparkles_state *s, const struct bg_ctx *ctx)
{
	bg_particle_t *p = bg_particles_spawn(s->sys);
	if (!p)
		return;
	struct bg_particle_system *sys = s->sys;
	const struct bg_common *c = &s->common;

	float rgba[4];
	bg_unpack_color(c->color, rgba);

	p->size = bg_vary(sys, c->size, c->size_var) * 0.5f;
	p->x = bg_frand(sys) * (float)ctx->width;
	p->y = bg_frand(sys) * (float)ctx->height;
	p->vx = 0.0f;
	p->vy = 0.0f;
	p->max_life = p->life = bg_vary(sys, c->lifetime, c->life_var);
	p->rot = bg_frand(sys) * BG_TAU;
	p->vrot = bg_frand_range(sys, -0.4f, 0.4f);
	p->seed = bg_frand(sys);
	p->r = rgba[0];
	p->g = rgba[1];
	p->b = rgba[2];
	p->a = c->alpha * bg_frand_range(sys, 0.6f, 1.0f);
	p->flick = s->twinkle_depth;
}

static void sparkles_tick(void *data, const struct bg_ctx *ctx, float dt)
{
	struct sparkles_state *s = data;
	struct bg_particle_system *sys = s->sys;
	sys->clock += dt;

	size_t cap = (size_t)(s->common.max_count < (int)sys->capacity
				      ? s->common.max_count
				      : (int)sys->capacity);
	sys->emit_accum += s->common.rate * dt;
	while (sys->emit_accum >= 1.0f && sys->live < cap) {
		sys->emit_accum -= 1.0f;
		sparkles_spawn(s, ctx);
	}
	if (sys->emit_accum > 4.0f)
		sys->emit_accum = 4.0f;

	for (size_t i = 0; i < sys->live; ++i) {
		bg_particle_t *p = &sys->items[i];
		p->rot += p->vrot * dt;
		p->life -= dt;
	}
	bg_particles_compact(sys);
}

static int sprite_shape(int ui_shape)
{
	switch (ui_shape) {
	case SP_CIRCLE:
		return BG_SHAPE_CIRCLE;
	case SP_CROSS:
		return BG_SHAPE_CROSS;
	case SP_STAR:
		return BG_SHAPE_STAR;
	case SP_IMAGE:
		return BG_SHAPE_IMAGE;
	default:
		return BG_SHAPE_SOFT;
	}
}

/* (Re)load the image when the path changes. Runs under the graphics lock. */
static void sparkles_sync_image(struct sparkles_state *s)
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

static void sparkles_render(void *data, const struct bg_ctx *ctx)
{
	struct sparkles_state *s = data;
	if (!s->sprite)
		return;

	if (s->shape == SP_IMAGE)
		sparkles_sync_image(s);
	else
		s->sys->image = NULL;

	gs_blend_state_push();
	/* Image sprites composite (premultiplied); procedural glints are
	 * additive light. */
	if (s->shape == SP_IMAGE)
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	else
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ONE);
	bg_particles_render(s->sys, s->sprite, sprite_shape(s->shape),
			    &s->post, &ctx->audio);
	gs_blend_state_pop();
}

/* The image picker only matters for the image shape. */
static bool on_shape_changed(void *priv, obs_properties_t *props,
			     obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(priv);
	UNUSED_PARAMETER(prop);
	char k[96];
	int shape = (int)obs_data_get_int(settings,
		bg_key(k, sizeof(k), PRE, "shape"));
	obs_property_t *img = obs_properties_get(props,
		bg_key(k, sizeof(k), PRE, "image"));
	if (img)
		obs_property_set_visible(img, shape == SP_IMAGE);
	return true;
}

static void sparkles_properties(obs_properties_t *g, obs_data_t *settings)
{
	char k[96];
	bg_common_props(g, PRE, &k_spec);

	obs_property_t *shape = obs_properties_add_list(g,
		bg_key(k, sizeof(k), PRE, "shape"),
		obs_module_text("SparkleShape"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(shape, obs_module_text("ShapeRound"), SP_ROUND);
	obs_property_list_add_int(shape, obs_module_text("ShapeCircle"),
				  SP_CIRCLE);
	obs_property_list_add_int(shape, obs_module_text("ShapeCross"), SP_CROSS);
	obs_property_list_add_int(shape, obs_module_text("ShapeStar"), SP_STAR);
	obs_property_list_add_int(shape, obs_module_text("ShapeImage"), SP_IMAGE);
	obs_property_set_modified_callback2(shape, on_shape_changed, NULL);

	obs_property_t *img = obs_properties_add_path(g,
		bg_key(k, sizeof(k), PRE, "image"),
		obs_module_text("SparkleImage"), OBS_PATH_FILE,
		"Images (*.png *.jpg *.jpeg *.bmp *.gif *.tga *.webp);;All (*.*)",
		NULL);
	int cur = settings ? (int)obs_data_get_int(settings,
			bg_key(k, sizeof(k), PRE, "shape")) : SP_STAR;
	obs_property_set_visible(img, cur == SP_IMAGE);

	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "twinkle"),
		obs_module_text("Twinkle"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g,
		bg_key(k, sizeof(k), PRE, "twinkle_speed"),
		obs_module_text("TwinkleSpeed"), 0.5, 20.0, 0.1);

	bg_post_props(g, PRE);
}

static void sparkles_defaults(obs_data_t *settings)
{
	char k[96];
	bg_common_defaults(settings, PRE, &k_spec);
	/* Wider size spread by default to show off the larger range. */
	obs_data_set_default_int(settings, bg_key(k, sizeof(k), PRE, "size_var"),
				 80);
	obs_data_set_default_int(settings, bg_key(k, sizeof(k), PRE, "shape"),
				 SP_STAR);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "twinkle"), 0.7);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "twinkle_speed"), 6.0);
	bg_post_defaults(settings, PRE, 0.4, 0.3, 0.4, 0.15);
}

const struct bg_effect bgfx_sparkles = {
	.id             = "sparkles",
	.name_key       = "EffectSparkles",
	.create         = sparkles_create,
	.destroy        = sparkles_destroy,
	.load_graphics  = sparkles_load_graphics,
	.update         = sparkles_update,
	.tick           = sparkles_tick,
	.render         = sparkles_render,
	.reset          = sparkles_reset,
	.get_properties = sparkles_properties,
	.get_defaults   = sparkles_defaults,
};
