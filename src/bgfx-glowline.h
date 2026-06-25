#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gs_effect;
typedef struct gs_effect gs_effect_t;
struct bg_post;
struct bg_ctx;

/* Pack normalized rgba into OBS 0xAABBGGRR (clamped + rounded), for the vertex
 * colours the line-art effects feed to gs_color. */
static inline uint32_t bg_pack_rgba(float r, float g, float b, float a)
{
	uint32_t R = (uint32_t)(r < 0 ? 0 : (r > 1 ? 255 : r * 255.0f + 0.5f));
	uint32_t G = (uint32_t)(g < 0 ? 0 : (g > 1 ? 255 : g * 255.0f + 0.5f));
	uint32_t B = (uint32_t)(b < 0 ? 0 : (b > 1 ? 255 : b * 255.0f + 0.5f));
	uint32_t A = (uint32_t)(a < 0 ? 0 : (a > 1 ? 255 : a * 255.0f + 0.5f));
	return (A << 24) | (B << 16) | (G << 8) | R;
}

/* Shared glowing-line drawing on top of data/effects/glowline.effect. The
 * line-art effects (magic circle, radial warp lines) build everything from the
 * emit helpers below: each emits thickened-segment geometry whose quads carry a
 * cross-line coordinate so the shader can add an analytical glow/bloom halo.
 *
 * Usage (under the graphics lock, additive blend pushed by the caller):
 *   bg_glowline_set_params(e, &post, reach);
 *   while (gs_effect_loop(e, "Draw")) {
 *       gs_render_start(true);
 *       bg_glow_seg(...); bg_glow_arc(...); ...
 *       gs_render_stop(GS_TRIS);
 *   }
 *
 * `reach` is the quad half-width as a multiple of the core half-width (and the
 * extent over which bloom falls to zero). Pass the SAME reach to set_params and
 * every emit call; BG_GLOW_REACH is the recommended default. Colours are OBS
 * 0xAABBGGRR; alpha drives the line opacity and (for streaks) the tail fade. */

#define BG_GLOW_REACH 3.0f

/* Load data/effects/glowline.effect (call under the graphics lock). */
gs_effect_t *bg_glowline_load_effect(void);

/* Set the glow/bloom/emissive/reach uniforms before the draw loop. */
void bg_glowline_set_params(gs_effect_t *e, const struct bg_post *post,
			    float reach);

/* Add this frame's audio drive (smoothed level + beat) into `post`'s light, for
 * the line-art effects' audio reactivity. No-op when audio is off or amt <= 0. */
void bg_glowline_audio_post(struct bg_post *post, const struct bg_ctx *ctx,
			    float amt);

/* One straight segment, constant colour, core half-width `hw`. No end fade —
 * for connected strokes. */
void bg_glow_seg(uint32_t col, float x0, float y0, float x1, float y1, float hw,
		 float reach);

/* Streak with per-end colour/alpha: col0 at (x0,y0), col1 at (x1,y1). Use a low
 * alpha at the tail end for a comet look. */
void bg_glow_streak(uint32_t col0, uint32_t col1, float x0, float y0, float x1,
		    float y1, float hw, float reach);

/* Arc / circle as a polyline of `seg` segments from angle a0 to a1 (radians).
 * Pass a0=0, a1=2*PI for a full circle. Corners overlap so joints stay solid.
 * Shorten a1 to draw a partial arc (used for the magic-circle self-draw). */
void bg_glow_arc(uint32_t col, float cx, float cy, float r, float a0, float a1,
		 int seg, float hw, float reach);

#ifdef __cplusplus
}
#endif
