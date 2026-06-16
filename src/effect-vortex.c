#include "effect-vortex.h"
#include "bgfx-effect.h"
#include "bgfx-particles.h"
#include "bgfx-props.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>

#include <math.h>

#define PRE "vortex"
#define CAPACITY 8192
#define BG_TAU 6.28318530718f
#define GOLDEN_ANGLE 2.39996323f /* radians */

enum vortex_mode {
	VTX_CURL = 0,    /* divergence-free curl-noise flow + updraft   */
	VTX_TORNADO = 1, /* orbit a vertical axis, rising funnel         */
	VTX_MULTI = 2,   /* several wandering vortex centres             */
	VTX_PHYLLO = 3,  /* golden-angle fountain, spiral arms           */
};

static const struct bg_common_spec k_spec = {
	.size_min = 1.0, .size_max = 40.0, .size_step = 0.5, .size_def = 5.0,
	.life_min = 0.5, .life_max = 15.0, .life_def = 6.0,
	.rate_max = 2500.0, .rate_def = 400.0,
	.max_cap = CAPACITY, .max_def = 3000,
	.color_def = 0xFFFFC864, /* cyan-blue (#64C8FF) */
	.alpha_def = 0.9,
};

struct vortex_state {
	gs_effect_t *sprite;
	struct bg_particle_system *sys;

	struct bg_common common;
	struct bg_post post;

	int      mode;
	float    updraft;     /* base rise speed, px/s                     */
	float    swirl;       /* swirl strength (per-mode units)            */
	float    radius;      /* funnel radius / launch spread, px          */
	float    turbulence;  /* 0..1 extra wobble                          */
	float    noise_scale; /* curl field spatial frequency (internal)    */
	float    gravity;     /* downward accel, px/s² (phyllotaxis)        */
	int      vortices;    /* 1..4 centres (multi mode)                  */
	float    react;       /* 0..2 audio drive on emission/rise/swirl    */
	uint32_t color2;      /* top-of-gradient colour                     */
	float    grad;        /* 0..1 gradient strength                     */

	float    col1[4], col2[4]; /* precomputed rgba                      */
	double   phi_count;        /* phyllotaxis spawn index               */
};

/* ---- CPU 3D value noise (animated 2D field = slice at z = time) ---------- */

static float fract1(float x) { return x - floorf(x); }
static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

static float hash13(float x, float y, float z)
{
	float h = sinf(x * 127.1f + y * 311.7f + z * 74.7f) * 43758.5453f;
	return fract1(h);
}

static float vnoise3(float x, float y, float z)
{
	float ix = floorf(x), iy = floorf(y), iz = floorf(z);
	float fx = x - ix, fy = y - iy, fz = z - iz;
	float ux = fx * fx * (3.0f - 2.0f * fx);
	float uy = fy * fy * (3.0f - 2.0f * fy);
	float uz = fz * fz * (3.0f - 2.0f * fz);

	float c000 = hash13(ix, iy, iz), c100 = hash13(ix + 1, iy, iz);
	float c010 = hash13(ix, iy + 1, iz), c110 = hash13(ix + 1, iy + 1, iz);
	float c001 = hash13(ix, iy, iz + 1), c101 = hash13(ix + 1, iy, iz + 1);
	float c011 = hash13(ix, iy + 1, iz + 1),
	      c111 = hash13(ix + 1, iy + 1, iz + 1);

	float x00 = lerpf(c000, c100, ux), x10 = lerpf(c010, c110, ux);
	float x01 = lerpf(c001, c101, ux), x11 = lerpf(c011, c111, ux);
	float y0 = lerpf(x00, x10, uy), y1 = lerpf(x01, x11, uy);
	return lerpf(y0, y1, uz);
}

static float fbm3(float x, float y, float z)
{
	float f = 0.0f, amp = 0.5f;
	for (int i = 0; i < 4; ++i) {
		f += amp * vnoise3(x, y, z);
		x *= 2.02f;
		y *= 2.02f;
		amp *= 0.5f;
	}
	return f;
}

/* Divergence-free velocity = curl of the scalar potential ψ = fbm3(x,y,t).
 * Normalised so `swirl` ≈ peak field speed (px/s) regardless of noise scale. */
