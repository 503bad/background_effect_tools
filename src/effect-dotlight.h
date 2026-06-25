#pragma once

#include "bgfx-effect.h"

extern const struct bg_effect bgfx_dotlight;

/* Shared full-canvas dot-grid draw + property helpers. */

struct gs_effect;
typedef struct gs_effect gs_effect_t;

/* Set every dotlight.effect uniform and draw the full-canvas grid. `flash`
 * lifts all dots (beat flash); `phase` is the travelling-wave offset. */
void bg_dotlight_draw(gs_effect_t *e, const struct bg_ctx *ctx, uint32_t color_a,
		      uint32_t color_b, int color_mode, int axis, float spacing,
		      float dotsize, float cycles, float phase, float sharp,
		      float flash, float floor_b, float glow, float bloom,
		      float blur, float flare, float opacity);

/* The shared visual property block (colour/size/glow/etc.), namespaced under
 * `pre` so each effect keeps its own settings. */
void bg_dotlight_visual_props(obs_properties_t *g, const char *pre,
			      obs_data_t *settings);
void bg_dotlight_visual_defaults(obs_data_t *s, const char *pre);
