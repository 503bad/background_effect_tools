/* Geometric pattern backgrounds (see effect-geometric.h).
 *
 * Four modes, each redrawn every frame from a deterministic hash of the tile /
 * shape index so the look is stable while scroll / colour-cycle / pulse animate
 * it. All geometry goes through the flat vertex-colour viz shader with normal
 * alpha blending, so translucent tiles composite over the (optional) background.
 *
 *   0 TRIANGLES  square cells split on a per-cell diagonal (checker triangles)
 *   1 HEX        pointy-top hexagon grid (filled or outlined), corner fade
 *   2 DIAMONDS   gradient bg + translucent diamonds + diagonal slashes + dots
 *   3 MEMPHIS    scattered squares/triangles/rings/plus/cross/dots/waves
 */

#include "effect-geometric.h"
#include "bgfx-effect.h"
#include "bgfx-props.h"
#include "bgfx-audio.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>

#include <math.h>

#define PRE "geo"
#define BG_TAU 6.28318530718f

enum geo_mode {
	GEO_TRIANGLES = 0,
	GEO_HEX = 1,
	GEO_DIAMONDS = 2,
	GEO_MEMPHIS = 3,
};

struct geometric_state {
	gs_effect_t *viz;

	int      mode;
	float    cell;       /* base tile / shape size (px)              */
	float    line_width; /* hex outline thickness (0 = filled)        */
	float    fade;       /* hex corner fade amount                    */
	int      count;      /* memphis shape count                       */
	int      seed;
	uint32_t color0, color1, color2, bgcolor;
	bool     transparent;
	bool     grad_bg;    /* fill background with color0→color1 gradient */
	float    scroll_x, scroll_y;
	float    cycle;      /* palette cycling speed                     */
	float    pulse_amt, pulse_speed;
	float    audio_amt;  /* brightness/scale from audio level + beat  */
	float    opacity;
	float    time_scale;

	/* runtime */
	float c0[4], c1[4], c2[4], bg[4];
	float clock;
	float ox, oy; /* accumulated scroll offset (px) */
	struct bg_audio_react areact; /* shared size/colour/bounce, per frame */
};

/* ---- helpers ------------------------------------------------------------- */

