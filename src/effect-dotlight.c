/* Dot Light: a regular grid of glowing dots that blink in a travelling wave.
 * The wave direction (vertical / horizontal / alternating), how many bands ride
 * the screen (frequency), the travel speed, single/gradient colour and the
 * glow / bloom / bleed(にじみ) / lens-flare / dot-size are all configurable.
 * Renders data/effects/dotlight.effect as a full-canvas sprite. */

#include "effect-dotlight.h"
#include "bgfx-effect.h"
#include "bgfx-props.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

#define PRE "dotlight"

/* Flow direction (UI order). */
enum {
	DL_VERT = 0,  /* wave travels vertically   */
	DL_HORIZ = 1, /* wave travels horizontally */
	DL_ALT = 2,   /* alternate axis each band  */
};

struct dotlight_state {
	gs_effect_t *fx;

	uint32_t color_a, color_b;
	int      color_mode; /* 0 single, 1 gradient */
	int      flow;
	float    spacing;
	float    dotsize;
	float    cycles;  /* bands across the axis (frequency) */
	float    speed;   /* phase advance, cycles/s          */
	float    sharp;
	float    floor_b;
	float    glow, bloom, blur, flare;
	float    opacity;

	/* runtime */
	float phase;
	int   cur_axis;   /* 0 vertical, 1 horizontal */
	int   last_floor;
};

static void *dotlight_create(void)
{
	return bzalloc(sizeof(struct dotlight_state));
}

static void dotlight_destroy(void *data)
{
	struct dotlight_state *s = data;
	if (!s)
		return;
	if (s->fx)
		gs_effect_destroy(s->fx);
	bfree(s);
}

static void dotlight_load_graphics(void *data)
{
	struct dotlight_state *s = data;
	char *path = obs_module_file("effects/dotlight.effect");
	if (path) {
		s->fx = gs_effect_create_from_file(path, NULL);
		if (!s->fx)
			obs_log(LOG_ERROR, "failed to load dotlight.effect (%s)",
				path);
	}
	bfree(path);
}

static double getd(obs_data_t *s, const char *n)
{
	char k[96];
	return obs_data_get_double(s, bg_key(k, sizeof(k), PRE, n));
}

static void dotlight_update(void *data, obs_data_t *settings)
{
	struct dotlight_state *s = data;
	char k[96];
	s->color_a = (uint32_t)obs_data_get_int(settings,
		bg_key(k, sizeof(k), PRE, "color_a"));
	s->color_b = (uint32_t)obs_data_get_int(settings,
		bg_key(k, sizeof(k), PRE, "color_b"));
	s->color_mode = (int)obs_data_get_int(settings,
		bg_key(k, sizeof(k), PRE, "color_mode"));
	s->flow = (int)obs_data_get_int(settings,
		bg_key(k, sizeof(k), PRE, "flow"));
	s->spacing = (float)getd(settings, "spacing");
	s->dotsize = (float)getd(settings, "dotsize");
	s->cycles = (float)getd(settings, "cycles");
	s->speed = (float)getd(settings, "speed");
	s->sharp = (float)getd(settings, "sharp");
	s->floor_b = (float)getd(settings, "floor");
	s->glow = (float)getd(settings, "glow");
	s->bloom = (float)getd(settings, "bloom");
	s->blur = (float)getd(settings, "blur");
	s->flare = (float)getd(settings, "flare");
	s->opacity = (float)getd(settings, "opacity");

	if (s->flow == DL_HORIZ)
		s->cur_axis = 1;
	else if (s->flow == DL_VERT)
		s->cur_axis = 0;
}

static void dotlight_reset(void *data, uint32_t seed)
{
	struct dotlight_state *s = data;
	UNUSED_PARAMETER(seed);
	s->phase = 0.0f;
	s->last_floor = 0;
	s->cur_axis = (s->flow == DL_HORIZ) ? 1 : 0;
}

static void dotlight_tick(void *data, const struct bg_ctx *ctx, float dt)
{
	struct dotlight_state *s = data;
	UNUSED_PARAMETER(ctx);
	s->phase += s->speed * dt;
}

static void set_f(gs_effect_t *e, const char *n, float v)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(e, n);
	if (p)
		gs_effect_set_float(p, v);
}

static void set_c(gs_effect_t *e, const char *n, uint32_t c)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(e, n);
	if (!p)
		return;
	float rgba[4];
	bg_unpack_color(c, rgba);
	struct vec4 v;
	vec4_set(&v, rgba[0], rgba[1], rgba[2], rgba[3]);
	gs_effect_set_vec4(p, &v);
}

/* Shared draw used by both Dot Light and Beat Dot Light. */
void bg_dotlight_draw(gs_effect_t *e, const struct bg_ctx *ctx, uint32_t color_a,
		      uint32_t color_b, int color_mode, int axis, float spacing,
		      float dotsize, float cycles, float phase, float sharp,
		      float flash, float floor_b, float glow, float bloom,
		      float blur, float flare, float opacity)
{
	if (!e || opacity <= 0.0f)
		return;
	float w = (float)ctx->width, h = (float)ctx->height;

	gs_eparam_t *p = gs_effect_get_param_by_name(e, "canvas");
	if (p) {
		struct vec2 c;
		vec2_set(&c, w, h);
		gs_effect_set_vec2(p, &c);
	}
	set_c(e, "color_a", color_a);
	set_c(e, "color_b", color_b);
	set_f(e, "color_mode", (float)color_mode);
	set_f(e, "axis", (float)axis);
	set_f(e, "spacing", spacing);
	set_f(e, "dotsize", dotsize);
	set_f(e, "cycles", cycles);
	set_f(e, "phase", phase);
	set_f(e, "sharp", sharp);
	set_f(e, "flash", flash);
	set_f(e, "floor_b", floor_b);
	set_f(e, "glow", glow);
	set_f(e, "bloom", bloom);
	set_f(e, "blur", blur);
	set_f(e, "flare", flare);
	set_f(e, "opacity", opacity);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(e, "Draw"))
		gs_draw_sprite(NULL, 0, ctx->width, ctx->height);
	gs_blend_state_pop();
}

