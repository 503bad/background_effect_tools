#include "bgfx-source.h"
#include "bgfx-effect.h"
#include "bgfx-registry.h"
#include "bgfx-audio.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>

#include <stdio.h>
#include <string.h>

#include <plugin-support.h>

#define DEFAULT_WIDTH  1920
#define DEFAULT_HEIGHT 1080
#define DEFAULT_EFFECT_ID "smoke"

/* Per-instance counter so two sources with an unlocked seed do not animate in
 * lockstep. */
static uint32_t s_instance_salt = 0x51ed270bu;

static const char *bgfx_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("BackgroundEffectSource");
}

/* Per-effect settings group key, e.g. "smoke_group". Used both as the group's
 * settings key and to toggle its visibility from the effect selector. */
static void group_key(const struct bg_effect *e, char *buf, size_t n)
{
	snprintf(buf, n, "%s_group", e->id);
}

/* Mix the host seed so each effect gets a distinct but reproducible stream. */
static uint32_t effect_seed(const struct bgfx_source *s, size_t index)
{
	uint32_t base = s->seed_locked ? s->seed : s->seed ^ s_instance_salt;
	uint32_t x = base + (uint32_t)index * 0x9e3779b9u + 1u;
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	return x ? x : 0x1234567u;
}

static void reset_all(struct bgfx_source *s)
{
	s->clock = 0.0f;
	size_t n;
	const struct bg_effect *const *fx = bg_registry(&n);
	for (size_t i = 0; i < n; ++i) {
		if (fx[i]->reset)
			fx[i]->reset(s->states[i], effect_seed(s, i));
	}
}

static void bgfx_update(void *data, obs_data_t *settings)
{
	struct bgfx_source *s = data;

	uint32_t w = (uint32_t)obs_data_get_int(settings, "width");
	uint32_t h = (uint32_t)obs_data_get_int(settings, "height");
	s->width = w ? w : DEFAULT_WIDTH;
	s->height = h ? h : DEFAULT_HEIGHT;

	bool was_locked = s->seed_locked;
	uint32_t old_seed = s->seed;
	s->seed_locked = obs_data_get_bool(settings, "seed_lock");
	s->seed = (uint32_t)obs_data_get_int(settings, "seed");
	s->reset_on_show = obs_data_get_bool(settings, "reset_on_show");

	/* --- active effect selection --- */
	int idx = bg_registry_index(obs_data_get_string(settings, "effect"));
	s->active = idx >= 0 ? idx : 0;

	/* --- audio reactivity (shared across effects) --- */
	s->audio.enabled = obs_data_get_bool(settings, "audio_enable");
	bg_audio_read(&s->audio, &s->audio_gain, &s->audio_attack,
		      &s->audio_release, settings);
	bg_audio_set_source(s->meter,
			    obs_data_get_string(settings, "audio_source"));

	/* --- per-effect parameters (update all so inactive ones stay in sync) */
	size_t n;
	const struct bg_effect *const *fx = bg_registry(&n);
	for (size_t i = 0; i < n; ++i) {
		if (fx[i]->update)
			fx[i]->update(s->states[i], settings);
	}

	/* Reseed deterministically whenever the lock engages or moves. */
	if (s->seed_locked && (!was_locked || old_seed != s->seed))
		reset_all(s);
}

static void *bgfx_create(obs_data_t *settings, obs_source_t *source)
{
	struct bgfx_source *s = bzalloc(sizeof(*s));
	s->source = source;
	s->clock = 0.0f;
	s->meter = bg_audio_create();

	/* xorshift the salt so each created instance differs. */
	s_instance_salt ^= s_instance_salt << 13;
	s_instance_salt ^= s_instance_salt >> 17;
	s_instance_salt ^= s_instance_salt << 5;

	size_t n;
	const struct bg_effect *const *fx = bg_registry(&n);
	s->effect_count = n;
	s->states = bzalloc(sizeof(void *) * (n ? n : 1));
	for (size_t i = 0; i < n; ++i)
		s->states[i] = fx[i]->create ? fx[i]->create() : NULL;

	obs_enter_graphics();
	for (size_t i = 0; i < n; ++i) {
		if (fx[i]->load_graphics)
			fx[i]->load_graphics(s->states[i]);
	}
	obs_leave_graphics();

	bgfx_update(s, settings);
	reset_all(s);
	return s;
}

