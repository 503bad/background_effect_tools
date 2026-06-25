/* Radial warp lines: a tunnel of glowing streaks rushing from the centre (far)
 * outward to the edges (near). Each streak accelerates as it nears the edge and
 * grows longer, thicker and brighter, giving a 3D warp-speed feel. Streak
 * parameters (angle, phase, hue, flicker) are derived deterministically from
 * the streak index + seed, so no per-streak storage is needed. Drawn through
 * the shared glowing-line shader; audio can drive the flow speed and glow. */

#include "effect-radial.h"
#include "bgfx-glowline.h"
#include "bgfx-props.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>

#include <math.h>

#define PRE "radial"
#define BG_TAU 6.28318530718f

struct radial_state {
	gs_effect_t *line;

	uint32_t color, accent;
	float    speed;        /* flow speed                       */
	float    thickness;    /* base stroke width, px            */
	float    length;       /* base streak length, px           */
	float    spread;       /* 0..1 colour scatter across lines */
	float    flick_freq;   /* flicker frequency, Hz (0 = off)  */
	int      count;        /* number of streaks                */
	float    audio_amt;    /* 0..2 audio drive                 */
	float    offset;       /* 0..1 inner start radius fraction */
	float    fade_in;      /* 0..1 fade-in strength            */
	float    fade_out;     /* 0..1 fade-out strength           */
	struct bg_post post;   /* glow / bloom / emissive          */

	/* runtime */
	float    c0[4], c1[4];
	uint32_t seed;
	float    flow;  /* integrated outward progress */
	float    clock; /* wall clock for flicker      */
};

static float h1(float x)
{
	float s = sinf(x * 127.1f) * 43758.5453f;
	return s - floorf(s);
}

static float clamp01(float v)
{
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static void *radial_create(void)
{
	return bzalloc(sizeof(struct radial_state));
}

static void radial_destroy(void *data)
{
	struct radial_state *s = data;
	if (!s)
		return;
	if (s->line)
		gs_effect_destroy(s->line);
	bfree(s);
}

static void radial_load_graphics(void *data)
{
	struct radial_state *s = data;
	s->line = bg_glowline_load_effect();
}

static double getd(obs_data_t *s, const char *n)
{
	char k[96];
	return obs_data_get_double(s, bg_key(k, sizeof(k), PRE, n));
}

static void radial_update(void *data, obs_data_t *settings)
{
	struct radial_state *s = data;
	char k[96];
	s->color = (uint32_t)obs_data_get_int(settings,
					      bg_key(k, sizeof(k), PRE, "color"));
	s->accent = (uint32_t)obs_data_get_int(
		settings, bg_key(k, sizeof(k), PRE, "accent"));
	s->speed = (float)getd(settings, "speed");
	s->thickness = (float)getd(settings, "thickness");
	s->length = (float)getd(settings, "length");
	s->spread = (float)getd(settings, "spread");
	s->flick_freq = (float)getd(settings, "flicker");
	s->count = (int)obs_data_get_int(settings,
					 bg_key(k, sizeof(k), PRE, "count"));
	s->audio_amt = (float)getd(settings, "audio");
	s->offset = (float)getd(settings, "offset");
	s->fade_in = (float)getd(settings, "fade_in");
	s->fade_out = (float)getd(settings, "fade_out");
	bg_post_update(&s->post, settings, PRE);

	bg_unpack_color(s->color, s->c0);
	bg_unpack_color(s->accent, s->c1);
}

static void radial_reset(void *data, uint32_t seed)
{
	struct radial_state *s = data;
	s->seed = seed ? seed : 0x1234567u;
	s->flow = 0.0f;
	s->clock = 0.0f;
}

static void radial_tick(void *data, const struct bg_ctx *ctx, float dt)
{
	struct radial_state *s = data;
	float spd = s->speed / 200.0f;
	if (ctx->audio.enabled && s->audio_amt > 0.0f)
		spd *= 1.0f + s->audio_amt * ctx->audio.level;
	s->flow += spd * dt;
	s->clock += dt;
}

static void radial_render(void *data, const struct bg_ctx *ctx)
{
	struct radial_state *s = data;
	if (!s->line || s->count < 1)
		return;

	float w = (float)ctx->width, h = (float)ctx->height;
	float cx = w * 0.5f, cy = h * 0.5f;
	float maxR = sqrtf(w * w + h * h) * 0.5f * 1.1f;
	float sd = (float)(s->seed & 0xFFFF) * 0.000123f;

	struct bg_post post = s->post;
	bg_glowline_audio_post(&post, ctx, s->audio_amt);
	bg_glowline_set_params(s->line, &post, BG_GLOW_REACH);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ONE); /* additive light */
	while (gs_effect_loop(s->line, "Draw")) {
		gs_render_start(true);
		for (int i = 0; i < s->count; ++i) {
			float fi = (float)i + sd;
			float ang = h1(fi * 1.13f) * BG_TAU;
			float phase = h1(fi * 2.71f + 5.0f);
			float hue = h1(fi * 3.33f + 9.0f);
			float fph = h1(fi * 4.57f + 13.0f);

			/* Depth 0 (centre/far) .. 1 (edge/near), squared so the
			 * streak accelerates as it approaches the edge. */
			float d = s->flow + phase;
			d -= floorf(d);
			float depth = d * d;

			float dir_x = cosf(ang), dir_y = sinf(ang);
			/* Offset pushes the start radius outward so the streaks
			 * begin away from the centre instead of at it. */
			float startR = s->offset * maxR;
			float dist = startR + (maxR - startR) * depth;
			float len = s->length * (0.2f + 1.3f * d);
			float tail = dist - len;
			if (tail < startR)
				tail = startR;
			float hw = s->thickness * 0.5f * (0.3f + 1.2f * d);
			if (hw < 0.4f)
				hw = 0.4f;

			/* Fade in from the start, ease off toward the edge; both
			 * spans are driven by their strength sliders (0 = instant
			 * appearance / no edge fade). */
			float env = 1.0f;
			if (s->fade_in > 0.001f)
				env *= clamp01(d / (0.02f + s->fade_in * 0.8f));
			if (s->fade_out > 0.001f)
				env *= clamp01((1.0f - d) /
					       (0.02f + s->fade_out * 0.8f));

			float fl = 1.0f;
			if (s->flick_freq > 0.0f) {
				float ph = s->clock * s->flick_freq * BG_TAU +
					   fph * BG_TAU;
				fl = 0.4f + 0.6f * (0.5f + 0.5f * sinf(ph));
			}

			/* Colour scatter between the two colours. */
			float t = hue * s->spread;
			float r = s->c0[0] + (s->c1[0] - s->c0[0]) * t;
			float gg = s->c0[1] + (s->c1[1] - s->c0[1]) * t;
			float b = s->c0[2] + (s->c1[2] - s->c0[2]) * t;
			float baseA = s->c0[3] + (s->c1[3] - s->c0[3]) * t;
			float a = baseA * env * fl;
			if (a <= 0.003f)
				continue;

			uint32_t head = bg_pack_rgba(r, gg, b, a);
			uint32_t tcol = bg_pack_rgba(r, gg, b, 0.0f); /* faded tail */

			float hx = cx + dir_x * dist, hy = cy + dir_y * dist;
			float tx = cx + dir_x * tail, ty = cy + dir_y * tail;
			bg_glow_streak(tcol, head, tx, ty, hx, hy, hw,
				       BG_GLOW_REACH);
		}
		gs_render_stop(GS_TRIS);
	}
	gs_blend_state_pop();
}

