/*
Satellite - NDI and OMT transport for OBS Studio
Copyright (C) 2026 Voidscape Development

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

#include "config/config.hpp"
#include "discovery/discovery-service.hpp"
#include "metrics/feed-registry.hpp"
#include "obs/satellite-filter.hpp"
#include "obs/satellite-output.hpp"
#include "obs/satellite-source.hpp"
#include "transport/transport.hpp"
#include "ui/import-dialog.hpp"
#include "ui/satellite-dock.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

void on_frontend_event(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		// The dock needs the main window to exist, so it is created here rather than in
		// obs_module_load().
		satellite::register_satellite_dock();
		satellite::update_frontend_outputs();

		// Offered once, and only when there is actually a DistroAV setup to convert. The
		// scene collection has to be loaded for the scan to see anything, which is why it
		// happens here rather than in obs_module_load.
		satellite::maybe_offer_distroav_import(nullptr);
		break;

	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
		// The Preview sender renders through a view of its own, so it has to be pointed
		// at the new scene. Outbound tally needs nothing here: sources track their own
		// program/preview state through OBS's activate/show callbacks.
		satellite::update_frontend_outputs();
		break;

	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
		// The Preview sender only exists while Studio Mode is on.
		satellite::update_frontend_outputs();
		break;

	case OBS_FRONTEND_EVENT_EXIT:
		satellite::shutdown_frontend_outputs();
		satellite::unregister_satellite_dock();
		satellite::DiscoveryService::instance().stop();
		break;

	default:
		break;
	}
}

} // namespace

bool obs_module_load(void)
{
	satellite::Config::instance().load();

	// Backends first: everything else asks them whether their protocol is usable.
	satellite::register_backends();

	satellite::register_satellite_source();
	satellite::register_satellite_filter();
	satellite::register_satellite_output();

	satellite::DiscoveryService::instance().start();

	obs_frontend_add_event_callback(on_frontend_event, nullptr);

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);

	satellite::shutdown_frontend_outputs();
	satellite::DiscoveryService::instance().stop();
	satellite::FeedRegistry::instance().clear();
	satellite::unregister_backends();

	satellite::Config::instance().save();

	obs_log(LOG_INFO, "plugin unloaded");
}