static void curl_vel(const struct vortex_state *s, float x, float y, float t,
		     float *vx, float *vy)
{
	float ns = s->noise_scale;
	const float E = 2.0f; /* px */
	float zt = t * 0.25f; /* field evolution speed */
	float dpdx = (fbm3((x + E) * ns, y * ns, zt) -
		      fbm3((x - E) * ns, y * ns, zt)) /
		     (2.0f * E);
	float dpdy = (fbm3(x * ns, (y + E) * ns, zt) -
		      fbm3(x * ns, (y - E) * ns, zt)) /
		     (2.0f * E);
	float norm = s->swirl / (ns * 0.5f + 1e-5f);
	*vx = dpdy * norm;
	*vy = -dpdx * norm;
}

/* ---- lifecycle ----------------------------------------------------------- */

static void *vortex_create(void)
{
	struct vortex_state *s = bzalloc(sizeof(*s));
	s->sys = bg_particles_create(CAPACITY);
	s->sys->fade_in = 0.12f;
	s->sys->fade_out = 0.3f;
	return s;
}

static void vortex_destroy(void *data)
{
	struct vortex_state *s = data;
	if (!s)
		return;
	if (s->sprite)
		gs_effect_destroy(s->sprite);
	bg_particles_destroy(s->sys);
	bfree(s);
}

static void vortex_load_graphics(void *data)
{
	struct vortex_state *s = data;
	s->sprite = bg_particles_load_effect();
}

static void vortex_update(void *data, obs_data_t *settings)
{
	struct vortex_state *s = data;
	char k[96];
	bg_common_update(&s->common, settings, PRE);
	bg_post_update(&s->post, settings, PRE);

	s->mode = (int)obs_data_get_int(settings,
		bg_key(k, sizeof(k), PRE, "mode"));
	s->updraft = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "updraft"));
	s->swirl = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "swirl"));
	s->radius = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "radius"));
	s->turbulence = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "turb"));
	/* slider 0.2..5 → feature size; bigger = finer swirls */
	s->noise_scale = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "noise")) * 0.0025f;
	s->gravity = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "gravity"));
	s->vortices = (int)obs_data_get_int(settings,
		bg_key(k, sizeof(k), PRE, "count"));
	s->react = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "react"));
	s->color2 = (uint32_t)obs_data_get_int(settings,
		bg_key(k, sizeof(k), PRE, "color2"));
	s->grad = (float)obs_data_get_double(settings,
		bg_key(k, sizeof(k), PRE, "grad"));

	bg_unpack_color(s->common.color, s->col1);
	bg_unpack_color(s->color2, s->col2);
}

static void vortex_reset(void *data, uint32_t seed)
{
	struct vortex_state *s = data;
	bg_particles_reset(s->sys, seed);
	s->phi_count = 0.0;
}

/* Blend the particle colour from base toward the top colour by height. */
static void apply_grad(const struct vortex_state *s, bg_particle_t *p,
		       float frac)
{
	float t = frac * s->grad;
	p->r = s->col1[0] + (s->col2[0] - s->col1[0]) * t;
	p->g = s->col1[1] + (s->col2[1] - s->col1[1]) * t;
	p->b = s->col1[2] + (s->col2[2] - s->col1[2]) * t;
}

