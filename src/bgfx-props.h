#pragma once

#include <obs-module.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

struct bg_particle_system;
struct bg_particle_t;

/* Compose a prefixed settings key, e.g. bg_key(buf, sizeof(buf), "smoke",
 * "size") → "smoke_size". Every effect namespaces its keys this way because
 * all effects share one obs_data_t. */
static inline const char *bg_key(char *buf, size_t n, const char *pre,
				 const char *name)
{
	snprintf(buf, n, "%s_%s", pre, name);
	return buf;
}

/* Decompose an OBS 0xAABBGGRR colour into normalized rgba. */
void bg_unpack_color(uint32_t c, float rgba[4]);

/* ---- common particle parameters (size/alpha/colour/lifetime/rate/max) ---- */

struct bg_common {
	float    size;     /* base half-extent, px       */
	float    size_var; /* 0..1 random variation       */
	float    alpha;    /* 0..1 overall opacity        */
	uint32_t color;    /* OBS 0xAABBGGRR              */
	float    lifetime; /* base seconds                */
	float    life_var; /* 0..1 random variation       */
	float    rate;     /* spawns per second           */
	int      max_count;/* live-particle cap           */
};

/* UI ranges/defaults differ per effect; each effect fills one of these. */
struct bg_common_spec {
	double size_min, size_max, size_step, size_def;
	double life_min, life_max, life_def;
	double rate_max, rate_def;
	int    max_cap, max_def;
	long long color_def; /* OBS 0xAABBGGRR */
	double alpha_def;
};

void bg_common_props(obs_properties_t *g, const char *pre,
		     const struct bg_common_spec *spec);
void bg_common_defaults(obs_data_t *s, const char *pre,
			const struct bg_common_spec *spec);
void bg_common_update(struct bg_common *c, obs_data_t *s, const char *pre);

/* ---- flow direction (drifting effects: smoke, embers) -------------------- */

enum bg_flow {
	BG_FLOW_UP = 0,    /* spawn at the bottom, drift up            */
	BG_FLOW_DOWN = 1,  /* spawn at the top, drift down             */
	BG_FLOW_LEFT = 2,  /* spawn at the right edge, drift left      */
	BG_FLOW_RIGHT = 3, /* spawn at the left edge, drift right      */
	BG_FLOW_LR = 4,    /* spawn at the centre, drift to both sides */
	BG_FLOW_UP_FAN = 5,/* spawn at the bottom centre, rise while
			    * fanning out left and right               */
	BG_FLOW_UP_CURVE = 6,/* launch sideways (away from the centre),
			      * bleed off the sideways speed and ease
			      * into a gentle climb — a curved rise     */
};

void bg_flow_props(obs_properties_t *g, const char *pre, obs_data_t *settings);
void bg_flow_defaults(obs_data_t *s, const char *pre);
int  bg_flow_update(obs_data_t *s, const char *pre);
/* Fan spread amount (0..1) for BG_FLOW_UP_FAN. */
float bg_flow_fan_update(obs_data_t *s, const char *pre);

/* True when the flow runs along the x axis (left/right/centre-out). */
static inline bool bg_flow_horizontal(int flow)
{
	return flow >= BG_FLOW_LEFT && flow <= BG_FLOW_LR;
}

/* ---- wind ---------------------------------------------------------------- */

struct bg_wind {
	bool  on;
	float strength; /* px/s² push                       */
	float dir_deg;  /* 0..360, 0 = right, 90 = up        */
	float turb;     /* 0..1 turbulence / scatter amount  */
};

void bg_wind_props(obs_properties_t *g, const char *pre);
void bg_wind_defaults(obs_data_t *s, const char *pre);
void bg_wind_update(struct bg_wind *w, obs_data_t *s, const char *pre);

/* Acceleration (px/s²) the wind applies to a particle with variation `seed`
 * at time `t`. Returns zero when the wind is off. */
void bg_wind_accel(const struct bg_wind *w, float t, float seed, float *ax,
		   float *ay);

/* ---- post effects (glow / bloom / emissive / lens flare) ----------------- */

struct bg_post {
	float glow;     /* soft halo around the particle      */
	float bloom;    /* wide light spill                    */
	float emissive; /* core brightness boost               */
	float flare;    /* anamorphic streak + cross flare     */
};

void bg_post_props(obs_properties_t *g, const char *pre);
void bg_post_defaults(obs_data_t *s, const char *pre, double glow,
		      double bloom, double emissive, double flare);
void bg_post_update(struct bg_post *p, obs_data_t *s, const char *pre);

/* ---- audio reactivity --------------------------------------------------- */

/* How the active particle effect should react to the captured audio level.
 * `enabled` and `level` are owned by the host (filled per frame); the rest are
 * the user-chosen targets. Each target is independent and may be combined.
 * Applied centrally in bg_particles_render so every particle effect reacts. */
struct bg_audio_mod {
	bool     enabled;     /* master switch (the source's audio group)    */
	float    level;       /* 0..1 smoothed level, set by the host        */

	bool     size_on;     /* swell particle size with the level          */
	float    size_amount; /* extra size multiplier at full level (1.5 …) */

	bool     color_on;        /* shift colour with the level             */
	float    color_amount;    /* blend/brighten strength 0..1 at full    */
	bool     color_peak_on;   /* blend toward color_peak (else brighten) */
	uint32_t color_peak;      /* OBS 0xAABBGGRR colour reached at peak    */

	bool     bounce_on;       /* make particles hop with the level       */
	float    bounce_amount;   /* hop height in px at full level          */
	float    bounce_speed;    /* hop oscillation in Hz                   */
};

/* ---- FFT spectrum analysis (for the audio visualizer effect) ------------ */

#define BG_FFT_BARS    64  /* log-spaced spectrum bars                    */
#define BG_WAVE_POINTS 128 /* time-domain points for the oscilloscope      */

/* One frame of analysed audio. Filled by the host from the meter and handed
 * to effects via bg_ctx.fft (NULL/!valid when no audio is being analysed). */
struct bg_audio_fft {
	bool  valid;                  /* a source is attached & analysing     */
	int   bar_count;              /* = BG_FFT_BARS                        */
	float bars[BG_FFT_BARS];      /* 0..1 smoothed magnitude, low→high    */
	int   wave_count;             /* = BG_WAVE_POINTS                     */
	float wave[BG_WAVE_POINTS];   /* -1..1 time-domain waveform           */
	float bass, mid, treble;      /* 0..1 smoothed band energies          */
	float beat;                   /* 0..1 pulse, decays after an onset    */
	bool  beat_trigger;           /* true on the frame an onset fires     */
	float level;                  /* 0..1 overall level (RMS)             */
};

#ifdef __cplusplus
}
#endif
