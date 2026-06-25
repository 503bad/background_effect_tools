/* Realistic procedural flame: a single full-canvas pass of flame.effect. The
 * flame rises from a base near the bottom-centre; width and height are given as
 * a percentage of the canvas. The shader does the heavy lifting (rising
 * domain-warped fbm + heat→colour ramp); this just feeds it the parameters and
 * advances the clock. Core / outer colours, 発光 / ブルーム / 光彩 / レンズフレア,
 * ゆらめき (flicker) and rotation are all exposed. */

#include "effect-flame.h"
#include "bgfx-effect.h"
#include "bgfx-props.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

#define PRE "flame"
#define BG_TAU 6.28318530718f

struct flame_state {
	gs_effect_t *fx;

	float    width_pct, height_pct;
	uint32_t color_core, color_outer;
	float    emissive, glow, bloom, flare;
	float    flicker, rotation, rise, detail, opacity;

	float    clock;
};

static void *flame_create(void)
{
	return bzalloc(sizeof(struct flame_state));
}

static void flame_destroy(void *data)
{
	struct flame_state *s = data;
	if (!s)
		return;
	if (s->fx)
		gs_effect_destroy(s->fx);
	bfree(s);
}

static void flame_load_graphics(void *data)
{
	struct flame_state *s = data;
	char *path = obs_module_file("effects/flame.effect");
	if (path) {
		s->fx = gs_effect_create_from_file(path, NULL);
		if (!s->fx)
			obs_log(LOG_ERROR, "failed to load flame.effect (%s)",
				path);
	}
	bfree(path);
}

static double getd(obs_data_t *s, const char *n)
{
	char k[96];
	return obs_data_get_double(s, bg_key(k, sizeof(k), PRE, n));
}

static void flame_update(void *data, obs_data_t *settings)
{
	struct flame_state *s = data;
	char k[96];
	s->width_pct = (float)getd(settings, "width");
	s->height_pct = (float)getd(settings, "height");
	s->color_core = (uint32_t)obs_data_get_int(settings,
		bg_key(k, sizeof(k), PRE, "color_core"));
	s->color_outer = (uint32_t)obs_data_get_int(settings,
		bg_key(k, sizeof(k), PRE, "color_outer"));
	s->emissive = (float)getd(settings, "emissive");
	s->glow = (float)getd(settings, "glow");
	s->bloom = (float)getd(settings, "bloom");
	s->flare = (float)getd(settings, "flare");
	s->flicker = (float)getd(settings, "flicker");
	s->rotation = (float)getd(settings, "rotation");
	s->rise = (float)getd(settings, "rise");
	s->detail = (float)getd(settings, "detail");
	s->opacity = (float)getd(settings, "opacity");
}

static void flame_reset(void *data, uint32_t seed)
{
	struct flame_state *s = data;
	UNUSED_PARAMETER(seed);
	s->clock = 0.0f;
}

static void flame_tick(void *data, const struct bg_ctx *ctx, float dt)
{
	struct flame_state *s = data;
	UNUSED_PARAMETER(ctx);
	s->clock += dt;
}

static void set_f(gs_effect_t *e, const char *n, float v)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(e, n);
	if (p)
		gs_effect_set_float(p, v);
}

static void set_c3(gs_effect_t *e, const char *n, uint32_t c)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(e, n);
	if (!p)
		return;
	float rgba[4];
	bg_unpack_color(c, rgba);
	struct vec3 v;
	vec3_set(&v, rgba[0], rgba[1], rgba[2]);
	gs_effect_set_vec3(p, &v);
}

