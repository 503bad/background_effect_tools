#include "bgfx-props.h"

#include <math.h>

#define BG_TAU 6.28318530718f

void bg_unpack_color(uint32_t c, float rgba[4])
{
	rgba[0] = (float)((c >> 0) & 0xFF) / 255.0f;
	rgba[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	rgba[2] = (float)((c >> 16) & 0xFF) / 255.0f;
	rgba[3] = (float)((c >> 24) & 0xFF) / 255.0f;
}

float bg_fft_band(const struct bg_audio_fft *fft, float lo_hz, float hi_hz)
{
	if (!fft || !fft->valid || fft->bar_count <= 0)
		return 0.0f;
	float fmin = fft->freq_min, fmax = fft->freq_max;
	if (fmin <= 0.0f || fmax <= fmin)
		return 0.0f;
	if (hi_hz < lo_hz) {
		float t = lo_hz;
		lo_hz = hi_hz;
		hi_hz = t;
	}
	int N = fft->bar_count;
	float lr = logf(fmax / fmin);
	/* Map each edge to a fractional bar index, then clamp into range. */
	float bf_lo = logf((lo_hz < fmin ? fmin : lo_hz) / fmin) / lr * N;
	float bf_hi = logf((hi_hz > fmax ? fmax : hi_hz) / fmin) / lr * N;
	int b0 = (int)bf_lo;
	int b1 = (int)bf_hi;
	if (b0 < 0)
		b0 = 0;
	if (b1 >= N)
		b1 = N - 1;
	if (b1 < b0)
		b1 = b0;
	float sum = 0.0f;
	for (int b = b0; b <= b1; ++b)
		sum += fft->bars[b];
	return sum / (float)(b1 - b0 + 1);
}

/* ---- common particle parameters ------------------------------------------ */

void bg_common_props(obs_properties_t *g, const char *pre,
		     const struct bg_common_spec *spec)
{
	char k[96];
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), pre, "size"),
		obs_module_text("Size"), spec->size_min, spec->size_max,
		spec->size_step);
	obs_properties_add_int_slider(g, bg_key(k, sizeof(k), pre, "size_var"),
		obs_module_text("SizeVar"), 0,
		spec->size_var_max > 0 ? spec->size_var_max : 100, 1);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), pre, "alpha"),
		obs_module_text("Alpha"), 0.0, 1.0, 0.01);
	obs_properties_add_color(g, bg_key(k, sizeof(k), pre, "color"),
		obs_module_text("Color"));
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), pre, "lifetime"),
		obs_module_text("Lifetime"), spec->life_min, spec->life_max,
		0.05);
	obs_properties_add_int_slider(g, bg_key(k, sizeof(k), pre, "life_var"),
		obs_module_text("LifeVar"), 0, 100, 1);
	/* Burst-driven effects (rate_max 0) have no continuous spawn rate. */
	if (spec->rate_max > 0.0)
		obs_properties_add_float_slider(g,
			bg_key(k, sizeof(k), pre, "rate"),
			obs_module_text("EmitRate"), 0.0, spec->rate_max, 1.0);
	obs_properties_add_int_slider(g, bg_key(k, sizeof(k), pre, "max"),
		obs_module_text("MaxParticles"), 16, spec->max_cap, 16);
}

void bg_common_defaults(obs_data_t *s, const char *pre,
			const struct bg_common_spec *spec)
{
	char k[96];
	obs_data_set_default_double(s, bg_key(k, sizeof(k), pre, "size"),
				    spec->size_def);
	obs_data_set_default_int(s, bg_key(k, sizeof(k), pre, "size_var"), 40);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), pre, "alpha"),
				    spec->alpha_def);
	obs_data_set_default_int(s, bg_key(k, sizeof(k), pre, "color"),
				 spec->color_def);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), pre, "lifetime"),
				    spec->life_def);
	obs_data_set_default_int(s, bg_key(k, sizeof(k), pre, "life_var"), 30);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), pre, "rate"),
				    spec->rate_def);
	obs_data_set_default_int(s, bg_key(k, sizeof(k), pre, "max"),
				 spec->max_def);
}