static void bgfx_destroy(void *data)
{
	struct bgfx_source *s = data;
	size_t n;
	const struct bg_effect *const *fx = bg_registry(&n);

	obs_enter_graphics();
	for (size_t i = 0; i < n; ++i) {
		if (fx[i]->destroy)
			fx[i]->destroy(s->states[i]);
	}
	obs_leave_graphics();

	bg_audio_destroy(s->meter);
	bfree(s->states);
	bfree(s);
}

static void fill_ctx(const struct bgfx_source *s, struct bg_ctx *ctx)
{
	ctx->time = s->clock;
	ctx->width = s->width ? s->width : 1u;
	ctx->height = s->height ? s->height : 1u;
	ctx->seed = s->seed;
	ctx->seed_locked = s->seed_locked;
	ctx->audio = s->audio;
	ctx->fft = s->audio.enabled ? bg_audio_get_fft(s->meter) : NULL;
}

static void bgfx_video_tick(void *data, float seconds)
{
	struct bgfx_source *s = data;
	s->clock += seconds;

	/* Refresh the smoothed audio level once per frame (0 when disabled). */
	s->audio.level = s->audio.enabled
				 ? bg_audio_tick(s->meter, seconds,
						 s->audio_gain, s->audio_attack,
						 s->audio_release)
				 : 0.0f;

	const struct bg_effect *fx = bg_registry(NULL)[s->active];
	if (fx->tick) {
		struct bg_ctx ctx;
		fill_ctx(s, &ctx);
		fx->tick(s->states[s->active], &ctx, seconds);
	}
}

static void bgfx_show(void *data)
{
	struct bgfx_source *s = data;
	if (s->reset_on_show)
		reset_all(s);
}

static void bgfx_video_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct bgfx_source *s = data;

	const struct bg_effect *fx = bg_registry(NULL)[s->active];
	if (fx->render) {
		struct bg_ctx ctx;
		fill_ctx(s, &ctx);
		fx->render(s->states[s->active], &ctx);
	}
}

static uint32_t bgfx_get_width(void *data)
{
	const struct bgfx_source *s = data;
	return s->width ? s->width : 1u;
}

static uint32_t bgfx_get_height(void *data)
{
	const struct bgfx_source *s = data;
	return s->height ? s->height : 1u;
}

/* Show the seed field only while the lock is on. */
static bool on_seed_lock_changed(void *priv, obs_properties_t *props,
				 obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(priv);
	UNUSED_PARAMETER(prop);
	obs_property_t *seed = obs_properties_get(props, "seed");
	if (seed)
		obs_property_set_visible(seed,
			obs_data_get_bool(settings, "seed_lock"));
	return true;
}

/* Reveal the audio settings group only while audio reactivity is enabled. */
static bool on_audio_enable_changed(void *priv, obs_properties_t *props,
				    obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(priv);
	UNUSED_PARAMETER(prop);
	obs_property_t *g = obs_properties_get(props, "audio_group");
	if (g)
		obs_property_set_visible(g,
			obs_data_get_bool(settings, "audio_enable"));
	return true;
}

/* Show only the active effect's property group. */
static bool on_effect_changed(void *priv, obs_properties_t *props,
			      obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(priv);
	UNUSED_PARAMETER(prop);
	const char *id = obs_data_get_string(settings, "effect");

	size_t n;
	const struct bg_effect *const *fx = bg_registry(&n);
	for (size_t i = 0; i < n; ++i) {
		char key[96];
		group_key(fx[i], key, sizeof(key));
		obs_property_t *g = obs_properties_get(props, key);
		if (g)
			obs_property_set_visible(g, strcmp(id, fx[i]->id) == 0);
	}
	return true; /* property visibility changed → refresh the UI */
}