static void vortex_spawn(struct vortex_state *s, const struct bg_ctx *ctx)
{
	bg_particle_t *p = bg_particles_spawn(s->sys);
	if (!p)
		return;
	struct bg_particle_system *sys = s->sys;
	const struct bg_common *c = &s->common;
	float w = (float)ctx->width, h = (float)ctx->height, cx = w * 0.5f;

	p->size = bg_vary(sys, c->size, c->size_var) * 0.5f;
	p->seed = bg_frand(sys);
	p->max_life = p->life = bg_vary(sys, c->lifetime, c->life_var);
	p->a = c->alpha * bg_frand_range(sys, 0.7f, 1.0f);
	p->aux1 = p->a; /* remember the base alpha (tornado depth shading) */

	switch (s->mode) {
	case VTX_TORNADO: {
		float ang = bg_frand(sys) * BG_TAU;
		p->rot = ang;
		p->vrot = (BG_TAU / 360.0f) * s->swirl *
			  bg_frand_range(sys, 0.85f, 1.15f);
		p->aux0 = s->radius * bg_frand_range(sys, 0.35f, 1.0f);
		p->y = h + bg_frand(sys) * h * 0.04f;
		p->vy = -s->updraft * bg_frand_range(sys, 0.7f, 1.25f);
		p->x = cx + p->aux0 * cosf(ang);
		break;
	}
	case VTX_MULTI:
		p->x = bg_frand(sys) * w;
		p->y = h - bg_frand(sys) * h * 0.15f;
		p->vx = bg_frand_range(sys, -20.0f, 20.0f);
		p->vy = -s->updraft * bg_frand_range(sys, 0.4f, 0.9f);
		break;
	case VTX_PHYLLO: {
		float ga = (float)(s->phi_count) * GOLDEN_ANGLE;
		s->phi_count += 1.0;
		float spin = ctx->time * (BG_TAU / 360.0f) * s->swirl * 0.15f;
		float ang = fmodf(ga + spin, BG_TAU);
		float spd = s->updraft * bg_frand_range(sys, 0.9f, 1.15f);
		p->x = cx;
		p->y = h * 0.62f;
		p->vx = cosf(ang) * s->radius * 0.6f;
		p->vy = -spd + sinf(ang) * s->radius * 0.15f;
		break;
	}
	case VTX_CURL:
	default:
		p->x = bg_frand(sys) * w;
		p->y = h - bg_frand(sys) * h * 0.12f;
		p->vx = 0.0f;
		p->vy = -s->updraft * 0.5f;
		break;
	}

	float frac = (h - p->y) / h;
	if (frac < 0.0f)
		frac = 0.0f;
	else if (frac > 1.0f)
		frac = 1.0f;
	apply_grad(s, p, frac);
}