static void flame_render(void *data, const struct bg_ctx *ctx)
{
	struct flame_state *s = data;
	if (!s->fx)
		return;
	float w = (float)ctx->width, h = (float)ctx->height;

	gs_eparam_t *pc = gs_effect_get_param_by_name(s->fx, "canvas");
	if (pc) {
		struct vec2 cv;
		vec2_set(&cv, w, h);
		gs_effect_set_vec2(pc, &cv);
	}
	gs_eparam_t *pb = gs_effect_get_param_by_name(s->fx, "base");
	if (pb) {
		struct vec2 bv;
		vec2_set(&bv, w * 0.5f, h * 0.98f); /* bottom-centre */
		gs_effect_set_vec2(pb, &bv);
	}
	set_f(s->fx, "time", s->clock);
	set_f(s->fx, "half_width", w * (s->width_pct / 100.0f) * 0.5f);
	set_f(s->fx, "height", h * (s->height_pct / 100.0f));
	set_f(s->fx, "rot", s->rotation * (BG_TAU / 360.0f));
	set_c3(s->fx, "col_core", s->color_core);
	set_c3(s->fx, "col_outer", s->color_outer);
	set_f(s->fx, "emissive", s->emissive);
	set_f(s->fx, "glow", s->glow);
	set_f(s->fx, "bloom", s->bloom);
	set_f(s->fx, "flare", s->flare);
	set_f(s->fx, "flicker", s->flicker);
	set_f(s->fx, "rise", s->rise);
	set_f(s->fx, "detail", s->detail);
	set_f(s->fx, "opacity", s->opacity);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(s->fx, "Draw"))
		gs_draw_sprite(NULL, 0, ctx->width, ctx->height);
	gs_blend_state_pop();
}

static void flame_properties(obs_properties_t *g, obs_data_t *settings)
{
	char k[96];
	UNUSED_PARAMETER(settings);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "width"),
		obs_module_text("FlameWidth"), 5.0, 100.0, 1.0);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "height"),
		obs_module_text("FlameHeight"), 5.0, 100.0, 1.0);
	obs_properties_add_color(g, bg_key(k, sizeof(k), PRE, "color_core"),
		obs_module_text("FlameColorCore"));
	obs_properties_add_color(g, bg_key(k, sizeof(k), PRE, "color_outer"),
		obs_module_text("FlameColorOuter"));
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "flicker"),
		obs_module_text("FlameFlicker"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "rise"),
		obs_module_text("FlameRise"), 0.2, 4.0, 0.05);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "detail"),
		obs_module_text("FlameDetail"), 0.3, 3.0, 0.05);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "rotation"),
		obs_module_text("FlameRotation"), -180.0, 180.0, 1.0);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "emissive"),
		obs_module_text("Emissive"), 0.0, 2.0, 0.05);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "glow"),
		obs_module_text("Glow"), 0.0, 2.0, 0.05);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "bloom"),
		obs_module_text("Bloom"), 0.0, 2.0, 0.05);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "flare"),
		obs_module_text("LensFlare"), 0.0, 2.0, 0.05);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "opacity"),
		obs_module_text("Alpha"), 0.0, 1.0, 0.01);
}

static void flame_defaults(obs_data_t *s)
{
	char k[96];
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "width"), 28.0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "height"), 70.0);
	/* white-hot yellow core, deep orange-red outer */
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "color_core"),
				 (long long)0xFF66E6FF); /* #FFE666 warm */
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "color_outer"),
				 (long long)0xFF1030C8); /* #C83010 red-orange */
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "flicker"), 0.5);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "rise"), 1.4);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "detail"), 1.0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "rotation"), 0.0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "emissive"), 0.6);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "glow"), 0.5);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "bloom"), 0.4);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "flare"), 0.0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "opacity"), 1.0);
}

const struct bg_effect bgfx_flame = {
	.id             = "flame",
	.name_key       = "EffectFlame",
	.create         = flame_create,
	.destroy        = flame_destroy,
	.load_graphics  = flame_load_graphics,
	.update         = flame_update,
	.tick           = flame_tick,
	.render         = flame_render,
	.reset          = flame_reset,
	.get_properties = flame_properties,
	.get_defaults   = flame_defaults,
};
