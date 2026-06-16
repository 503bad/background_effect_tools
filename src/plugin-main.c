#include <obs-module.h>
#include <plugin-support.h>
#include "bgfx-source.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

bool obs_module_load(void)
{
	bgfx_register_source();
	obs_log(LOG_INFO, "background-effect-tools loaded (version %s)",
		PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "background-effect-tools unloaded");
}
