#pragma once

#include "bgfx-effect.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Audio-reactive particle visualizer with six presets and a built-in Seed
 * generator so every preset keeps animating with no audio source / silence. */
extern const struct bg_effect bgfx_audioviz;

#ifdef __cplusplus
}
#endif