static void vortex_tick(void *data, const struct bg_ctx *ctx, float dt)
{
	struct vortex_state *s = data;
	struct bg_particle_system *sys = s->sys;
	sys->clock += dt;

	float lvl = ctx->audio.enabled ? ctx->audio.level : 0.0f;
	float drive = 1.0f + s->react * lvl;

	size_t cap = (size_t)(s->common.max_count < (int)sys->capacity
				      ? s->common.max_count
				      : (int)sys->capacity);
	sys->emit_accum += s->common.rate * drive * dt;
	while (sys->emit_accum >= 1.0f && sys->live < cap) {
		sys->emit_accum -= 1.0f;
		vortex_spawn(s, ctx);
	}
	if (sys->emit_accum > 8.0f)
		sys->emit_accum = 8.0f;

	float t = ctx->time;
	float w = (float)ctx->width, h = (float)ctx->height, cx = w * 0.5f;
	float up = s->updraft * (1.0f + 0.5f * s->react * lvl);
	float swl = s->swirl * (1.0f + 0.4f * s->react * lvl);
	const float margin = 300.0f;

	/* Wandering vortex centres (Lissajous), shared across particles. */
	float vcx[4], vcy[4], vsign[4];
	int nv = s->vortices < 1 ? 1 : (s->vortices > 4 ? 4 : s->vortices);
	if (s->mode == VTX_MULTI) {
		for (int i = 0; i < nv; ++i) {
			float fi = (float)i;
			float fx = 0.13f + 0.05f * fi, fy = 0.09f + 0.04f * fi;
			vcx[i] = cx + w * 0.30f *
					     sinf(t * fx + 0.6f + 1.7f * fi);
			vcy[i] = h * 0.5f + h * 0.30f *
						    sinf(t * fy + 1.1f + 1.3f * fi);
			vsign[i] = (i % 2 == 0) ? 1.0f : -1.0f;
		}
	}

	for (size_t i = 0; i < sys->live; ++i) {
		bg_particle_t *p = &sys->items[i];
		float frac;

		switch (s->mode) {
		case VTX_TORNADO: {
			p->rot += p->vrot * dt;
			p->y += p->vy * dt;
			frac = (h - p->y) / h;
			if (frac < 0.0f)
				frac = 0.0f;
			else if (frac > 1.0f)
				frac = 1.0f;
			/* Funnel narrows toward the top, with a breathing wobble. */
			float taper = 0.25f + 0.75f * (1.0f - frac);
			float wob = 1.0f + s->turbulence * 0.5f *
						   sinf(t * 1.7f +
							p->seed * BG_TAU +
							frac * 6.0f);
			float R = p->aux0 * taper * wob *
				  (1.0f + 0.3f * s->react * lvl);
			p->x = cx + R * cosf(p->rot);
			/* Pseudo-depth: the front of the orbit reads brighter. */
			float depth = sinf(p->rot);
			p->a = p->aux1 * (0.45f + 0.55f * (0.5f + 0.5f * depth));
			p->len = 0.0f;
			apply_grad(s, p, frac);
			if (p->y < -margin)
				p->life = 0.0f;
			else
				p->life -= dt;
			break;
		}
		case VTX_MULTI: {
			float ax = 0.0f, ay = 0.0f;
			for (int kk = 0; kk < nv; ++kk) {
				float dx = p->x - vcx[kk], dy = p->y - vcy[kk];
				float r = sqrtf(dx * dx + dy * dy) + 1.0f;
				float strg = swl * 200.0f * vsign[kk] /
					     (r * 0.02f + 1.0f);
				ax += (-dy / r) * strg;
				ay += (dx / r) * strg;
			}
			ay -= up; /* rise */
			float a = 4.0f * dt;
			if (a > 1.0f)
				a = 1.0f;
			p->vx += (ax - p->vx) * a;
			p->vy += (ay - p->vy) * a;
			p->x += p->vx * dt;
			p->y += p->vy * dt;
			frac = (h - p->y) / h;
			if (frac < 0.0f)
				frac = 0.0f;
			else if (frac > 1.0f)
				frac = 1.0f;
			apply_grad(s, p, frac);
			if (p->y < -margin || p->x < -margin || p->x > w + margin)
				p->life = 0.0f;
			else
				p->life -= dt;
			break;
		}
		case VTX_PHYLLO:
			p->vy += s->gravity * dt;
			p->x += p->vx * dt;
			p->y += p->vy * dt;
			frac = (h - p->y) / h;
			if (frac < 0.0f)
				frac = 0.0f;
			else if (frac > 1.0f)
				frac = 1.0f;
			apply_grad(s, p, frac);
			if (p->y > h + margin || p->x < -margin ||
			    p->x > w + margin)
				p->life = 0.0f;
			else
				p->life -= dt;
			break;
		case VTX_CURL:
		default: {
			float fvx, fvy;
			curl_vel(s, p->x, p->y, t, &fvx, &fvy);
			float jit = s->turbulence * 40.0f;
			fvx += jit * sinf(t * 2.3f + p->seed * 20.0f);
			fvy += -up + jit * cosf(t * 1.9f + p->seed * 17.0f);
			float a = 6.0f * dt;
			if (a > 1.0f)
				a = 1.0f;
			p->vx += (fvx - p->vx) * a;
			p->vy += (fvy - p->vy) * a;
			p->x += p->vx * dt;
			p->y += p->vy * dt;
			frac = (h - p->y) / h;
			if (frac < 0.0f)
				frac = 0.0f;
			else if (frac > 1.0f)
				frac = 1.0f;
			apply_grad(s, p, frac);
			if (p->y < -margin || p->x < -margin || p->x > w + margin)
				p->life = 0.0f;
			else
				p->life -= dt;
			break;
		}
		}
	}
	bg_particles_compact(sys);
}

static void vortex_render(void *data, const struct bg_ctx *ctx)
{
	struct vortex_state *s = data;
	if (!s->sprite)
		return;
	/* Additive: the particles are light. */
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ONE);
	bg_particles_render(s->sys, s->sprite, BG_SHAPE_SOFT, &s->post,
			    &ctx->audio);
	gs_blend_state_pop();
}

/* Show only the parameters the chosen mode actually reads. */
static void vortex_apply_vis(obs_properties_t *props, int mode)
{
	char k[96];
	obs_property_t *p;
	if ((p = obs_properties_get(props, bg_key(k, sizeof(k), PRE, "noise"))))
		obs_property_set_visible(p, mode == VTX_CURL);
	if ((p = obs_properties_get(props, bg_key(k, sizeof(k), PRE, "gravity"))))
		obs_property_set_visible(p, mode == VTX_PHYLLO);
	if ((p = obs_properties_get(props, bg_key(k, sizeof(k), PRE, "count"))))
		obs_property_set_visible(p, mode == VTX_MULTI);
	if ((p = obs_properties_get(props, bg_key(k, sizeof(k), PRE, "radius"))))
		obs_property_set_visible(p,
			mode == VTX_TORNADO || mode == VTX_PHYLLO);
	if ((p = obs_properties_get(props, bg_key(k, sizeof(k), PRE, "turb"))))
		obs_property_set_visible(p,
			mode == VTX_CURL || mode == VTX_TORNADO);
}