static float clamp01f(float v)
{
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

static float fr1(float x) { return x - floorf(x); }

static float h11(float x) { return fr1(sinf(x * 127.1f) * 43758.5453f); }

static float h21(float x, float y)
{
	return fr1(sinf(x * 127.1f + y * 311.7f) * 43758.5453f);
}

static uint32_t pack_rgba(float r, float g, float b, float a)
{
	uint32_t R = (uint32_t)(r < 0 ? 0 : (r > 1 ? 255 : r * 255.0f + 0.5f));
	uint32_t G = (uint32_t)(g < 0 ? 0 : (g > 1 ? 255 : g * 255.0f + 0.5f));
	uint32_t B = (uint32_t)(b < 0 ? 0 : (b > 1 ? 255 : b * 255.0f + 0.5f));
	uint32_t A = (uint32_t)(a < 0 ? 0 : (a > 1 ? 255 : a * 255.0f + 0.5f));
	return (A << 24) | (B << 16) | (G << 8) | R; /* OBS 0xAABBGGRR */
}

/* Cycle through the three palette colours by phase t (0..1). */
static void pal(const struct geometric_state *s, float t, float out[3])
{
	t = fr1(t);
	float seg = t * 3.0f;
	int i = (int)seg;
	float f = seg - (float)i;
	const float *a = i == 0 ? s->c0 : (i == 1 ? s->c1 : s->c2);
	const float *b = i == 0 ? s->c1 : (i == 1 ? s->c2 : s->c0);
	for (int k = 0; k < 3; ++k)
		out[k] = lerpf(a[k], b[k], f);
	bg_audio_react_color(&s->areact, out); /* audio colour target */
}

static void vtx(uint32_t col, float x, float y)
{
	gs_color(col);
	gs_vertex2f(x, y);
}

static void tri(uint32_t col, float ax, float ay, float bx, float by, float cx,
		float cy)
{
	vtx(col, ax, ay);
	vtx(col, bx, by);
	vtx(col, cx, cy);
}

static void quad(uint32_t col, float ax, float ay, float bx, float by, float cx,
		 float cy, float dx, float dy)
{
	tri(col, ax, ay, bx, by, cx, cy);
	tri(col, ax, ay, cx, cy, dx, dy);
}

/* Quad with per-corner colours (for gradients). */
static void quad4(uint32_t ca, uint32_t cb, uint32_t cc, uint32_t cd, float ax,
		  float ay, float bx, float by, float cx, float cy, float dx,
		  float dy)
{
	vtx(ca, ax, ay);
	vtx(cb, bx, by);
	vtx(cc, cx, cy);
	vtx(ca, ax, ay);
	vtx(cc, cx, cy);
	vtx(cd, dx, dy);
}

static void fill_ngon(uint32_t col, float cx, float cy, float r, int n,
		      float rot)
{
	for (int k = 0; k < n; ++k) {
		float a0 = rot + BG_TAU * (float)k / (float)n;
		float a1 = rot + BG_TAU * (float)(k + 1) / (float)n;
		tri(col, cx, cy, cx + r * cosf(a0), cy + r * sinf(a0),
		    cx + r * cosf(a1), cy + r * sinf(a1));
	}
}

static void ring_ngon(uint32_t col, float cx, float cy, float r, float width,
		      int n, float rot)
{
	float ri = r - width;
	if (ri < 0.0f)
		ri = 0.0f;
	for (int k = 0; k < n; ++k) {
		float a0 = rot + BG_TAU * (float)k / (float)n;
		float a1 = rot + BG_TAU * (float)(k + 1) / (float)n;
		float o0x = cx + r * cosf(a0), o0y = cy + r * sinf(a0);
		float o1x = cx + r * cosf(a1), o1y = cy + r * sinf(a1);
		float i0x = cx + ri * cosf(a0), i0y = cy + ri * sinf(a0);
		float i1x = cx + ri * cosf(a1), i1y = cy + ri * sinf(a1);
		quad(col, o0x, o0y, o1x, o1y, i1x, i1y, i0x, i0y);
	}
}

static void fan_arc(uint32_t col, float cx, float cy, float r, float a0,
		    float a1, int n)
{
	for (int k = 0; k < n; ++k) {
		float t0 = lerpf(a0, a1, (float)k / (float)n);
		float t1 = lerpf(a0, a1, (float)(k + 1) / (float)n);
		tri(col, cx, cy, cx + r * cosf(t0), cy + r * sinf(t0),
		    cx + r * cosf(t1), cy + r * sinf(t1));
	}
}

static void thick_seg(uint32_t col, float x0, float y0, float x1, float y1,
		      float hw)
{
	float dx = x1 - x0, dy = y1 - y0;
	float l = sqrtf(dx * dx + dy * dy) + 1e-6f;
	float nx = -dy / l * hw, ny = dx / l * hw;
	quad(col, x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny,
	     x0 - nx, y0 - ny);
}

static void diamond(uint32_t col, float cx, float cy, float rx, float ry)
{
	quad(col, cx, cy - ry, cx + rx, cy, cx, cy + ry, cx - rx, cy);
}

static void plus_shape(uint32_t col, float cx, float cy, float len, float hw)
{
	quad(col, cx - len, cy - hw, cx + len, cy - hw, cx + len, cy + hw,
	     cx - len, cy + hw);
	quad(col, cx - hw, cy - len, cx + hw, cy - len, cx + hw, cy + len,
	     cx - hw, cy + len);
}

static void cross_shape(uint32_t col, float cx, float cy, float len, float hw)
{
	thick_seg(col, cx - len, cy - len, cx + len, cy + len, hw);
	thick_seg(col, cx - len, cy + len, cx + len, cy - len, hw);
}

static void dot_grid(uint32_t col, float ox, float oy, float spacing, int nx,
		     int ny, float dotr)
{
	for (int j = 0; j < ny; ++j)
		for (int i = 0; i < nx; ++i)
			fill_ngon(col, ox + i * spacing, oy + j * spacing, dotr,
				  8, 0.0f);
}

static void wavy(uint32_t col, float x0, float y0, float len, float amp,
		 float wl, float hw)
{
	int n = 24;
	float px = x0, py = y0;
	for (int k = 1; k <= n; ++k) {
		float t = (float)k / (float)n;
		float x = x0 + len * t;
		float y = y0 + amp * sinf((len * t) / wl * BG_TAU);
		thick_seg(col, px, py, x, y, hw);
		px = x;
		py = y;
	}
}

/* ---- lifecycle ----------------------------------------------------------- */

static void *geometric_create(void)
{
	struct geometric_state *s = bzalloc(sizeof(*s));
	return s;
}

static void geometric_destroy(void *data)
{
	struct geometric_state *s = data;
	if (!s)
		return;
	if (s->viz)
		gs_effect_destroy(s->viz);
	bfree(s);
}

static gs_effect_t *load_viz(void)
{
	char *path = obs_module_file("effects/viz.effect");
	gs_effect_t *e = NULL;
	if (path)
		e = gs_effect_create_from_file(path, NULL);
	bfree(path);
	return e;
}

static void geometric_load_graphics(void *data)
{
	struct geometric_state *s = data;
	s->viz = load_viz();
}

static double getd(obs_data_t *s, const char *n)
{
	char k[96];
	return obs_data_get_double(s, bg_key(k, sizeof(k), PRE, n));
}
static long long geti(obs_data_t *s, const char *n)
{
	char k[96];
	return obs_data_get_int(s, bg_key(k, sizeof(k), PRE, n));
}
static bool getb(obs_data_t *s, const char *n)
{
	char k[96];
	return obs_data_get_bool(s, bg_key(k, sizeof(k), PRE, n));
}

static void geometric_update(void *data, obs_data_t *settings)
{
	struct geometric_state *s = data;
	s->mode = (int)geti(settings, "mode");
	s->cell = (float)getd(settings, "cell");
	s->line_width = (float)getd(settings, "line_width");
	s->fade = (float)getd(settings, "fade");
	s->count = (int)geti(settings, "count");
	s->seed = (int)geti(settings, "seed");
	s->color0 = (uint32_t)geti(settings, "color0");
	s->color1 = (uint32_t)geti(settings, "color1");
	s->color2 = (uint32_t)geti(settings, "color2");
	s->bgcolor = (uint32_t)geti(settings, "bgcolor");
	s->transparent = getb(settings, "transparent");
	s->grad_bg = getb(settings, "grad_bg");
	s->scroll_x = (float)getd(settings, "scroll_x");
	s->scroll_y = (float)getd(settings, "scroll_y");
	s->cycle = (float)getd(settings, "cycle");
	s->pulse_amt = (float)getd(settings, "pulse_amt");
	s->pulse_speed = (float)getd(settings, "pulse_speed");
	s->audio_amt = (float)getd(settings, "audio_amt");
	s->opacity = (float)getd(settings, "opacity");
	s->time_scale = (float)getd(settings, "time_scale");

	bg_unpack_color(s->color0, s->c0);
	bg_unpack_color(s->color1, s->c1);
	bg_unpack_color(s->color2, s->c2);
	bg_unpack_color(s->bgcolor, s->bg);
}

static void geometric_reset(void *data, uint32_t seed)
{
	struct geometric_state *s = data;
	UNUSED_PARAMETER(seed);
	s->clock = 0.0f;
	s->ox = 0.0f;
	s->oy = 0.0f;
}

static void geometric_tick(void *data, const struct bg_ctx *ctx, float dt)
{
	struct geometric_state *s = data;
	UNUSED_PARAMETER(ctx);
	dt *= s->time_scale;
	if (dt > 0.1f)
		dt = 0.1f;
	s->clock += dt;
	s->ox += s->scroll_x * dt;
	s->oy += s->scroll_y * dt;
}

/* ---- shared per-frame drive --------------------------------------------- */

/* Audio brightness/scale boost for this frame (1 = none). */
static float audio_drive(const struct geometric_state *s,
			 const struct bg_ctx *ctx)
{
	if (!ctx->audio.enabled || s->audio_amt <= 0.0f)
		return 0.0f;
	float beat = (ctx->fft && ctx->fft->valid) ? ctx->fft->beat : 0.0f;
	return s->audio_amt * (ctx->audio.level + 0.5f * beat);
}

/* ---- background ---------------------------------------------------------- */

static void draw_background(struct geometric_state *s, float w, float h)
{
	if (s->transparent)
		return;
	if (s->grad_bg) {
		uint32_t a = pack_rgba(s->c0[0], s->c0[1], s->c0[2], 1.0f);
		uint32_t b = pack_rgba(s->c1[0], s->c1[1], s->c1[2], 1.0f);
		/* diagonal gradient: top-left col0 → bottom-right col1 */
		quad4(a, a, b, b, 0, 0, w, 0, w, h, 0, h);
	} else {
		uint32_t c = pack_rgba(s->bg[0], s->bg[1], s->bg[2], 1.0f);
		quad(c, 0, 0, w, 0, w, h, 0, h);
	}
}

/* ---- mode: triangle tiles ------------------------------------------------ */

static void draw_triangles(struct geometric_state *s, const struct bg_ctx *ctx)
{
	float w = (float)ctx->width, h = (float)ctx->height;
	float cell = s->cell < 8.0f ? 8.0f : s->cell;
	float adrive = audio_drive(s, ctx);
	/* Scroll seamlessly: draw at the fractional offset, but key every tile's
	 * content off the absolute cell index (gx-sx) so the look shifts WITH the
	 * motion and never jumps when the offset wraps. */
	int sx = (int)floorf(s->ox / cell);
	int sy = (int)floorf(s->oy / cell);
	float fx = s->ox - (float)sx * cell;
	float fy = s->oy - (float)sy * cell;
	int cols = (int)(w / cell) + 3;
	int rows = (int)(h / cell) + 3;

	for (int gy = -1; gy < rows; ++gy) {
		for (int gx = -1; gx < cols; ++gx) {
			float x0 = (float)gx * cell + fx;
			float y0 = (float)gy * cell + fy;
			float x1 = x0 + cell, y1 = y0 + cell;
			int kx = gx - sx, ky = gy - sy;
			bool slash = h21((float)kx + (float)s->seed,
					 (float)ky) < 0.5f;
			for (int half = 0; half < 2; ++half) {
				float ct = h21((float)(kx * 2 + half) +
						       (float)s->seed * 3.1f,
					       (float)ky * 1.7f);
				float ph = h11((float)(kx * 7 + ky * 13 +
						       half));
				float rgb[3];
				pal(s, ct + s->clock * s->cycle, rgb);
				float m = 1.0f +
					  s->pulse_amt *
						  sinf(s->clock * s->pulse_speed +
						       ph * BG_TAU) +
					  adrive;
				uint32_t col = pack_rgba(rgb[0] * m, rgb[1] * m,
							 rgb[2] * m, s->opacity);
				if (slash) {
					if (half == 0)
						tri(col, x0, y0, x1, y0, x1, y1);
					else
						tri(col, x0, y0, x1, y1, x0, y1);
				} else {
					if (half == 0)
						tri(col, x0, y0, x1, y0, x0, y1);
					else
						tri(col, x1, y0, x1, y1, x0, y1);
				}
			}
		}
	}
}

/* ---- mode: hexagon grid -------------------------------------------------- */

static void draw_hex(struct geometric_state *s, const struct bg_ctx *ctx)
{
	float w = (float)ctx->width, h = (float)ctx->height;
	float r = s->cell < 8.0f ? 8.0f : s->cell;
	float adrive = audio_drive(s, ctx);
	float hsp = 1.73205081f * r;  /* horizontal spacing (pointy-top) */
	float vsp = 1.5f * r;         /* vertical spacing                */
	/* Seamless scroll (see draw_triangles). Vertical shift is in whole
	 * 2-row periods so the odd-row half-offset parity stays aligned. */
	int sx = (int)floorf(s->ox / hsp);
	int syc = (int)floorf(s->oy / (vsp * 2.0f));
	int sy = syc * 2;
	float fx = s->ox - (float)sx * hsp;
	float fy = s->oy - (float)syc * vsp * 2.0f;
	int cols = (int)(w / hsp) + 3;
	int rows = (int)(h / vsp) + 3;

	for (int gy = -2; gy < rows; ++gy) {
		for (int gx = -2; gx < cols; ++gx) {
			float cx = (float)gx * hsp + ((gy & 1) ? hsp * 0.5f : 0)
				   + fx;
			float cy = (float)gy * vsp + fy;
			int kx = gx - sx, ky = gy - sy;
			float ct = h21((float)kx + (float)s->seed,
				       (float)ky * 2.3f);
			float rgb[3];
			pal(s, ct + s->clock * s->cycle, rgb);
			/* corner fade: dim toward the lower-right (image3 look) */
			float ff = 1.0f - s->fade * clamp01f(cx / w * 0.6f +
							     cy / h * 0.6f);
			float ph = h11((float)(kx * 5 + ky * 11));
			float m = 1.0f +
				  s->pulse_amt * sinf(s->clock * s->pulse_speed +
						      ph * BG_TAU) +
				  adrive;
			uint32_t col = pack_rgba(rgb[0] * m, rgb[1] * m,
						 rgb[2] * m,
						 clamp01f(s->opacity * ff));
			if (s->line_width > 0.5f)
				ring_ngon(col, cx, cy, r, s->line_width, 6,
					  BG_TAU / 12.0f);
			else
				fill_ngon(col, cx, cy, r * 0.98f, 6,
					  BG_TAU / 12.0f);
		}
	}
}

/* ---- mode: diamonds + slashes + dots (image1) ---------------------------- */

static void draw_diamonds(struct geometric_state *s, const struct bg_ctx *ctx)
{
	float w = (float)ctx->width, h = (float)ctx->height;
	float cell = s->cell < 16.0f ? 16.0f : s->cell;
	float adrive = audio_drive(s, ctx);
	float period = cell * 2.0f;
	int sx = (int)floorf(s->ox / period);
	int sy = (int)floorf(s->oy / period);
	float fx = s->ox - (float)sx * period;
	float fy = s->oy - (float)sy * period;
	/* The diamond palette is the single accent colour c2; apply the audio
	 * colour target to it once for every shape this frame. */
	float c2r[3] = {s->c2[0], s->c2[1], s->c2[2]};
	bg_audio_react_color(&s->areact, c2r);
	uint32_t light = pack_rgba(c2r[0], c2r[1], c2r[2],
				   clamp01f(0.10f + 0.10f * adrive) *
					   s->opacity);
	uint32_t outline = pack_rgba(c2r[0], c2r[1], c2r[2],
				     0.5f * s->opacity);

	int cols = (int)(w / period) + 3;
	int rows = (int)(h / period) + 3;
	for (int gy = -1; gy < rows; ++gy) {
		for (int gx = -1; gx < cols; ++gx) {
			float cx = (float)gx * period + fx;
			float cy = (float)gy * period + fy;
			int kx = gx - sx, ky = gy - sy;
			float hsh = h21((float)kx + (float)s->seed,
					(float)ky * 1.3f);
			if (hsh < 0.25f)
				continue; /* leave gaps */
			/* Keep the diameter below the cell spacing so diamonds
			 * don't overlap — overlapping translucent fills make the
			 * density (and thus brightness) flicker as they scroll. */
			float sz = cell * (0.35f + 0.45f * hsh);
			if (hsh > 0.7f)
				/* outlined diamond (frame); thicker line so the
				 * edge doesn't shimmer at sub-pixel offsets */
				ring_ngon(outline, cx, cy, sz,
					  fmaxf(3.0f, sz * 0.14f), 4,
					  BG_TAU / 8.0f);
			else
				diamond(light, cx, cy, sz, sz);
		}
	}

	/* Evenly spaced 45° slashes; the drift offset is periodic in their
	 * spacing, so they too scroll seamlessly (no teleport on wrap). */
	uint32_t slash = pack_rgba(c2r[0], c2r[1], c2r[2],
				   0.55f * s->opacity);
	float sp = period * 1.5f;
	float soff = fmodf(s->ox * 0.5f, sp);
	if (soff < 0.0f)
		soff += sp;
	float len = h * 0.8f;
	int ns = (int)((w + h) / sp) + 2;
	for (int i = 0; i < ns; ++i) {
		float p = (float)i * sp + soff;
		thick_seg(slash, p, -h * 0.1f, p - len, -h * 0.1f + len, 1.5f);
	}

	/* dotted triangle clusters in two corners (image1) */
	uint32_t dot = pack_rgba(c2r[0], c2r[1], c2r[2],
				 0.85f * s->opacity);
	float ds = cell * 0.5f;
	for (int j = 0; j < 6; ++j)
		dot_grid(dot, w * 0.62f + ds, h * 0.72f + (float)j * ds,
			 ds, 6 - j, 1, ds * 0.12f + 1.0f);
	for (int j = 0; j < 5; ++j)
		dot_grid(dot, ds, ds + (float)j * ds, ds, j + 1, 1,
			 ds * 0.12f + 1.0f);
}

/* ---- mode: Memphis scatter (image6/7) ------------------------------------ */

static void draw_memphis(struct geometric_state *s, const struct bg_ctx *ctx)
{
	float w = (float)ctx->width, h = (float)ctx->height;
	float adrive = audio_drive(s, ctx);
	int n = s->count < 1 ? 1 : s->count;
	float base = s->cell < 8.0f ? 8.0f : s->cell;

	for (int i = 0; i < n; ++i) {
		float fi = (float)i + (float)s->seed * 1.7f;
		float hx = h11(fi * 1.13f);
		float hy = h11(fi * 2.71f + 5.0f);
		float drift = base * 0.6f;
		/* Wrap over screen + margin so a shape teleports only while
		 * fully off-screen (margin > max half-size + drift); on-screen
		 * it just glides off one edge and back in the other. */
		float margin = base * 2.5f;
		float spanx = w + 2.0f * margin;
		float spany = h + 2.0f * margin;
		float cx = -margin + fr1(hx + s->ox / spanx) * spanx +
			   drift * sinf(s->clock * 0.3f + fi);
		float cy = -margin + fr1(hy + s->oy / spany) * spany +
			   drift * cosf(s->clock * 0.27f + fi * 1.3f);
		float hs = h11(fi * 3.7f + 1.0f);
		float sz = base * (0.5f + hs);
		float rot = s->clock * 0.2f * (h11(fi * 5.1f) - 0.5f) * 2.0f +
			    h11(fi) * BG_TAU;
		float ct = h11(fi * 4.4f);
		float rgb[3];
		pal(s, ct + s->clock * s->cycle, rgb);
		float m = 1.0f + adrive;
		float sc = 1.0f + 0.2f * adrive;
		sz *= sc;
		uint32_t col = pack_rgba(rgb[0] * m, rgb[1] * m, rgb[2] * m,
					 s->opacity);
		float hw = fmaxf(2.0f, sz * 0.18f);
		int type = (int)(h11(fi * 6.6f) * 9.0f);
		switch (type) {
		case 0: { /* rotated square */
			float c = cosf(rot), sn = sinf(rot);
			float px[4] = {-sz, sz, sz, -sz};
			float py[4] = {-sz, -sz, sz, sz};
			float qx[4], qy[4];
			for (int k = 0; k < 4; ++k) {
				qx[k] = cx + px[k] * c - py[k] * sn;
				qy[k] = cy + px[k] * sn + py[k] * c;
			}
			quad(col, qx[0], qy[0], qx[1], qy[1], qx[2], qy[2],
			     qx[3], qy[3]);
			break;
		}
		case 1: /* triangle */
			fill_ngon(col, cx, cy, sz, 3, rot);
			break;
		case 2: /* ring */
			ring_ngon(col, cx, cy, sz, hw, 24, 0.0f);
			break;
		case 3: /* disc */
			fill_ngon(col, cx, cy, sz, 24, 0.0f);
			break;
		case 4: /* plus */
			plus_shape(col, cx, cy, sz, hw);
			break;
		case 5: /* cross */
			cross_shape(col, cx, cy, sz, hw);
			break;
		case 6: /* small dot grid */
			dot_grid(col, cx - sz, cy - sz, sz * 0.7f, 4, 4,
				 hw * 0.6f);
			break;
		case 7: /* wavy line */
			wavy(col, cx - sz * 1.5f, cy, sz * 3.0f, sz * 0.5f,
			     sz * 1.2f, hw * 0.7f);
			break;
		default: /* half circle */
			fan_arc(col, cx, cy, sz, rot, rot + 3.14159265f, 16);
			break;
		}
	}
}

/* ---- render -------------------------------------------------------------- */

static void geometric_render(void *data, const struct bg_ctx *ctx)
{
	struct geometric_state *s = data;
	if (!s->viz)
		return;
	float w = (float)ctx->width, h = (float)ctx->height;

	/* Shared audio targets for this frame. Colour is consumed while drawing
	 * (pal / draw_diamonds); size + bounce become a centre-anchored scale +
	 * vertical hop of the pattern. */
	bg_audio_react_init(&s->areact, &ctx->audio);
	float cx = w * 0.5f, cy = h * 0.5f;
	float by = bg_audio_react_bounce(&s->areact, s->clock, 0.0f);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(s->viz, "Draw")) {
		/* Background fills the whole canvas and is never scaled/hopped,
		 * so the size/bounce targets can't open an edge gap behind it. */
		gs_render_start(true);
		draw_background(s, w, h);
		gs_render_stop(GS_TRIS);

		/* Pattern: scale (≥1, so coverage holds) + hop about the centre.
		 * The hop may reveal a thin background strip — that is the
		 * intended "bounce". */
		gs_matrix_push();
		gs_matrix_translate3f(cx, cy + by, 0.0f);
		gs_matrix_scale3f(s->areact.size_mul, s->areact.size_mul, 1.0f);
		gs_matrix_translate3f(-cx, -cy, 0.0f);
		gs_render_start(true);
		switch (s->mode) {
		case GEO_TRIANGLES:
			draw_triangles(s, ctx);
			break;
		case GEO_HEX:
			draw_hex(s, ctx);
			break;
		case GEO_DIAMONDS:
			draw_diamonds(s, ctx);
			break;
		case GEO_MEMPHIS:
			draw_memphis(s, ctx);
			break;
		}
		gs_render_stop(GS_TRIS);
		gs_matrix_pop();
	}
	gs_blend_state_pop();
}

