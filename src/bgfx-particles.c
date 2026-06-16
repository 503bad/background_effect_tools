#include "bgfx-particles.h"
#include "bgfx-props.h"

#include <obs.h>
#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>

#include <math.h>
#include <string.h>

#include <plugin-support.h>

#define BG_TAU 6.28318530718f

/* The quad is drawn larger than the particle core so halos and flare streaks
 * have room; particle.effect maps uv back with the same factor. */
#define BG_QUAD_SCALE 2.4f

struct bg_particle_system *bg_particles_create(size_t capacity)
{
	struct bg_particle_system *s = bzalloc(sizeof(*s));
	s->capacity = capacity;
	s->items = bzalloc(sizeof(bg_particle_t) * capacity);
	s->rng = 0x1234567u;
	s->fade_in = 0.1f;
	s->fade_out = 0.3f;
	s->flicker_speed = 6.0f;
	return s;
}

void bg_particles_destroy(struct bg_particle_system *s)
{
	if (!s)
		return;
	bfree(s->items);
	bfree(s);
}

void bg_particles_reset(struct bg_particle_system *s, uint32_t seed)
{
	if (!s)
		return;
	s->live = 0;
	s->emit_accum = 0.0f;
	s->clock = 0.0f;
	if (seed)
		s->rng = seed;
}

static uint32_t xs32(uint32_t *st)
{
	uint32_t x = *st ? *st : 0x9e3779b9u;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*st = x;
	return x;
}

float bg_frand(struct bg_particle_system *s)
{
	return (float)(xs32(&s->rng) & 0xFFFFFF) / (float)0x1000000;
}

float bg_frand_range(struct bg_particle_system *s, float lo, float hi)
{
	return lo + (hi - lo) * bg_frand(s);
}

float bg_vary(struct bg_particle_system *s, float base, float var)
{
	if (var <= 0.0f)
		return base;
	float v = base * (1.0f + var * (bg_frand(s) * 2.0f - 1.0f));
	return v > 0.0f ? v : 0.01f;
}

bg_particle_t *bg_particles_spawn(struct bg_particle_system *s)
{
	if (!s || s->live >= s->capacity)
		return NULL;
	bg_particle_t *p = &s->items[s->live++];
	memset(p, 0, sizeof(*p));
	p->grow = 1.0f;
	p->dirsign = 1.0f;
	return p;
}

void bg_particles_compact(struct bg_particle_system *s)
{
	if (!s)
		return;
	for (size_t i = 0; i < s->live;) {
		if (s->items[i].life <= 0.0f)
			s->items[i] = s->items[--s->live];
		else
			++i;
	}
}

static uint32_t pack_rgba(float r, float g, float b, float a)
{
	uint32_t R = (uint32_t)(r < 0 ? 0 : (r > 1 ? 255 : r * 255.0f + 0.5f));
	uint32_t G = (uint32_t)(g < 0 ? 0 : (g > 1 ? 255 : g * 255.0f + 0.5f));
	uint32_t B = (uint32_t)(b < 0 ? 0 : (b > 1 ? 255 : b * 255.0f + 0.5f));
	uint32_t A = (uint32_t)(a < 0 ? 0 : (a > 1 ? 255 : a * 255.0f + 0.5f));
	return (A << 24) | (B << 16) | (G << 8) | R; /* OBS 0xAABBGGRR */
}