void bg_common_update(struct bg_common *c, obs_data_t *s, const char *pre)
{
	char k[96];
	c->size = (float)obs_data_get_double(s, bg_key(k, sizeof(k), pre, "size"));
	c->size_var = (float)obs_data_get_int(s,
		bg_key(k, sizeof(k), pre, "size_var")) / 100.0f;
	c->alpha = (float)obs_data_get_double(s,
		bg_key(k, sizeof(k), pre, "alpha"));
	c->color = (uint32_t)obs_data_get_int(s,
		bg_key(k, sizeof(k), pre, "color"));
	c->lifetime = (float)obs_data_get_double(s,
		bg_key(k, sizeof(k), pre, "lifetime"));
	c->life_var = (float)obs_data_get_int(s,
		bg_key(k, sizeof(k), pre, "life_var")) / 100.0f;
	c->rate = (float)obs_data_get_double(s,
		bg_key(k, sizeof(k), pre, "rate"));
	c->max_count = (int)obs_data_get_int(s, bg_key(k, sizeof(k), pre, "max"));
}

/* ---- flow direction --------------------------------------------------------- */

/* The fan-spread slider only matters for the two fanning flows. */
static bool flow_uses_fan(int flow)
{
	return flow == BG_FLOW_UP_FAN || flow == BG_FLOW_UP_CURVE;
}

static bool on_flow_changed(void *priv, obs_properties_t *props,
			    obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);
	const char *pre = priv;
	char k[96];
	int flow = (int)obs_data_get_int(settings, bg_key(k, sizeof(k), pre,
							  "flow"));
	obs_property_t *fan = obs_properties_get(props,
		bg_key(k, sizeof(k), pre, "fan"));
	if (fan)
		obs_property_set_visible(fan, flow_uses_fan(flow));
	return true;
}

void bg_flow_props(obs_properties_t *g, const char *pre, obs_data_t *settings)
{
	char k[96];
	obs_property_t *flow = obs_properties_add_list(g,
		bg_key(k, sizeof(k), pre, "flow"),
		obs_module_text("FlowDir"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(flow, obs_module_text("FlowUp"), BG_FLOW_UP);
	obs_property_list_add_int(flow, obs_module_text("FlowDown"),
				  BG_FLOW_DOWN);
	obs_property_list_add_int(flow, obs_module_text("FlowLeft"),
				  BG_FLOW_LEFT);
	obs_property_list_add_int(flow, obs_module_text("FlowRight"),
				  BG_FLOW_RIGHT);
	obs_property_list_add_int(flow, obs_module_text("FlowCenterOut"),
				  BG_FLOW_LR);
	obs_property_list_add_int(flow, obs_module_text("FlowUpFan"),
				  BG_FLOW_UP_FAN);
	obs_property_list_add_int(flow, obs_module_text("FlowUpCurve"),
				  BG_FLOW_UP_CURVE);
	obs_property_set_modified_callback2(flow, on_flow_changed, (void *)pre);

	/* Fan spread; only the fanning flows read it. Up to 1 it widens the
	 * launch cone; beyond 1 it keeps strengthening the outward push. */
	obs_property_t *fan = obs_properties_add_float_slider(g,
		bg_key(k, sizeof(k), pre, "fan"), obs_module_text("FanSpread"),
		0.0, 5.0, 0.05);

	/* Initial visibility from the current (or default) flow. */
	int cur = settings ? (int)obs_data_get_int(settings,
			bg_key(k, sizeof(k), pre, "flow")) : BG_FLOW_UP;
	obs_property_set_visible(fan, flow_uses_fan(cur));
}

void bg_flow_defaults(obs_data_t *s, const char *pre)
{
	char k[96];
	obs_data_set_default_int(s, bg_key(k, sizeof(k), pre, "flow"),
				 BG_FLOW_UP);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), pre, "fan"), 0.5);
}

float bg_flow_fan_update(obs_data_t *s, const char *pre)
{
	char k[96];
	return (float)obs_data_get_double(s, bg_key(k, sizeof(k), pre, "fan"));
}

int bg_flow_update(obs_data_t *s, const char *pre)
{
	char k[96];
	return (int)obs_data_get_int(s, bg_key(k, sizeof(k), pre, "flow"));
}