/* ---- properties ---------------------------------------------------------- */

#define ADDF(name, key, lo, hi, step)                                          \
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, name),    \
					obs_module_text(key), lo, hi, step)
#define ADDI(name, key, lo, hi, step)                                          \
	obs_properties_add_int_slider(g, bg_key(k, sizeof(k), PRE, name),      \
				      obs_module_text(key), lo, hi, step)
#define ADDB(name, key)                                                        \
	obs_properties_add_bool(g, bg_key(k, sizeof(k), PRE, name),            \
				obs_module_text(key))
#define ADDC(name, key)                                                        \
	obs_properties_add_color(g, bg_key(k, sizeof(k), PRE, name),           \
				 obs_module_text(key))

static void apply_vis(obs_properties_t *props, int mode)
{
	char k[96];
	obs_property_t *p;
#define VIS(name, cond)                                                        \
	if ((p = obs_properties_get(props, bg_key(k, sizeof(k), PRE, name))))  \
	obs_property_set_visible(p, (cond))
	VIS("line_width", mode == GEO_HEX);
	VIS("fade", mode == GEO_HEX);
	VIS("count", mode == GEO_MEMPHIS);
	VIS("grad_bg", mode == GEO_DIAMONDS || mode == GEO_TRIANGLES ||
			       mode == GEO_HEX);
#undef VIS
}

