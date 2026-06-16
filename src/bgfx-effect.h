#pragma once

#include <obs-module.h>
#include <stdint.h>
#include <stdbool.h>

#include "bgfx-props.h" /* struct bg_audio_mod */

#ifdef __cplusplus
extern "C" {
#endif

/* Read-only frame context handed to an effect each tick/render. The host owns
 * the canvas size, the master clock and the (optionally locked) random seed. */
struct bg_ctx {
	float    time;   /* seconds since the source was last reset      */
	uint32_t width;  /* canvas width in px                            */
	uint32_t height; /* canvas height in px                           */
	uint32_t seed;   /* base RNG seed for deterministic playback      */
	bool     seed_locked; /* true → effects must reseed from `seed`   */

	/* Audio-reactive modulation for this frame (see bg_particles_render).
	 * `audio.enabled` is false when the source has no audio reactivity. */
	struct bg_audio_mod audio;

	/* Analysed audio spectrum for visualizer effects; NULL when audio is
	 * disabled, otherwise check `fft->valid`. Owned by the host. */
	const struct bg_audio_fft *fft;
};

/* A selectable background effect. Each effect owns a per-source-instance,
 * opaque `state` blob and a set of prefixed settings keys; the host owns the
 * canvas size and decides which effect is active.
 *
 * Threading contract (mirrors OBS source callbacks):
 *   - load_graphics / render / destroy run with the OBS graphics lock held
 *     (the host wraps them in obs_enter_graphics()).
 *   - create / update / tick / reset / get_properties / get_defaults run
 *     without the graphics lock.
 * Any callback may be NULL if the effect does not need it. */
struct bg_effect {
	const char *id;       /* stable id stored in settings, e.g. "smoke" */
	const char *name_key; /* locale key shown in the effect selector    */

	void *(*create)(void);
	void  (*destroy)(void *state);

	/* Load GPU resources (shaders, etc.). */
	void  (*load_graphics)(void *state);

	/* Pull this effect's own parameters out of `settings`. */
	void  (*update)(void *state, obs_data_t *settings);

	/* Advance any simulation by `dt` seconds. */
	void  (*tick)(void *state, const struct bg_ctx *ctx, float dt);

	/* Draw one frame. */
	void  (*render)(void *state, const struct bg_ctx *ctx);

	/* Reset transient state (on show() when the host says so, or when the
	 * seed lock kicks in). `seed` is the host seed mixed per effect. */
	void  (*reset)(void *state, uint32_t seed);

	/* Add this effect's properties to `group`. `settings` is the source's
	 * live settings (may be NULL) so the effect can set initial per-mode
	 * visibility; runtime changes are handled via modified callbacks. */
	void  (*get_properties)(obs_properties_t *group, obs_data_t *settings);

	/* Provide this effect's setting defaults. */
	void  (*get_defaults)(obs_data_t *settings);
};

#ifdef __cplusplus
}
#endif