/* ---- wind ----------------------------------------------------------------- */

void bg_wind_props(obs_properties_t *g, const char *pre)
{
	char k[96];
	obs_properties_add_bool(g, bg_key(k, sizeof(k), pre, "wind_on"),
		obs_module_text("WindOn"));
	obs_properties_add_float_slider(g,
		bg_key(k, sizeof(k), pre, "wind_strength"),
		obs_module_text("WindStrength"), 0.0, 600.0, 1.0);
	obs_properties_add_float_slider(g,
		bg_key(k, sizeof(k), pre, "wind_dir"),
		obs_module_text("WindDir"), 0.0, 360.0, 1.0);
	obs_properties_add_float_slider(g,
		bg_key(k, sizeof(k), pre, "wind_turb"),
		obs_module_text("WindTurb"), 0.0, 1.0, 0.01);
}

void bg_wind_defaults(obs_data_t *s, const char *pre)
{
	char k[96];
	obs_data_set_default_bool(s, bg_key(k, sizeof(k), pre, "wind_on"),
				  false);
	obs_data_set_default_double(s,
		bg_key(k, sizeof(k), pre, "wind_strength"), 80.0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), pre, "wind_dir"),
				    0.0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), pre, "wind_turb"),
				    0.3);
}

void bg_wind_update(struct bg_wind *w, obs_data_t *s, const char *pre)
{
	char k[96];
	w->on = obs_data_get_bool(s, bg_key(k, sizeof(k), pre, "wind_on"));
	w->strength = (float)obs_data_get_double(s,
		bg_key(k, sizeof(k), pre, "wind_strength"));
	w->dir_deg = (float)obs_data_get_double(s,
		bg_key(k, sizeof(k), pre, "wind_dir"));
	w->turb = (float)obs_data_get_double(s,
		bg_key(k, sizeof(k), pre, "wind_turb"));
}

void bg_wind_accel(const struct bg_wind *w, float t, float seed, float *ax,
		   float *ay)
{
	*ax = 0.0f;
	*ay = 0.0f;
	if (!w || !w->on)
		return;

	/* Steady push. 0° points right; 90° points up (screen y grows down). */
	float rad = w->dir_deg * (BG_TAU / 360.0f);
	*ax = cosf(rad) * w->strength;
	*ay = -sinf(rad) * w->strength;

	/* Per-particle pseudo-noise scatter. */
	if (w->turb > 0.0f) {
		float amp = w->turb * 180.0f;
		float p1 = t * 1.7f + seed * 31.4f;
		float p2 = t * 1.3f + seed * 23.0f;
		*ax += amp * (sinf(p1) + 0.5f * sinf(p1 * 2.3f + 1.0f));
		*ay += amp * (cosf(p2) + 0.5f * sinf(p2 * 2.9f + 2.0f)) * 0.6f;
	}
}

/* ---- post effects ---------------------------------------------------------- */

void bg_post_props(obs_properties_t *g, const char *pre)
{
	char k[96];
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), pre, "glow"),
		obs_module_text("Glow"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), pre, "bloom"),
		obs_module_text("Bloom"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), pre, "emissive"),
		obs_module_text("Emissive"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), pre, "flare"),
		obs_module_text("LensFlare"), 0.0, 1.0, 0.01);
}

void bg_post_defaults(obs_data_t *s, const char *pre, double glow,
		      double bloom, double emissive, double flare)
{
	char k[96];
	obs_data_set_default_double(s, bg_key(k, sizeof(k), pre, "glow"), glow);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), pre, "bloom"),
				    bloom);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), pre, "emissive"),
				    emissive);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), pre, "flare"),
				    flare);
}

void bg_post_update(struct bg_post *p, obs_data_t *s, const char *pre)
{
	char k[96];
	p->glow = (float)obs_data_get_double(s, bg_key(k, sizeof(k), pre, "glow"));
	p->bloom = (float)obs_data_get_double(s,
		bg_key(k, sizeof(k), pre, "bloom"));
	p->emissive = (float)obs_data_get_double(s,
		bg_key(k, sizeof(k), pre, "emissive"));
	p->flare = (float)obs_data_get_double(s,
		bg_key(k, sizeof(k), pre, "flare"));
}
