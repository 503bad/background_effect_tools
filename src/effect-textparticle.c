/* Random text particles: spawn the characters of a user string as billboard
 * particles, one random glyph each. Each unique character is rasterized once to
 * its own alpha texture (white ink, coverage = alpha) and tinted by the text
 * colour; particles drift like the image-particle effect (flow directions, or a
 * radial burst from the centre) with rotation, sway (ゆらめき) and the shared
 * glow / bloom / emissive(発光) / lens-flare post light.
 *
 * Glyph rasterization is platform code: Windows uses GDI (CreateFontIndirect +
 * TextOut to a DIB). On other platforms no glyphs are produced (the build still
 * compiles; nothing draws). The motion mirrors effect-imgparticle.c. */

#include "effect-textparticle.h"
#include "bgfx-effect.h"
#include "bgfx-particles.h"
#include "bgfx-props.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>

#include <math.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#define PRE "textparticle"
#define CAPACITY 2048
#define BG_TAU 6.28318530718f
#define TP_MAXGLYPH 192
#define TP_RASTER_H 96 /* glyph ink height in px before the particle scale */

enum tp_mode { TP_FLOW = 0, TP_RADIAL = 1 };

struct tp_glyph {
	uint32_t      cp;
	gs_texture_t *tex;
	float         aspect; /* width / height of the padded glyph texture */
};

struct tp_state {
	gs_effect_t *sprite;
	struct bg_particle_system *sys;

	struct bg_common common; /* size / alpha / lifetime / rate / max / colour */
	struct bg_post   post;   /* glow / bloom / emissive / flare               */

	int   mode;     /* TP_FLOW / TP_RADIAL                     */
	int   flow;     /* bg_flow direction (flow mode)           */
	float fan;
	float speed;
	float spread;
	float vrot;     /* rotation speed, rad/s                   */
	float gravity;
	float drag;
	float wobble;   /* ゆらめき: lateral sway + flicker depth   */
	float blur;     /* にじみ / softness                        */

	/* text + font (UTF-8 face) */
	char *text;
	char *font_face;
	int   font_flags;

	/* glyph cache (rebuilt when text/font changes, under graphics lock) */
	struct tp_glyph glyphs[TP_MAXGLYPH];
	int             nglyph;
	char           *built_sig;
};

/* ---- helpers ------------------------------------------------------------- */