static bool on_mode(void *priv, obs_properties_t *props, obs_property_t *prop,
		    obs_data_t *settings)
{
	UNUSED_PARAMETER(priv);
	UNUSED_PARAMETER(prop);
	char k[96];
	apply_vis(props,
		  (int)obs_data_get_int(settings,
					bg_key(k, sizeof(k), PRE, "mode")));
	return true;
}

static bool on_transparent(void *priv, obs_properties_t *props,
			   obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(priv);
	UNUSED_PARAMETER(prop);
	char k[96];
	obs_property_t *p = obs_properties_get(props,
		bg_key(k, sizeof(k), PRE, "bgcolor"));
	if (p)
		obs_property_set_visible(p, !obs_data_get_bool(settings,
			bg_key(k, sizeof(k), PRE, "transparent")));
	return true;
}

static void geometric_properties(obs_properties_t *g, obs_data_t *settings)
{
	char k[96];
	obs_property_t *mode = obs_properties_add_list(g,
		bg_key(k, sizeof(k), PRE, "mode"), obs_module_text("GeoMode"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, obs_module_text("GeoModeTri"),
				  GEO_TRIANGLES);
	obs_property_list_add_int(mode, obs_module_text("GeoModeHex"), GEO_HEX);
	obs_property_list_add_int(mode, obs_module_text("GeoModeDiamond"),
				  GEO_DIAMONDS);
	obs_property_list_add_int(mode, obs_module_text("GeoModeMemphis"),
				  GEO_MEMPHIS);
	obs_property_set_modified_callback2(mode, on_mode, NULL);

	ADDF("cell", "GeoCell", 8.0, 400.0, 1.0);
	ADDF("line_width", "GeoLineWidth", 0.0, 30.0, 0.5);
	ADDF("fade", "GeoFade", 0.0, 1.0, 0.01);
	ADDI("count", "GeoCount", 10, 400, 1);

	ADDC("color0", "GeoColor0");
	ADDC("color1", "GeoColor1");
	ADDC("color2", "GeoColor2");
	ADDF("cycle", "GeoCycle", 0.0, 1.0, 0.01);

	ADDF("scroll_x", "GeoScrollX", -400.0, 400.0, 1.0);
	ADDF("scroll_y", "GeoScrollY", -400.0, 400.0, 1.0);
	ADDF("pulse_amt", "GeoPulseAmt", 0.0, 1.0, 0.01);
	ADDF("pulse_speed", "GeoPulseSpeed", 0.0, 8.0, 0.05);
	ADDF("audio_amt", "GeoAudioAmt", 0.0, 2.0, 0.05);
	ADDF("time_scale", "GeoTimeScale", 0.1, 4.0, 0.05);
	ADDF("opacity", "GeoOpacity", 0.0, 1.0, 0.01);

	obs_property_t *tr = obs_properties_add_bool(g,
		bg_key(k, sizeof(k), PRE, "transparent"),
		obs_module_text("GeoTransparent"));
	obs_property_set_modified_callback2(tr, on_transparent, NULL);
	ADDB("grad_bg", "GeoGradBg");
	ADDC("bgcolor", "GeoBgColor");
	ADDI("seed", "GeoSeed", 0, 99999, 1);

	apply_vis(g, settings ? (int)obs_data_get_int(settings,
			bg_key(k, sizeof(k), PRE, "mode")) : GEO_TRIANGLES);
	{
		obs_property_t *p = obs_properties_get(g,
			bg_key(k, sizeof(k), PRE, "bgcolor"));
		if (p)
			obs_property_set_visible(p, settings ?
				!obs_data_get_bool(settings,
					bg_key(k, sizeof(k), PRE,
					       "transparent")) : true);
	}
}

#undef ADDF
#undef ADDI
#undef ADDB
#undef ADDC

static void geometric_defaults(obs_data_t *s)
{
	char k[96];
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "mode"),
				 GEO_TRIANGLES);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "cell"), 80.0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "line_width"),
				    0.0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "fade"), 0.5);
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "count"), 120);
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "color0"),
				 (long long)0xFFDB9C2D); /* #2D9CDB blue */
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "color1"),
				 (long long)0xFFC5D945); /* #45D9C5 teal */
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "color2"),
				 (long long)0xFFFFFFFF); /* white */
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "bgcolor"),
				 (long long)0xFF55362A); /* #2A3655 navy */
	obs_data_set_default_bool(s, bg_key(k, sizeof(k), PRE, "transparent"),
				  false);
	obs_data_set_default_bool(s, bg_key(k, sizeof(k), PRE, "grad_bg"), true);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "scroll_x"),
				    12.0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "scroll_y"),
				    8.0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "cycle"), 0.03);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "pulse_amt"),
				    0.12);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "pulse_speed"),
				    1.0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "audio_amt"),
				    0.5);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "time_scale"),
				    1.0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "opacity"),
				    1.0);
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "seed"), 0);
}

const struct bg_effect bgfx_geometric = {
	.id             = "geometric",
	.name_key       = "EffectGeometric",
	.create         = geometric_create,
	.destroy        = geometric_destroy,
	.load_graphics  = geometric_load_graphics,
	.update         = geometric_update,
	.tick           = geometric_tick,
	.render         = geometric_render,
	.reset          = geometric_reset,
	.get_properties = geometric_properties,
	.get_defaults   = geometric_defaults,
};
