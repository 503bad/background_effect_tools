#pragma once

#include <obs-module.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

struct bg_audio_mod;
struct bg_audio_fft;

/* Audio meter: attaches an audio capture callback to a chosen OBS source and
 * turns its PCM into a smoothed level plus an FFT spectrum / band energies /
 * beat detection the effects can react to. One meter per source instance; the
 * callback runs on the audio thread, analysis runs on the video thread
 * (mutex-protected PCM hand-off). */
struct bg_audio_meter;

struct bg_audio_meter *bg_audio_create(void);
void bg_audio_destroy(struct bg_audio_meter *m);

/* (Re)point the meter at the named source. NULL or "" detaches. Safe to call
 * every update; it only re-attaches when the name actually changes. */
void bg_audio_set_source(struct bg_audio_meter *m, const char *name);

/* Analyse the audio captured since the last call (level, FFT spectrum, bands,
 * beat) and return the smoothed overall level (0..1). Call once per video
 * frame.
 *   gain_db  - input gain in dB (also scales the spectrum)
 *   attack   - rise smoothing time constant, seconds (snappier when small)
 *   release  - fall smoothing time constant, seconds                       */
float bg_audio_tick(struct bg_audio_meter *m, float dt, float gain_db,
		    float attack, float release);

/* The most recent analysis frame (valid until the next bg_audio_tick). Never
 * NULL for a live meter; check the returned struct's `valid` flag. */
const struct bg_audio_fft *bg_audio_get_fft(const struct bg_audio_meter *m);

/* ---- shared "audio reactive" UI (lives on the source, not per-effect) ---- */

/* Populate `g` with the audio-reactive controls (source picker, gain/attack/
 * release, and the size/color/bounce target toggles + amounts). */
void bg_audio_props(obs_properties_t *g);
void bg_audio_defaults(obs_data_t *s);

/* Pull the target toggles/amounts into `mod` and the meter response settings
 * into the out params. Does not touch mod->enabled or mod->level. */
void bg_audio_read(struct bg_audio_mod *mod, float *gain_db, float *attack,
		   float *release, obs_data_t *s);

/* ---- shared audio reaction for non-particle (geometry) effects ----------- */

/* Frame-constant factors derived from the shared size / colour / bounce
 * targets, for effects that draw geometry directly instead of going through
 * bg_particles_render (which applies the same targets to particles). Fill once
 * per frame with bg_audio_react_init, then use the inline helpers while
 * drawing. Mirrors the semantics in bg_particles_render. */
struct bg_audio_react {
	float size_mul;      /* multiply shape size/scale (1 = none)         */
	float bounce_amp;    /* vertical hop amplitude, px (0 = off)         */
	float bounce_speed;  /* hop oscillation, Hz                          */
	bool  color_on;      /* shift colours this frame                     */
	bool  color_peak_on; /* blend toward peak colour (else brighten)     */
	float color_amt;     /* 0..1 strength this frame (amount × level)    */
	float peak[4];       /* peak colour rgba (when color_peak_on)        */
};

/* Derive this frame's reaction from the shared mod. With audio disabled or a
 * zero level, yields the identity (size_mul = 1, no bounce, no colour). */
void bg_audio_react_init(struct bg_audio_react *r, const struct bg_audio_mod *m);

/* Vertical hop offset (px, ≤ 0) for a shape at oscillator phase `phase`
 * (radians); 0 when bounce is off. */
static inline float bg_audio_react_bounce(const struct bg_audio_react *r,
					  float clock, float phase)
{
	if (r->bounce_amp <= 0.0f)
		return 0.0f;
	float v = sinf(clock * r->bounce_speed * 6.28318530718f + phase);
	return -r->bounce_amp * (v < 0.0f ? -v : v);
}

/* Apply the colour reaction to `rgb` in place (no-op when colour is off). */
static inline void bg_audio_react_color(const struct bg_audio_react *r,
					float rgb[3])
{
	if (!r->color_on)
		return;
	float amt = r->color_amt;
	if (r->color_peak_on) {
		rgb[0] += (r->peak[0] - rgb[0]) * amt;
		rgb[1] += (r->peak[1] - rgb[1]) * amt;
		rgb[2] += (r->peak[2] - rgb[2]) * amt;
	} else {
		rgb[0] *= 1.0f + amt;
		rgb[1] *= 1.0f + amt;
		rgb[2] *= 1.0f + amt;
	}
}

#ifdef __cplusplus
}
#endif
