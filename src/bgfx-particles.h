#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gs_effect;
typedef struct gs_effect gs_effect_t;
struct bg_post;
struct bg_audio_mod;

/* Procedural billboard shapes drawn by particle.effect. */
enum bg_shape {
	BG_SHAPE_SOFT = 0,   /* soft round glow blob          */
	BG_SHAPE_CIRCLE = 1, /* hard-ish disc                 */
	BG_SHAPE_CROSS = 2,  /* 4-point cross-filter sparkle  */
	BG_SHAPE_STAR = 3,   /* 5-point star                  */
	BG_SHAPE_PUFF = 4,   /* fbm-noise smoke puff          */
};

/* One CPU-managed billboard particle. Position/velocity are in canvas pixels
 * (y grows downward); life counts down in seconds. Colour is linear 0..1 and
 * `a` is the base alpha before the lifetime envelope and flicker. */
typedef struct {
	float x, y;
	float vx, vy;
	float life, max_life;
	float size;  /* half-extent across, px                       */
	float len;   /* half-extent along velocity, px; 0 = square   */
	float grow;  /* size multiplier reached at end of life (1=off) */
	float rot, vrot;
	float seed;  /* 0..1 per-particle variation                  */
	float r, g, b, a;
	float flick; /* flicker depth 0..1 (0 = steady)              */
	float dirsign; /* ±1; mirrors per-particle forces (centre-out flows) */
	float aux0, aux1; /* effect-private scratch (e.g. orbit radius/depth) */
} bg_particle_t;

/* A fixed-capacity pool. Live particles occupy [0, live); effects integrate
 * them in their own tick and swap-remove the dead via bg_particles_compact. */
struct bg_particle_system {
	bg_particle_t *items;
	size_t capacity;
	size_t live;
	uint32_t rng;
	float emit_accum;    /* fractional spawn carry                   */
	float clock;         /* flicker phase; advance in the effect tick */
	float fade_in;       /* envelope ramp-in, fraction of life  (0..1) */
	float fade_out;      /* envelope ramp-out, fraction of life (0..1) */
	float flicker_speed; /* flicker cycles per second                */
};

struct bg_particle_system *bg_particles_create(size_t capacity);
void bg_particles_destroy(struct bg_particle_system *s);

/* Clear the pool and reseed its RNG (pass 0 to keep the current seed). */
void bg_particles_reset(struct bg_particle_system *s, uint32_t seed);

/* Uniform random in [0,1) from the pool's xorshift state. */
float bg_frand(struct bg_particle_system *s);
/* Uniform random in [lo,hi). */
float bg_frand_range(struct bg_particle_system *s, float lo, float hi);
/* base * (1 ± var), var in 0..1. */
float bg_vary(struct bg_particle_system *s, float base, float var);

/* Grab a free slot (returns NULL when full). The caller fills it in. */
bg_particle_t *bg_particles_spawn(struct bg_particle_system *s);

/* Swap-remove every particle whose life ran out. Call after integrating. */
void bg_particles_compact(struct bg_particle_system *s);

/* Draw every live particle as a billboard. The caller selects the blend
 * state; the shader outputs premultiplied alpha (use GS_BLEND_ONE,
 * GS_BLEND_INVSRCALPHA, or GS_BLEND_ONE/GS_BLEND_ONE for pure additive).
 * `post` supplies glow/bloom/emissive/lens-flare strengths (may be NULL).
 * `audio` drives the size/colour/bounce reaction (may be NULL or disabled);
 * pass &ctx->audio to make the particles respond to the chosen audio source.
 * Must run under the OBS graphics lock, inside video_render. */
void bg_particles_render(struct bg_particle_system *s, gs_effect_t *e,
			 int shape, const struct bg_post *post,
			 const struct bg_audio_mod *audio);

/* Load data/effects/particle.effect (call under the graphics lock). */
gs_effect_t *bg_particles_load_effect(void);

#ifdef __cplusplus
}
#endif
