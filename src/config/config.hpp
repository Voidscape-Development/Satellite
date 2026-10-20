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
#include <vector>

namespace satellite {

/// Configuration for one of the two frontend senders (Program or Preview).
struct OutputConfig {
	bool enabled = false;
	std::string name;
	Protocol protocol = Protocol::NDI;
	Quality quality = Quality::Default;
	bool send_audio = true;
};

/// Configuration for one sender of an additional OBS canvas (OBS 31.1 and later), such as
/// a vertical canvas added by another plugin.
///
/// The canvas is remembered by UUID, with its name as a fallback: a canvas that is not
/// saved with the scene collection gets a fresh UUID each launch but keeps its name.
struct CanvasSenderConfig {
	/// Stable per-sender id, so editing or removing one sender never restarts another.
	int id = 0;
	bool enabled = false;
	std::string canvas_uuid;
	std::string canvas_name;
	std::string name;
	Protocol protocol = Protocol::NDI;
	Quality quality = Quality::Default;
	/// 0 sends no audio; 1-6 send that OBS mixer track. Canvases have no audio of their
	/// own - OBS mixes all audio globally - so a track is the only meaningful choice.
	int audio_track = 1;
};

/// Global plugin settings, stored in the module config directory rather than per-profile,
/// so a Satellite install behaves the same across every profile.
///
/// Per-source and per-filter settings are not here - those live in the normal OBS settings
/// blob so they travel with the scene collection.
struct Config {
	OutputConfig program;
	OutputConfig preview;
	std::vector<CanvasSenderConfig> canvas_senders;

	/// OMT: written through omt_settings_set_string("DiscoveryServer", ...). Empty means
	/// default DNS-SD behaviour.
	std::string omt_discovery_server;
	int omt_port_start = 6400;
	int omt_port_end = 6600;

	/// NDI: comma-separated receive groups, empty for the default group.
	std::string ndi_groups;

	/// Set once the DistroAV import has been offered, so the user is not asked again.
	bool distroav_import_offered = false;

	/// Satellite window state: the tab last shown, and the feed columns the user has
	/// turned off, as a comma-separated list of column keys.
	int dock_tab = 0;
	std::string feed_hidden_columns;

	/// The id for a new canvas sender: one past the highest in use.
	int next_canvas_sender_id() const;

	static Config &instance();

	void load();
	void save() const;
};

} // namespace satellite