static float clamp01f(float v)
{
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static uint32_t pack_rgba(float r, float g, float b, float a)
{
	uint32_t R = (uint32_t)(r < 0 ? 0 : (r > 1 ? 255 : r * 255.0f + 0.5f));
	uint32_t G = (uint32_t)(g < 0 ? 0 : (g > 1 ? 255 : g * 255.0f + 0.5f));
	uint32_t B = (uint32_t)(b < 0 ? 0 : (b > 1 ? 255 : b * 255.0f + 0.5f));
	uint32_t A = (uint32_t)(a < 0 ? 0 : (a > 1 ? 255 : a * 255.0f + 0.5f));
	return (A << 24) | (B << 16) | (G << 8) | R;
}

static double getd(obs_data_t *s, const char *n)
{
	char k[96];
	return obs_data_get_double(s, bg_key(k, sizeof(k), PRE, n));
}

/* Decode the next UTF-8 codepoint from *p; advances *p. Returns 0 at the end. */
static uint32_t utf8_next(const char **p)
{
	const unsigned char *s = (const unsigned char *)*p;
	if (!*s)
		return 0;
	uint32_t cp;
	int n;
	if (*s < 0x80) {
		cp = *s;
		n = 1;
	} else if ((*s & 0xE0) == 0xC0) {
		cp = *s & 0x1F;
		n = 2;
	} else if ((*s & 0xF0) == 0xE0) {
		cp = *s & 0x0F;
		n = 3;
	} else if ((*s & 0xF8) == 0xF0) {
		cp = *s & 0x07;
		n = 4;
	} else {
		cp = *s;
		n = 1;
	}
	for (int i = 1; i < n; ++i) {
		if ((s[i] & 0xC0) != 0x80) {
			n = 1;
			cp = *s;
			break;
		}
		cp = (cp << 6) | (s[i] & 0x3F);
	}
	*p = (const char *)(s + n);
	return cp;
}

/* ---- platform glyph rasterization ---------------------------------------- */

#ifdef _WIN32
/* Render codepoint `cp` to a white-ink BGRA buffer (alpha = coverage), padded
 * so glow has room around the character. Caller bfree()s the returned buffer. */
static uint8_t *tp_raster(uint32_t cp, const char *face, int flags, int *ow,
			  int *oh)
{
	HDC hdc = CreateCompatibleDC(NULL);
	if (!hdc)
		return NULL;

	wchar_t wface[LF_FACESIZE];
	const char *f = (face && face[0]) ? face : "Arial";
	MultiByteToWideChar(CP_UTF8, 0, f, -1, wface, LF_FACESIZE);
	wface[LF_FACESIZE - 1] = 0;

	LOGFONTW lf;
	memset(&lf, 0, sizeof(lf));
	lf.lfHeight = -TP_RASTER_H;
	lf.lfWeight = (flags & OBS_FONT_BOLD) ? FW_BOLD : FW_NORMAL;
	lf.lfItalic = (flags & OBS_FONT_ITALIC) ? 1 : 0;
	lf.lfUnderline = (flags & OBS_FONT_UNDERLINE) ? 1 : 0;
	lf.lfStrikeOut = (flags & OBS_FONT_STRIKEOUT) ? 1 : 0;
	lf.lfQuality = ANTIALIASED_QUALITY;
	lf.lfCharSet = DEFAULT_CHARSET;
	wcsncpy(lf.lfFaceName, wface, LF_FACESIZE - 1);

	HFONT font = CreateFontIndirectW(&lf);
	HFONT oldf = (HFONT)SelectObject(hdc, font);

	wchar_t wc[2];
	int wl;
	if (cp <= 0xFFFF) {
		wc[0] = (wchar_t)cp;
		wl = 1;
	} else {
		uint32_t c = cp - 0x10000;
		wc[0] = (wchar_t)(0xD800 + (c >> 10));
		wc[1] = (wchar_t)(0xDC00 + (c & 0x3FF));
		wl = 2;
	}

	SIZE sz;
	GetTextExtentPoint32W(hdc, wc, wl, &sz);
	int pad = (int)(TP_RASTER_H * 0.4f);
	int w = sz.cx + pad * 2, h = sz.cy + pad * 2;
	if (w < 2)
		w = 2;
	if (h < 2)
		h = 2;

	BITMAPINFO bmi;
	memset(&bmi, 0, sizeof(bmi));
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = w;
	bmi.bmiHeader.biHeight = -h; /* top-down */
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;

	void *bits = NULL;
	HBITMAP dib = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
	uint8_t *out = NULL;
	if (dib && bits) {
		HBITMAP oldb = (HBITMAP)SelectObject(hdc, dib);
		memset(bits, 0, (size_t)w * h * 4);
		SetBkMode(hdc, TRANSPARENT);
		SetTextColor(hdc, RGB(255, 255, 255));
		TextOutW(hdc, pad, pad, wc, wl);
		GdiFlush();

		const uint8_t *src = bits;
		out = bmalloc((size_t)w * h * 4);
		for (int i = 0; i < w * h; ++i) {
			uint8_t b = src[i * 4 + 0];
			uint8_t g = src[i * 4 + 1];
			uint8_t r = src[i * 4 + 2];
			uint8_t cov = b > g ? (b > r ? b : r) : (g > r ? g : r);
			out[i * 4 + 0] = 255; /* B */
			out[i * 4 + 1] = 255; /* G */
			out[i * 4 + 2] = 255; /* R */
			out[i * 4 + 3] = cov; /* A = coverage */
		}
		SelectObject(hdc, oldb);
	}
	if (dib)
		DeleteObject(dib);
	SelectObject(hdc, oldf);
	DeleteObject(font);
	DeleteDC(hdc);

	*ow = w;
	*oh = h;
	return out;
}
#endif /* _WIN32 */

/* Free every cached glyph texture (graphics lock held). */
static void tp_free_glyphs(struct tp_state *s)
{
	for (int i = 0; i < s->nglyph; ++i) {
		if (s->glyphs[i].tex)
			gs_texture_destroy(s->glyphs[i].tex);
		s->glyphs[i].tex = NULL;
	}
	s->nglyph = 0;
}

/* Rebuild the glyph cache from the current text + font (graphics lock held). */
static void tp_build_glyphs(struct tp_state *s)
{
	tp_free_glyphs(s);
	if (!s->text || !s->text[0])
		return;

	const char *p = s->text;
	uint32_t cp;
	while ((cp = utf8_next(&p)) && s->nglyph < TP_MAXGLYPH) {
		if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r')
			continue;
		bool dup = false;
		for (int i = 0; i < s->nglyph; ++i)
			if (s->glyphs[i].cp == cp) {
				dup = true;
				break;
			}
		if (dup)
			continue;

#ifdef _WIN32
		int w = 0, h = 0;
		uint8_t *rgba = tp_raster(cp, s->font_face, s->font_flags, &w,
					  &h);
		if (!rgba)
			continue;
		gs_texture_t *tex = gs_texture_create(
			(uint32_t)w, (uint32_t)h, GS_BGRA, 1,
			(const uint8_t **)&rgba, 0);
		bfree(rgba);
		if (!tex)
			continue;
		s->glyphs[s->nglyph].cp = cp;
		s->glyphs[s->nglyph].tex = tex;
		s->glyphs[s->nglyph].aspect = h > 0 ? (float)w / (float)h : 1.0f;
		s->nglyph++;
#else
		(void)cp; /* no platform rasterizer available */
#endif
	}
}

/* ---- lifecycle ----------------------------------------------------------- */

static const struct bg_common_spec k_spec = {
	.size_min = 4.0, .size_max = 512.0, .size_step = 1.0, .size_def = 64.0,
	.life_min = 0.5, .life_max = 20.0, .life_def = 5.0,
	.rate_max = 200.0, .rate_def = 14.0,
	.max_cap = CAPACITY, .max_def = 160,
	.color_def = 0xFFFFFFFF, /* white text */
	.alpha_def = 1.0,
	.size_var_max = 150,
};

static void *tp_create(void)
{
	struct tp_state *s = bzalloc(sizeof(*s));
	s->sys = bg_particles_create(CAPACITY);
	s->sys->fade_in = 0.15f;
	s->sys->fade_out = 0.2f;
	return s;
}

static void tp_destroy(void *data)
{
	struct tp_state *s = data;
	if (!s)
		return;
	if (s->sprite)
		gs_effect_destroy(s->sprite);
	tp_free_glyphs(s); /* graphics lock held by host */
	bfree(s->text);
	bfree(s->font_face);
	bfree(s->built_sig);
	bg_particles_destroy(s->sys);
	bfree(s);
}

static void tp_load_graphics(void *data)
{
	struct tp_state *s = data;
	s->sprite = bg_particles_load_effect();
}

static void tp_update(void *data, obs_data_t *settings)
{
	struct tp_state *s = data;
	char k[96];
	bg_common_update(&s->common, settings, PRE);
	bg_post_update(&s->post, settings, PRE);

	s->mode = (int)obs_data_get_int(settings,
		bg_key(k, sizeof(k), PRE, "mode"));
	s->flow = bg_flow_update(settings, PRE);
	s->fan = bg_flow_fan_update(settings, PRE);
	s->speed = (float)getd(settings, "speed");
	s->spread = (float)getd(settings, "spread");
	s->vrot = (float)getd(settings, "rotate") * (BG_TAU / 360.0f);
	s->gravity = (float)getd(settings, "gravity");
	s->drag = (float)getd(settings, "drag");
	s->wobble = (float)getd(settings, "wobble");
	s->blur = (float)getd(settings, "blur");

	s->sys->softness = s->blur;
	s->sys->fade_in = (float)getd(settings, "fade_in");
	s->sys->fade_out = (float)getd(settings, "fade_out");

	const char *text = obs_data_get_string(settings,
		bg_key(k, sizeof(k), PRE, "text"));
	bfree(s->text);
	s->text = bstrdup(text ? text : "");

	obs_data_t *font = obs_data_get_obj(settings,
		bg_key(k, sizeof(k), PRE, "font"));
	bfree(s->font_face);
	s->font_face = NULL;
	s->font_flags = 0;
	if (font) {
		const char *face = obs_data_get_string(font, "face");
		s->font_face = bstrdup(face ? face : "");
		s->font_flags = (int)obs_data_get_int(font, "flags");
		obs_data_release(font);
	}
}

static void tp_reset(void *data, uint32_t seed)
{
	struct tp_state *s = data;
	bg_particles_reset(s->sys, seed);
}

/* ---- simulation ---------------------------------------------------------- */

static void tp_spawn(struct tp_state *s, const struct bg_ctx *ctx)
{
	if (s->nglyph < 1)
		return;
	bg_particle_t *p = bg_particles_spawn(s->sys);
	if (!p)
		return;
	struct bg_particle_system *sys = s->sys;
	const struct bg_common *c = &s->common;

	float W = (float)ctx->width, H = (float)ctx->height;
	float m = c->size + 8.0f;
	float sp = s->speed;
	float x, y, vx = 0.0f, vy = 0.0f, dirsign = 1.0f;

	if (s->mode == TP_RADIAL) {
		/* Burst outward from the centre in every direction. */
		float a = bg_frand(sys) * BG_TAU;
		x = W * 0.5f;
		y = H * 0.5f;
		float v = sp * bg_frand_range(sys, 0.6f, 1.0f);
		vx = cosf(a) * v;
		vy = sinf(a) * v;
	} else {
		switch (s->flow) {
		case BG_FLOW_DOWN:
			x = bg_frand(sys) * W; y = -m; vy = sp; break;
		case BG_FLOW_LEFT:
			x = W + m; y = bg_frand(sys) * H; vx = -sp; break;
		case BG_FLOW_RIGHT:
			x = -m; y = bg_frand(sys) * H; vx = sp; break;
		case BG_FLOW_LR:
			dirsign = (bg_frand(sys) < 0.5f) ? -1.0f : 1.0f;
			x = W * 0.5f; y = bg_frand(sys) * H;
			vx = dirsign * sp; break;
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
	p->len = 0.0f;
	p->grow = 1.0f;
	p->max_life = p->life = bg_vary(sys, c->lifetime, c->life_var);
	p->rot = bg_frand(sys) * BG_TAU;
	p->vrot = s->vrot * (1.0f + 0.2f * bg_frand_range(sys, -1.0f, 1.0f));
	p->seed = bg_frand(sys);
	p->r = rgba[0];
	p->g = rgba[1];
	p->b = rgba[2];
	p->a = c->alpha;
	p->flick = s->wobble * 0.5f; /* ゆらめき brightness shimmer */
	p->dirsign = dirsign;
	p->aux0 = (float)(int)(bg_frand(sys) * (float)s->nglyph); /* glyph idx */
	p->aux1 = bg_frand_range(sys, 0.0f, BG_TAU);              /* sway phase */
}

static void tp_tick(void *data, const struct bg_ctx *ctx, float dt)
{
	struct tp_state *s = data;
	struct bg_particle_system *sys = s->sys;
	sys->clock += dt;

	size_t cap = (size_t)(s->common.max_count < (int)sys->capacity
				      ? s->common.max_count
				      : (int)sys->capacity);
	if (s->nglyph > 0) {
		sys->emit_accum += s->common.rate * dt;
		while (sys->emit_accum >= 1.0f && sys->live < cap) {
			sys->emit_accum -= 1.0f;
			tp_spawn(s, ctx);
		}
		if (sys->emit_accum > 4.0f)
			sys->emit_accum = 4.0f;
	}

	float dragf = 1.0f - s->drag * dt;
	if (dragf < 0.0f)
		dragf = 0.0f;
	float sway = s->wobble * 26.0f; /* px lateral drift amplitude */

	for (size_t i = 0; i < sys->live; ++i) {
		bg_particle_t *p = &sys->items[i];
		p->vy += s->gravity * dt;
		if (s->drag != 0.0f) {
			p->vx *= dragf;
			p->vy *= dragf;
		}
		p->x += p->vx * dt;
		p->y += p->vy * dt;
		if (sway > 0.0f) {
			/* ゆらめき: gentle seed-phased horizontal shimmer. */
			p->x += sway * dt *
				sinf(sys->clock * 2.3f + p->aux1 +
				     p->seed * 7.0f);
		}
		p->rot += p->vrot * dt;
		p->life -= dt;
	}
	bg_particles_compact(sys);
}

/* ---- rendering (per-glyph draw groups) ----------------------------------- */

static void tp_set_f(gs_effect_t *e, const char *n, float v)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(e, n);
	if (p)
		gs_effect_set_float(p, v);
}

static void tp_render(void *data, const struct bg_ctx *ctx)
{
	struct tp_state *s = data;
	if (!s->sprite)
		return;
	UNUSED_PARAMETER(ctx);

	/* Rebuild the glyph cache when the text or font changed. */
	char sig[1024];
	snprintf(sig, sizeof(sig), "%d|%s|%s", s->font_flags,
		 s->font_face ? s->font_face : "", s->text ? s->text : "");
	if (!s->built_sig || strcmp(s->built_sig, sig) != 0) {
		tp_build_glyphs(s);
		bfree(s->built_sig);
		s->built_sig = bstrdup(sig);
	}
	if (s->nglyph < 1 || s->sys->live == 0)
		return;

	const float QS = 2.4f; /* matches BG_QUAD_SCALE in bgfx-particles.c */
	gs_effect_t *e = s->sprite;
	tp_set_f(e, "shape", (float)BG_SHAPE_IMAGE);
	tp_set_f(e, "glow", s->post.glow);
	tp_set_f(e, "bloom", s->post.bloom);
	tp_set_f(e, "emissive", s->post.emissive);
	tp_set_f(e, "flare", s->post.flare);
	tp_set_f(e, "quad_scale", QS);
	tp_set_f(e, "softness", s->sys->softness);
	tp_set_f(e, "use_image", 1.0f);

	struct bg_particle_system *sys = s->sys;
	gs_eparam_t *ptex = gs_effect_get_param_by_name(e, "image_tex");

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	/* One draw batch per glyph so each can bind its own texture. */
	for (int gi = 0; gi < s->nglyph; ++gi) {
		struct tp_glyph *gl = &s->glyphs[gi];
		if (!gl->tex)
			continue;
		if (ptex)
			gs_effect_set_texture(ptex, gl->tex);

		while (gs_effect_loop(e, "Draw")) {
			gs_render_start(true);
			for (size_t i = 0; i < sys->live; ++i) {
				bg_particle_t *q = &sys->items[i];
				if ((int)q->aux0 != gi)
					continue;

				float age = q->max_life - q->life;
				float age_frac = q->max_life > 0.0f
							 ? age / q->max_life
							 : 0.0f;
				float env = 1.0f;
				float fi = sys->fade_in * q->max_life;
				float fo = sys->fade_out * q->max_life;
				if (fi > 0.0f && age < fi)
					env *= age / fi;
				if (fo > 0.0f && q->life < fo)
					env *= q->life / fo;

				float fl = 1.0f;
				if (q->flick > 0.0f) {
					float ph = (sys->clock *
							    sys->flicker_speed +
						    q->seed * 7.31f) * BG_TAU;
					float wv = 0.5f + 0.5f * sinf(ph) *
							   cosf(ph * 0.37f +
								q->seed * 11.0f);
					fl = 1.0f - q->flick * wv;
				}

				float a = q->a * env * fl;
				if (a <= 0.003f)
					continue;

				float across = q->size * QS;       /* half height */
				float along = across * gl->aspect; /* half width  */
				float c = cosf(q->rot), sn = sinf(q->rot);
				uint32_t col = pack_rgba(q->r, q->g, q->b, a);

				float cx[4] = {-along, along, along, -along};
				float cy[4] = {-across, -across, across, across};
				float ux[4] = {0.0f, 1.0f, 1.0f, 0.0f};
				float uy[4] = {0.0f, 0.0f, 1.0f, 1.0f};
				float px[4], py[4];
				for (int kk = 0; kk < 4; ++kk) {
					px[kk] = q->x + cx[kk] * c - cy[kk] * sn;
					py[kk] = q->y + cx[kk] * sn + cy[kk] * c;
				}
				int idx[6] = {0, 1, 2, 0, 2, 3};
				for (int t = 0; t < 6; ++t) {
					int kk = idx[t];
					gs_texcoord(ux[kk], uy[kk], 0);
					gs_texcoord(q->seed, age_frac, 1);
					gs_color(col);
					gs_vertex2f(px[kk], py[kk]);
				}
			}
			gs_render_stop(GS_TRIS);
		}
	}

	gs_blend_state_pop();
}

/* ---- properties ---------------------------------------------------------- */

static void tp_properties(obs_properties_t *g, obs_data_t *settings)
{
	char k[96];

	obs_properties_add_text(g, bg_key(k, sizeof(k), PRE, "text"),
		obs_module_text("TPText"), OBS_TEXT_MULTILINE);
	obs_properties_add_font(g, bg_key(k, sizeof(k), PRE, "font"),
		obs_module_text("TPFont"));

	obs_property_t *mode = obs_properties_add_list(g,
		bg_key(k, sizeof(k), PRE, "mode"), obs_module_text("TPMode"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, obs_module_text("TPModeFlow"), TP_FLOW);
	obs_property_list_add_int(mode, obs_module_text("TPModeRadial"),
				  TP_RADIAL);

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
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "wobble"),
		obs_module_text("TPWobble"), 0.0, 1.0, 0.01);

	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "fade_in"),
		obs_module_text("ImgParticleFadeIn"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "fade_out"),
		obs_module_text("ImgParticleFadeOut"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "blur"),
		obs_module_text("ImgParticleBlur"), 0.0, 1.0, 0.01);

	bg_post_props(g, PRE);
}

static void tp_defaults(obs_data_t *settings)
{
	char k[96];
	bg_common_defaults(settings, PRE, &k_spec);
	bg_flow_defaults(settings, PRE);

	obs_data_set_default_string(settings, bg_key(k, sizeof(k), PRE, "text"),
				    "ABC123");
	obs_data_set_default_int(settings, bg_key(k, sizeof(k), PRE, "mode"),
				 TP_FLOW);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "rotate"), 20.0);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "speed"), 120.0);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "spread"), 0.25);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "gravity"), 0.0);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "drag"), 0.1);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "wobble"), 0.3);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "fade_in"), 0.15);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "fade_out"), 0.2);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "blur"), 0.0);

	bg_post_defaults(settings, PRE, 0.5, 0.4, 0.3, 0.0);
}

const struct bg_effect bgfx_textparticle = {
	.id             = "textparticle",
	.name_key       = "EffectTextParticle",
	.create         = tp_create,
	.destroy        = tp_destroy,
	.load_graphics  = tp_load_graphics,
	.update         = tp_update,
	.tick           = tp_tick,
	.render         = tp_render,
	.reset          = tp_reset,
	.get_properties = tp_properties,
	.get_defaults   = tp_defaults,
};
