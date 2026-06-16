#pragma once

#include <obs-module.h>
#include <stdbool.h>
#include <stdint.h>

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

#ifdef __cplusplus
}
#endif