static obs_properties_t *bgfx_properties(void *data)
{
	struct bgfx_source *s = data;
	obs_properties_t *p = obs_properties_create();

	/* Effect selector first so the chosen look leads the panel. */
	obs_property_t *sel = obs_properties_add_list(p, "effect",
		obs_module_text("Effect"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);

	size_t n;
	const struct bg_effect *const *fx = bg_registry(&n);
	for (size_t i = 0; i < n; ++i)
		obs_property_list_add_string(sel,
			obs_module_text(fx[i]->name_key), fx[i]->id);
	obs_property_set_modified_callback2(sel, on_effect_changed, NULL);

	/* Shared canvas size. */
	obs_properties_add_int(p, "width", obs_module_text("Width"), 64, 7680,
			       2);
	obs_properties_add_int(p, "height", obs_module_text("Height"), 64,
			       4320, 2);

	/* One group per effect; only the active one is shown. Hand each effect
	 * the live settings so it can hide parameters its current mode ignores
	 * and register its own modified callbacks. */
	obs_data_t *settings = s ? obs_source_get_settings(s->source) : NULL;
	for (size_t i = 0; i < n; ++i) {
		obs_properties_t *grp = obs_properties_create();
		if (fx[i]->get_properties)
			fx[i]->get_properties(grp, settings);
		char key[96];
		group_key(fx[i], key, sizeof(key));
		obs_properties_add_group(p, key,
			obs_module_text(fx[i]->name_key), OBS_GROUP_NORMAL, grp);
	}
	obs_data_release(settings);

	/* Audio reactivity: a master toggle reveals the analysis settings
	 * (source / gain / attack / release) plus the size/colour/bounce
	 * targets. The analysis also feeds the spectrum visualizer effect. */
	obs_property_t *aen = obs_properties_add_bool(p, "audio_enable",
		obs_module_text("AudioReactive"));
	obs_property_set_modified_callback2(aen, on_audio_enable_changed, NULL);
	obs_properties_t *ag = obs_properties_create();
	bg_audio_props(ag);
	obs_properties_add_group(p, "audio_group",
		obs_module_text("AudioSettings"), OBS_GROUP_NORMAL, ag);
	{
		obs_property_t *g = obs_properties_get(p, "audio_group");
		if (g)
			obs_property_set_visible(g,
				s ? s->audio.enabled : false);
	}

	/* Shared behaviour switches, after the effect parameters. */
	obs_properties_add_bool(p, "reset_on_show",
		obs_module_text("ResetOnShow"));
	obs_property_t *lock = obs_properties_add_bool(p, "seed_lock",
		obs_module_text("SeedLock"));
	obs_property_set_modified_callback2(lock, on_seed_lock_changed, NULL);
	obs_property_t *seed = obs_properties_add_int(p, "seed",
		obs_module_text("Seed"), 0, 0x7FFFFFFF, 1);
	obs_property_set_visible(seed, s ? s->seed_locked : false);

	/* Initial visibility based on the currently active effect. */
	int active = s ? s->active : 0;
	for (size_t i = 0; i < n; ++i) {
		char key[96];
		group_key(fx[i], key, sizeof(key));
		obs_property_t *g = obs_properties_get(p, key);
		if (g)
			obs_property_set_visible(g, (int)i == active);
	}

	return p;
}

static void bgfx_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "effect", DEFAULT_EFFECT_ID);
	obs_data_set_default_int(settings, "width", DEFAULT_WIDTH);
	obs_data_set_default_int(settings, "height", DEFAULT_HEIGHT);
	obs_data_set_default_bool(settings, "reset_on_show", false);
	obs_data_set_default_bool(settings, "seed_lock", false);
	obs_data_set_default_int(settings, "seed", 1234);
	obs_data_set_default_bool(settings, "audio_enable", false);
	bg_audio_defaults(settings);

	size_t n;
	const struct bg_effect *const *fx = bg_registry(&n);
	for (size_t i = 0; i < n; ++i) {
		/* Keep each effect's property group enabled by default. */
		char key[96];
		group_key(fx[i], key, sizeof(key));
		obs_data_set_default_bool(settings, key, true);
		if (fx[i]->get_defaults)
			fx[i]->get_defaults(settings);
	}
}

static struct obs_source_info s_bgfx_source_info = {
	.id             = "background_effect_source",
	.type           = OBS_SOURCE_TYPE_INPUT,
	.output_flags   = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.icon_type      = OBS_ICON_TYPE_COLOR,
	.get_name       = bgfx_get_name,
	.create         = bgfx_create,
	.destroy        = bgfx_destroy,
	.update         = bgfx_update,
	.video_tick     = bgfx_video_tick,
	.video_render   = bgfx_video_render,
	.get_width      = bgfx_get_width,
	.get_height     = bgfx_get_height,
	.get_properties = bgfx_properties,
	.get_defaults   = bgfx_defaults,
	.show           = bgfx_show,
};

void bgfx_register_source(void)
{
	obs_register_source(&s_bgfx_source_info);
}
