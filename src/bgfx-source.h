#pragma once

#include <obs-module.h>
#include <stdbool.h>
#include <stdint.h>

#include "bgfx-props.h" /* struct bg_audio_mod */

#ifdef __cplusplus
extern "C" {
#endif

struct bg_audio_meter;

struct bgfx_source {
	obs_source_t *source;

	uint32_t width;
	uint32_t height;
	float    clock;

	bool     seed_locked;
	uint32_t seed;
	bool     reset_on_show;

	int    active;       /* index into bg_registry()      */
	size_t effect_count;
	void **states;       /* one opaque state per effect   */

	/* Audio reactivity: a meter bound to a chosen source plus the target
	 * settings; `audio` is refilled each frame and handed to effects. */
	struct bg_audio_meter *meter;
	struct bg_audio_mod    audio;
	float    audio_gain;    /* dB                                  */
	float    audio_attack;  /* rise smoothing, seconds             */
	float    audio_release; /* fall smoothing, seconds             */
};

void bgfx_register_source(void);

#ifdef __cplusplus
}
#endif
