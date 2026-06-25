#include "bgfx-glowline.h"
#include "bgfx-props.h"  /* struct bg_post */
#include "bgfx-effect.h" /* struct bg_ctx  */

#include <obs.h>
#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

#define BG_TAU 6.28318530718f

gs_effect_t *bg_glowline_load_effect(void)
{
	char *path = obs_module_file("effects/glowline.effect");
	gs_effect_t *e = NULL;
	if (path) {
		e = gs_effect_create_from_file(path, NULL);
		if (!e)
			obs_log(LOG_ERROR, "failed to load glowline.effect (%s)",
				path);
	}
	bfree(path);
	return e;
}

void bg_glowline_set_params(gs_effect_t *e, const struct bg_post *post,
			    float reach)
{
	if (!e)
		return;
	gs_eparam_t *p;
	if ((p = gs_effect_get_param_by_name(e, "glow")))
		gs_effect_set_float(p, post ? post->glow : 0.0f);
	if ((p = gs_effect_get_param_by_name(e, "bloom")))
		gs_effect_set_float(p, post ? post->bloom : 0.0f);
	if ((p = gs_effect_get_param_by_name(e, "emissive")))
		gs_effect_set_float(p, post ? post->emissive : 0.0f);
	if ((p = gs_effect_get_param_by_name(e, "reach")))
		gs_effect_set_float(p, reach < 1.001f ? 1.001f : reach);
}

void bg_glowline_audio_post(struct bg_post *post, const struct bg_ctx *ctx,
			    float amt)
{
	if (!post || !ctx->audio.enabled || amt <= 0.0f)
		return;
	float beat = (ctx->fft && ctx->fft->valid) ? ctx->fft->beat : 0.0f;
	float d = amt * (ctx->audio.level + 0.5f * beat);
	post->glow += d;
	post->bloom += d * 0.6f;
	post->emissive += d * 0.5f;
}

/* Emit one thickened segment as two triangles. The quad reaches hw*reach to
 * each side (cross coord ±reach) so the shader has halo room, and is extended
 * by `ext` px along its own direction at both ends so consecutive segments
 * overlap and corners stay solid. col0/col1 colour the two ends. */
static void emit_seg(uint32_t col0, uint32_t col1, float x0, float y0, float x1,
		     float y1, float hw, float reach, float ext)
{
	float dx = x1 - x0, dy = y1 - y0;
	float l = sqrtf(dx * dx + dy * dy);
	if (l < 1e-6f)
		return;
	float ux = dx / l, uy = dy / l; /* unit along  */
	float nx = -uy, ny = ux;        /* unit normal */
	if (ext > 0.0f) {
		x0 -= ux * ext;
		y0 -= uy * ext;
		x1 += ux * ext;
		y1 += uy * ext;
	}
	float ow = hw * reach; /* outer half-width */
	float ax = x0 + nx * ow, ay = y0 + ny * ow; /* +cross, end0 */
	float bx = x1 + nx * ow, by = y1 + ny * ow; /* +cross, end1 */
	float cx = x1 - nx * ow, cy = y1 - ny * ow; /* -cross, end1 */
	float dx2 = x0 - nx * ow, dy2 = y0 - ny * ow; /* -cross, end0 */

	/* tri A,B,C then A,C,D; uv.x = ±reach at the long edges, uv.y unused. */
	gs_texcoord(reach, 0.0f, 0); gs_color(col0); gs_vertex2f(ax, ay);
	gs_texcoord(reach, 1.0f, 0); gs_color(col1); gs_vertex2f(bx, by);
	gs_texcoord(-reach, 1.0f, 0); gs_color(col1); gs_vertex2f(cx, cy);

	gs_texcoord(reach, 0.0f, 0); gs_color(col0); gs_vertex2f(ax, ay);
	gs_texcoord(-reach, 1.0f, 0); gs_color(col1); gs_vertex2f(cx, cy);
	gs_texcoord(-reach, 0.0f, 0); gs_color(col0); gs_vertex2f(dx2, dy2);
}

void bg_glow_seg(uint32_t col, float x0, float y0, float x1, float y1, float hw,
		 float reach)
{
	emit_seg(col, col, x0, y0, x1, y1, hw, reach, 0.0f);
}

void bg_glow_streak(uint32_t col0, uint32_t col1, float x0, float y0, float x1,
		    float y1, float hw, float reach)
{
	emit_seg(col0, col1, x0, y0, x1, y1, hw, reach, 0.0f);
}

void bg_glow_arc(uint32_t col, float cx, float cy, float r, float a0, float a1,
		 int seg, float hw, float reach)
{
	if (seg < 1)
		seg = 1;
	float ext = hw; /* overlap joints so the ring stays unbroken */
	float px = cx + r * cosf(a0), py = cy + r * sinf(a0);
	for (int k = 1; k <= seg; ++k) {
		float t = (float)k / (float)seg;
		float a = a0 + (a1 - a0) * t;
		float x = cx + r * cosf(a), y = cy + r * sinf(a);
		emit_seg(col, col, px, py, x, y, hw, reach, ext);
		px = x;
		py = y;
	}
}