static bool on_vortex_mode(void *priv, obs_properties_t *props,
			   obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(priv);
	UNUSED_PARAMETER(prop);
	char k[96];
	vortex_apply_vis(props, (int)obs_data_get_int(settings,
		bg_key(k, sizeof(k), PRE, "mode")));
	return true;
}

static void vortex_properties(obs_properties_t *g, obs_data_t *settings)
{
	char k[96];

	obs_property_t *mode = obs_properties_add_list(g,
		bg_key(k, sizeof(k), PRE, "mode"), obs_module_text("VortexMode"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, obs_module_text("VortexModeCurl"),
				  VTX_CURL);
	obs_property_list_add_int(mode, obs_module_text("VortexModeTornado"),
				  VTX_TORNADO);
	obs_property_list_add_int(mode, obs_module_text("VortexModeMulti"),
				  VTX_MULTI);
	obs_property_list_add_int(mode, obs_module_text("VortexModePhyllo"),
				  VTX_PHYLLO);
	obs_property_set_modified_callback2(mode, on_vortex_mode, NULL);

	bg_common_props(g, PRE, &k_spec);

	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "updraft"),
		obs_module_text("VortexUpdraft"), 0.0, 600.0, 1.0);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "swirl"),
		obs_module_text("VortexSwirl"), 0.0, 600.0, 1.0);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "radius"),
		obs_module_text("VortexRadius"), 10.0, 800.0, 1.0);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "turb"),
		obs_module_text("VortexTurb"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "noise"),
		obs_module_text("VortexNoiseScale"), 0.2, 5.0, 0.05);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "gravity"),
		obs_module_text("VortexGravity"), 0.0, 2000.0, 10.0);
	obs_properties_add_int_slider(g, bg_key(k, sizeof(k), PRE, "count"),
		obs_module_text("VortexCount"), 1, 4, 1);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "react"),
		obs_module_text("VortexReact"), 0.0, 2.0, 0.05);

	obs_properties_add_color(g, bg_key(k, sizeof(k), PRE, "color2"),
		obs_module_text("VortexColor2"));
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "grad"),
		obs_module_text("VortexGrad"), 0.0, 1.0, 0.01);

	bg_post_props(g, PRE);

	vortex_apply_vis(g, settings ? (int)obs_data_get_int(settings,
		bg_key(k, sizeof(k), PRE, "mode")) : VTX_CURL);
}

static void vortex_defaults(obs_data_t *settings)
{
	char k[96];
	bg_common_defaults(settings, PRE, &k_spec);
	obs_data_set_default_int(settings, bg_key(k, sizeof(k), PRE, "mode"),
				 VTX_CURL);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "updraft"), 120.0);
	obs_data_set_default_double(settings, bg_key(k, sizeof(k), PRE, "swirl"),
				    180.0);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "radius"), 220.0);
	obs_data_set_default_double(settings, bg_key(k, sizeof(k), PRE, "turb"),
				    0.3);
	obs_data_set_default_double(settings, bg_key(k, sizeof(k), PRE, "noise"),
				    2.0);
	obs_data_set_default_double(settings,
		bg_key(k, sizeof(k), PRE, "gravity"), 600.0);
	obs_data_set_default_int(settings, bg_key(k, sizeof(k), PRE, "count"), 3);
	obs_data_set_default_double(settings, bg_key(k, sizeof(k), PRE, "react"),
				    0.8);
	obs_data_set_default_int(settings, bg_key(k, sizeof(k), PRE, "color2"),
				 (long long)0xFFC864FF); /* pink (#FF64C8) */
	obs_data_set_default_double(settings, bg_key(k, sizeof(k), PRE, "grad"),
				    0.6);
	bg_post_defaults(settings, PRE, 0.5, 0.4, 0.4, 0.0);
}

const struct bg_effect bgfx_vortex = {
	.id             = "vortex",
	.name_key       = "EffectVortex",
	.create         = vortex_create,
	.destroy        = vortex_destroy,
	.load_graphics  = vortex_load_graphics,
	.update         = vortex_update,
	.tick           = vortex_tick,
	.render         = vortex_render,
	.reset          = vortex_reset,
	.get_properties = vortex_properties,
	.get_defaults   = vortex_defaults,
};
