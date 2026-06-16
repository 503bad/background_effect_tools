#include "effect-clouds.h"
#include "bgfx-effect.h"
#include "bgfx-props.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

#define PRE "clouds"

struct clouds_state {
	gs_effect_t *fx;

	uint32_t color_a;
	uint32_t color_b;
	float opacity;
	float morph;      /* shape-change speed                  */
	float scroll_x;   /* px/s                                 */
	float scroll_y;   /* px/s                                 */
	float scale;      /* noise frequency                      */
	float contrast;   /* 0..1 mix hardness                    */
	float softness;   /* 0..1 mottle edge softness            */
	float glow;
	float bloom;
	uint32_t grad_color;
	float grad_angle; /* degrees                              */
	float grad_strength;
};

static void *clouds_create(void)
{
	return bzalloc(sizeof(struct clouds_state));
}

static void clouds_destroy(void *data)
{
	struct clouds_state *s = data;
	if (!s)
		return;
	/* Graphics lock held by the host. */
	if (s->fx)
		gs_effect_destroy(s->fx);
	bfree(s);
}

static void clouds_load_graphics(void *data)
{
	struct clouds_state *s = data;
	char *path = obs_module_file("effects/clouds.effect");
	if (path) {
		s->fx = gs_effect_create_from_file(path, NULL);
		if (!s->fx)
			obs_log(LOG_ERROR, "failed to load clouds.effect (%s)",
				path);
	}
	bfree(path);
}

static void clouds_update(void *data, obs_data_t *settings)
{
	struct clouds_state *s = data;
	char k[96];
	s->color_a = (uint32_t)obs_data_get_int(settings,
		bg_key(k, sizeof(k), PRE, "color_a"));
	s->color_b = (uint32_t)obs_data_get_int(settings,
		bg_key(k, sizeof(k), PRE, "color_b"));
	s->opacity = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "opacity"));
	s->morph = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "morph"));
	s->scroll_x = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "scroll_x"));
	s->scroll_y = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "scroll_y"));
	s->scale = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "scale"));
	s->contrast = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "contrast"));
	s->softness = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "softness"));
	s->glow = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "glow"));
	s->bloom = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "bloom"));
	s->grad_color = (uint32_t)obs_data_get_int(settings,
		bg_key(k, sizeof(k), PRE, "grad_color"));
	s->grad_angle = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "grad_angle"));
	s->grad_strength = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "grad_strength"));
}

static void set_vec4_color(gs_effect_t *e, const char *name, uint32_t color)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(e, name);
	if (!p)
		return;
	float rgba[4];
	bg_unpack_color(color, rgba);
	struct vec4 v;
	vec4_set(&v, rgba[0], rgba[1], rgba[2], rgba[3]);
	gs_effect_set_vec4(p, &v);
}

static void set_float_param(gs_effect_t *e, const char *name, float v)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(e, name);
	if (p)
		gs_effect_set_float(p, v);
}

static void clouds_render(void *data, const struct bg_ctx *ctx)
{
	struct clouds_state *s = data;
	if (!s->fx || s->opacity <= 0.0f)
		return;

	gs_effect_t *e = s->fx;
	float w = (float)ctx->width, h = (float)ctx->height;

	gs_eparam_t *p = gs_effect_get_param_by_name(e, "canvas");
	if (p) {
		struct vec2 c;
		vec2_set(&c, w, h);
		gs_effect_set_vec2(p, &c);
	}
	if ((p = gs_effect_get_param_by_name(e, "scroll"))) {
		struct vec2 c;
		vec2_set(&c, s->scroll_x, s->scroll_y);
		gs_effect_set_vec2(p, &c);
	}
	set_float_param(e, "time", fmodf(ctx->time, 3600.0f));
	set_vec4_color(e, "color_a", s->color_a);
	set_vec4_color(e, "color_b", s->color_b);
	set_float_param(e, "opacity", s->opacity);
	set_float_param(e, "morph", s->morph);
	set_float_param(e, "noise_scale", s->scale);
	set_float_param(e, "contrast", s->contrast);
	set_float_param(e, "softness", s->softness);
	set_float_param(e, "glow", s->glow);
	set_float_param(e, "bloom", s->bloom);
	set_vec4_color(e, "grad_color", s->grad_color);
	set_float_param(e, "grad_angle", s->grad_angle);
	set_float_param(e, "grad_strength", s->grad_strength);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(e, "Draw"))
		gs_draw_sprite(NULL, 0, ctx->width, ctx->height);
	gs_blend_state_pop();
}