static void radial_properties(obs_properties_t *g, obs_data_t *settings)
{
	char k[96];
	UNUSED_PARAMETER(settings);
	obs_properties_add_color(g, bg_key(k, sizeof(k), PRE, "color"),
				 obs_module_text("RadialColor"));
	obs_properties_add_color(g, bg_key(k, sizeof(k), PRE, "accent"),
				 obs_module_text("RadialAccent"));
	obs_properties_add_int_slider(g, bg_key(k, sizeof(k), PRE, "count"),
				      obs_module_text("RadialCount"), 10, 500, 1);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "speed"),
					obs_module_text("RadialSpeed"), 0.0,
					600.0, 1.0);
	obs_properties_add_float_slider(g,
					bg_key(k, sizeof(k), PRE, "thickness"),
					obs_module_text("RadialThickness"), 1.0,
					30.0, 0.5);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "length"),
					obs_module_text("RadialLength"), 20.0,
					800.0, 5.0);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "spread"),
					obs_module_text("RadialSpread"), 0.0, 1.0,
					0.01);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "flicker"),
					obs_module_text("RadialFlicker"), 0.0,
					20.0, 0.1);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "offset"),
					obs_module_text("RadialOffset"), 0.0, 0.95,
					0.01);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "fade_in"),
					obs_module_text("RadialFadeIn"), 0.0, 1.0,
					0.01);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "fade_out"),
					obs_module_text("RadialFadeOut"), 0.0, 1.0,
					0.01);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "glow"),
					obs_module_text("Glow"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "bloom"),
					obs_module_text("Bloom"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "emissive"),
					obs_module_text("Emissive"), 0.0, 1.0,
					0.01);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "audio"),
					obs_module_text("RadialAudio"), 0.0, 2.0,
					0.05);
}

static void radial_defaults(obs_data_t *s)
{
	char k[96];
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "color"),
				 (long long)0xFFFFFFFF); /* white */
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "accent"),
				 (long long)0xFFFF9C45); /* blue accent */
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "count"), 140);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "speed"),
				    150.0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "thickness"),
				    4.0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "length"),
				    180.0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "spread"),
				    0.4);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "flicker"),
				    0.0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "offset"), 0.0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "fade_in"),
				    0.25);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "fade_out"),
				    0.18);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "glow"), 0.6);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "bloom"), 0.4);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "emissive"),
				    0.4);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "audio"), 0.5);
}

const struct bg_effect bgfx_radial = {
	.id             = "radial",
	.name_key       = "EffectRadial",
	.create         = radial_create,
	.destroy        = radial_destroy,
	.load_graphics  = radial_load_graphics,
	.update         = radial_update,
	.tick           = radial_tick,
	.render         = radial_render,
	.reset          = radial_reset,
	.get_properties = radial_properties,
	.get_defaults   = radial_defaults,
};
