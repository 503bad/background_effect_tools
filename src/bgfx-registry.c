#include "bgfx-registry.h"
#include "bgfx-effect.h"

#include <string.h>

/* Each effect is declared in its own translation unit. */
extern const struct bg_effect bgfx_smoke;
extern const struct bg_effect bgfx_embers;
extern const struct bg_effect bgfx_clouds;
extern const struct bg_effect bgfx_sparkles;
extern const struct bg_effect bgfx_bubble;
extern const struct bg_effect bgfx_gravity;
extern const struct bg_effect bgfx_vortex;
extern const struct bg_effect bgfx_voice;
extern const struct bg_effect bgfx_spectrum;
extern const struct bg_effect bgfx_audioviz;
extern const struct bg_effect bgfx_geometric;
extern const struct bg_effect bgfx_snow;
extern const struct bg_effect bgfx_magiccircle;
extern const struct bg_effect bgfx_radial;
extern const struct bg_effect bgfx_imgparticle;
extern const struct bg_effect bgfx_dotlight;
extern const struct bg_effect bgfx_beatspot;
extern const struct bg_effect bgfx_textparticle;
extern const struct bg_effect bgfx_flame;

static const struct bg_effect *const k_effects[] = {
	&bgfx_smoke,
	&bgfx_embers,
	&bgfx_snow,
	&bgfx_clouds,
	&bgfx_sparkles,
	&bgfx_bubble,
	&bgfx_gravity,
	&bgfx_vortex,
	&bgfx_voice,
	&bgfx_spectrum,
	&bgfx_audioviz,
	&bgfx_geometric,
	&bgfx_magiccircle,
	&bgfx_radial,
	&bgfx_imgparticle,
	&bgfx_dotlight,
	&bgfx_beatspot,
	&bgfx_textparticle,
	&bgfx_flame,
};

const struct bg_effect *const *bg_registry(size_t *count)
{
	if (count)
		*count = sizeof(k_effects) / sizeof(k_effects[0]);
	return k_effects;
}

int bg_registry_index(const char *id)
{
	if (!id)
		return -1;
	size_t n;
	const struct bg_effect *const *e = bg_registry(&n);
	for (size_t i = 0; i < n; ++i) {
		if (strcmp(e[i]->id, id) == 0)
			return (int)i;
	}
	return -1;
}