void bg_particles_render(struct bg_particle_system *s, gs_effect_t *e,
			 int shape, const struct bg_post *post,
			 const struct bg_audio_mod *audio)
{
	if (!s || !e || s->live == 0)
		return;

	float glow = post ? post->glow : 0.0f;
	float bloom = post ? post->bloom : 0.0f;
	float emissive = post ? post->emissive : 0.0f;
	float flare = post ? post->flare : 0.0f;

	/* Audio reaction for this frame (all targets independent). */
	float a_lvl = (audio && audio->enabled) ? audio->level : 0.0f;
	bool a_size = a_lvl > 0.0f && audio->size_on;
	bool a_color = a_lvl > 0.0f && audio->color_on;
	bool a_bounce = a_lvl > 0.0f && audio->bounce_on;
	float size_mul = a_size ? 1.0f + audio->size_amount * a_lvl : 1.0f;
	float peak[4] = {0, 0, 0, 0};
	if (a_color && audio->color_peak_on)
		bg_unpack_color(audio->color_peak, peak);

	gs_eparam_t *p;
	if ((p = gs_effect_get_param_by_name(e, "shape")))
		gs_effect_set_float(p, (float)shape);
	if ((p = gs_effect_get_param_by_name(e, "glow")))
		gs_effect_set_float(p, glow);
	if ((p = gs_effect_get_param_by_name(e, "bloom")))
		gs_effect_set_float(p, bloom);
	if ((p = gs_effect_get_param_by_name(e, "emissive")))
		gs_effect_set_float(p, emissive);
	if ((p = gs_effect_get_param_by_name(e, "flare")))
		gs_effect_set_float(p, flare);
	if ((p = gs_effect_get_param_by_name(e, "quad_scale")))
		gs_effect_set_float(p, BG_QUAD_SCALE);

	while (gs_effect_loop(e, "Draw")) {
		gs_render_start(true);
		for (size_t i = 0; i < s->live; ++i) {
			bg_particle_t *q = &s->items[i];
			float age = q->max_life - q->life;
			float age_frac = q->max_life > 0.0f
						 ? age / q->max_life
						 : 0.0f;

			/* Lifetime envelope: ramp in, hold, ramp out. */
			float env = 1.0f;
			float fi = s->fade_in * q->max_life;
			float fo = s->fade_out * q->max_life;
			if (fi > 0.0f && age < fi)
				env *= age / fi;
			if (fo > 0.0f && q->life < fo)
				env *= q->life / fo;

			/* Per-particle flicker / twinkle. */
			float fl = 1.0f;
			if (q->flick > 0.0f) {
				float ph = (s->clock * s->flicker_speed +
					    q->seed * 7.31f) * BG_TAU;
				float w = 0.5f + 0.5f * sinf(ph) *
					  cosf(ph * 0.37f + q->seed * 11.0f);
				fl = 1.0f - q->flick * w;
			}

			float a = q->a * env * fl;
			if (a <= 0.003f)
				continue;

			float scale = 1.0f + (q->grow - 1.0f) * age_frac;
			float across = q->size * scale * BG_QUAD_SCALE * size_mul;
			float along = q->len > 0.0f
					      ? q->len * scale * BG_QUAD_SCALE *
							size_mul
					      : across;

			/* Audio bounce: a per-particle hop whose height tracks
			 * the level; the seed phase keeps them out of lockstep. */
			float bx = 0.0f, by = 0.0f;
			if (a_bounce) {
				float amp = audio->bounce_amount * a_lvl;
				float ph = s->clock * audio->bounce_speed *
						   BG_TAU +
					   q->seed * BG_TAU;
				by = -amp * fabsf(sinf(ph));
			}

			/* Audio colour: blend toward a peak colour, or brighten. */
			float pr = q->r, pg = q->g, pb = q->b;
			if (a_color) {
				float amt = audio->color_amount * a_lvl;
				if (audio->color_peak_on) {
					pr += (peak[0] - pr) * amt;
					pg += (peak[1] - pg) * amt;
					pb += (peak[2] - pb) * amt;
				} else {
					pr *= 1.0f + amt;
					pg *= 1.0f + amt;
					pb *= 1.0f + amt;
				}
			}

			float c, sn;
			if (q->len > 0.0f) {
				/* Orient the long axis along the velocity. */
				float ang = atan2f(q->vy, q->vx);
				c = cosf(ang);
				sn = sinf(ang);
			} else {
				c = cosf(q->rot);
				sn = sinf(q->rot);
			}

			uint32_t col = pack_rgba(pr, pg, pb, a);

			/* Quad corners: x along the (rotated) long axis. */
			float ox = q->x + bx, oy = q->y + by;
			float cx[4] = {-along, along, along, -along};
			float cy[4] = {-across, -across, across, across};
			float ux[4] = {0.0f, 1.0f, 1.0f, 0.0f};
			float uy[4] = {0.0f, 0.0f, 1.0f, 1.0f};
			float px[4], py[4];
			for (int k = 0; k < 4; ++k) {
				px[k] = ox + cx[k] * c - cy[k] * sn;
				py[k] = oy + cx[k] * sn + cy[k] * c;
			}
			int idx[6] = {0, 1, 2, 0, 2, 3};
			for (int t = 0; t < 6; ++t) {
				int k = idx[t];
				gs_texcoord(ux[k], uy[k], 0);
				gs_texcoord(q->seed, age_frac, 1);
				gs_color(col);
				gs_vertex2f(px[k], py[k]);
			}
		}
		gs_render_stop(GS_TRIS);
	}
}

gs_effect_t *bg_particles_load_effect(void)
{
	char *path = obs_module_file("effects/particle.effect");
	gs_effect_t *e = NULL;
	if (path) {
		e = gs_effect_create_from_file(path, NULL);
		if (!e)
			obs_log(LOG_ERROR, "failed to load particle.effect (%s)",
				path);
	}
	bfree(path);
	return e;
}
