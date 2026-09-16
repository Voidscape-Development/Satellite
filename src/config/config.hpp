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

#pragma once

#include "transport/transport.hpp"

#include <string>

namespace satellite {

/// Configuration for one of the two frontend senders (Program or Preview).
struct OutputConfig {
	bool enabled = false;
	std::string name;
	Protocol protocol = Protocol::NDI;
	Quality quality = Quality::Default;
	bool send_audio = true;
};

/// Global plugin settings, stored in the module config directory rather than per-profile,
/// so a Satellite install behaves the same across every profile.
///
/// Per-source and per-filter settings are not here - those live in the normal OBS settings
/// blob so they travel with the scene collection.
struct Config {
	OutputConfig program;
	OutputConfig preview;

	/// OMT: written through omt_settings_set_string("DiscoveryServer", ...). Empty means
	/// default DNS-SD behaviour.
	std::string omt_discovery_server;
	int omt_port_start = 6400;
	int omt_port_end = 6600;

	/// NDI: comma-separated receive groups, empty for the default group.
	std::string ndi_groups;

	/// Set once the DistroAV import has been offered, so the user is not asked again.
	bool distroav_import_offered = false;

	static Config &instance();

	void load();
	void save() const;
};

} // namespace satellite