static void clouds_properties(obs_properties_t *g, obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);
	char k[96];
	obs_properties_add_color(g, bg_key(k, sizeof(k), PRE, "color_a"),
		obs_module_text("CloudColorA"));
	obs_properties_add_color(g, bg_key(k, sizeof(k), PRE, "color_b"),
		obs_module_text("CloudColorB"));
	obs_properties_add_float_slider(g,
		bg_key(k, sizeof(k), PRE, "opacity"),
		obs_module_text("Alpha"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "morph"),
		obs_module_text("MorphSpeed"), 0.0, 2.0, 0.01);
	obs_properties_add_float_slider(g,
		bg_key(k, sizeof(k), PRE, "scroll_x"),
		obs_module_text("ScrollX"), -300.0, 300.0, 1.0);
	obs_properties_add_float_slider(g,
		bg_key(k, sizeof(k), PRE, "scroll_y"),
		obs_module_text("ScrollY"), -300.0, 300.0, 1.0);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "scale"),
		obs_module_text("NoiseScale"), 0.5, 8.0, 0.05);
	obs_properties_add_float_slider(g,
		bg_key(k, sizeof(k), PRE, "contrast"),
		obs_module_text("Contrast"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g,
		bg_key(k, sizeof(k), PRE, "softness"),
		obs_module_text("Softness"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "glow"),
		obs_module_text("Glow"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "bloom"),
		obs_module_text("Bloom"), 0.0, 1.0, 0.01);
	obs_properties_add_color(g, bg_key(k, sizeof(k), PRE, "grad_color"),
		obs_module_text("GradColor"));
	obs_properties_add_float_slider(g,
		bg_key(k, sizeof(k), PRE, "grad_angle"),
		obs_module_text("GradAngle"), 0.0, 360.0, 1.0);
	obs_properties_add_float_slider(g,
		bg_key(k, sizeof(k), PRE, "grad_strength"),
		obs_module_text("GradStrength"), 0.0, 1.0, 0.01);
}

static void clouds_defaults(obs_data_t *settings)
{
	char k[96];
	obs_data_set_default_int(settings,
		bg_key(k, sizeof(k), PRE, "color_a"), 0xFFF5F0EB); /* warm white */
	obs_data_set_default_int(settings,
		bg_key(k, sizeof(k), PRE, "color_b"), 0xFFB07040); /* slate blue */
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "opacity"), 1.0);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "morph"), 0.3);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "scroll_x"), 20.0);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "scroll_y"), 0.0);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "scale"), 2.0);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "contrast"), 0.5);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "softness"), 0.5);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "glow"), 0.0);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "bloom"), 0.0);
	obs_data_set_default_int(settings,
		bg_key(k, sizeof(k), PRE, "grad_color"), 0xFFFFFFFF);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "grad_angle"), 90.0);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "grad_strength"), 0.0);
}

const struct bg_effect bgfx_clouds = {
	.id             = "clouds",
	.name_key       = "EffectClouds",
	.create         = clouds_create,
	.destroy        = clouds_destroy,
	.load_graphics  = clouds_load_graphics,
	.update         = clouds_update,
	.render         = clouds_render,
	.get_properties = clouds_properties,
	.get_defaults   = clouds_defaults,
};
