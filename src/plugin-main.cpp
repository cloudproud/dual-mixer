/*
Dual Mixer
Copyright (C) 2026 Jeroen Rinzema <jeroen@rinzema.dev>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "common.hpp"
#include "manager.hpp"
#include "ui/workspace-dock.hpp"

#include <plugin-support.h>

#include <obs-frontend-api.h>
#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Plugin.Description");
}

bool obs_module_load(void)
{
	dm::Manager::create();
	dm::Manager *manager = dm::Manager::instance();

	/* The dock is registered during module load so OBS restores its saved layout. */
	auto *workspace = new dm::WorkspaceDock(manager);
	if (!obs_frontend_add_dock_by_id("dual_mixer_workspace", obs_module_text("Dock.Workspace"), workspace))
		delete workspace;

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	dm::Manager::destroy();
	obs_log(LOG_INFO, "plugin unloaded");
}