static void dotlight_render(void *data, const struct bg_ctx *ctx)
{
	struct dotlight_state *s = data;
	bg_dotlight_draw(s->fx, ctx, s->color_a, s->color_b, s->color_mode,
			 s->cur_axis, s->spacing, s->dotsize, s->cycles,
			 s->phase, s->sharp, 0.0f, s->floor_b, s->glow,
			 s->bloom, s->blur, s->flare, s->opacity);
}

/* Shared property block (visual controls) used by both dot-light effects. */
void bg_dotlight_visual_props(obs_properties_t *g, const char *pre,
			      obs_data_t *settings)
{
	char k[96];
	UNUSED_PARAMETER(settings);
	obs_properties_add_color(g, bg_key(k, sizeof(k), pre, "color_a"),
		obs_module_text("DotLightColorA"));
	obs_properties_add_color(g, bg_key(k, sizeof(k), pre, "color_b"),
		obs_module_text("DotLightColorB"));
	obs_property_t *cm = obs_properties_add_list(g,
		bg_key(k, sizeof(k), pre, "color_mode"),
		obs_module_text("DotLightColorMode"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(cm, obs_module_text("DotLightColorSingle"), 0);
	obs_property_list_add_int(cm, obs_module_text("DotLightColorGrad"), 1);

	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), pre, "spacing"),
		obs_module_text("DotLightSpacing"), 20.0, 300.0, 1.0);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), pre, "dotsize"),
		obs_module_text("DotLightSize"), 2.0, 120.0, 1.0);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), pre, "cycles"),
		obs_module_text("DotLightFreq"), 0.5, 16.0, 0.1);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), pre, "sharp"),
		obs_module_text("DotLightSharp"), 0.2, 8.0, 0.1);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), pre, "floor"),
		obs_module_text("DotLightFloor"), 0.0, 1.0, 0.01);

	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), pre, "glow"),
		obs_module_text("Glow"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), pre, "bloom"),
		obs_module_text("Bloom"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), pre, "blur"),
		obs_module_text("DotLightBlur"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), pre, "flare"),
		obs_module_text("LensFlare"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), pre, "opacity"),
		obs_module_text("Alpha"), 0.0, 1.0, 0.01);
}

void bg_dotlight_visual_defaults(obs_data_t *s, const char *pre)
{
	char k[96];
	obs_data_set_default_int(s, bg_key(k, sizeof(k), pre, "color_a"),
				 (long long)0xFF66E0FF); /* warm cyan-white */
	obs_data_set_default_int(s, bg_key(k, sizeof(k), pre, "color_b"),
				 (long long)0xFFFF6BD6); /* magenta */
	obs_data_set_default_int(s, bg_key(k, sizeof(k), pre, "color_mode"), 0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), pre, "spacing"),
				    60.0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), pre, "dotsize"),
				    14.0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), pre, "cycles"), 3.0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), pre, "sharp"), 2.0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), pre, "floor"), 0.05);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), pre, "glow"), 0.5);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), pre, "bloom"), 0.4);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), pre, "blur"), 0.2);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), pre, "flare"), 0.0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), pre, "opacity"), 1.0);
}

static void dotlight_properties(obs_properties_t *g, obs_data_t *settings)
{
	char k[96];
	obs_property_t *flow = obs_properties_add_list(g,
		bg_key(k, sizeof(k), PRE, "flow"),
		obs_module_text("DotLightFlow"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(flow, obs_module_text("DotLightFlowVert"),
				  DL_VERT);
	obs_property_list_add_int(flow, obs_module_text("DotLightFlowHoriz"),
				  DL_HORIZ);

	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "speed"),
		obs_module_text("DotLightSpeed"), 0.0, 6.0, 0.01);

	bg_dotlight_visual_props(g, PRE, settings);
}

static void dotlight_defaults(obs_data_t *settings)
{
	char k[96];
	obs_data_set_default_int(settings, bg_key(k, sizeof(k), PRE, "flow"),
				 DL_VERT);
	obs_data_set_default_double(settings, bg_key(k, sizeof(k), PRE, "speed"),
				    0.5);
	bg_dotlight_visual_defaults(settings, PRE);
}

const struct bg_effect bgfx_dotlight = {
	.id             = "dotlight",
	.name_key       = "EffectDotLight",
	.create         = dotlight_create,
	.destroy        = dotlight_destroy,
	.load_graphics  = dotlight_load_graphics,
	.update         = dotlight_update,
	.tick           = dotlight_tick,
	.render         = dotlight_render,
	.reset          = dotlight_reset,
	.get_properties = dotlight_properties,
	.get_defaults   = dotlight_defaults,
};
