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

#include <algorithm>

#include <obs-module.h>
#include <util/config-file.h>
#include <util/platform.h>
#include <plugin-support.h>

namespace satellite {

namespace {

constexpr const char *kConfigFile = "satellite.json";

std::string config_path()
{
	char *path = obs_module_config_path(kConfigFile);
	if (!path)
		return {};

	std::string result(path);
	bfree(path);
	return result;
}

void load_output(obs_data_t *parent, const char *key, OutputConfig &output, const char *default_suffix)
{
	obs_data_t *data = obs_data_get_obj(parent, key);
	if (!data) {
		output.name = std::string("OBS ") + default_suffix;
		return;
	}

	output.enabled = obs_data_get_bool(data, "enabled");

	const char *name = obs_data_get_string(data, "name");
	output.name = (name && *name) ? name : (std::string("OBS ") + default_suffix);

	Protocol protocol = Protocol::NDI;
	if (protocol_from_id(obs_data_get_string(data, "protocol"), protocol))
		output.protocol = protocol;

	Quality quality = Quality::Default;
	if (quality_from_id(obs_data_get_string(data, "quality"), quality))
		output.quality = quality;

	output.send_audio = obs_data_get_bool(data, "send_audio");

	obs_data_release(data);
}

void save_output(obs_data_t *parent, const char *key, const OutputConfig &output)
{
	obs_data_t *data = obs_data_create();

	obs_data_set_bool(data, "enabled", output.enabled);
	obs_data_set_string(data, "name", output.name.c_str());
	obs_data_set_string(data, "protocol", protocol_id(output.protocol));
	obs_data_set_string(data, "quality", quality_id(output.quality));
	obs_data_set_bool(data, "send_audio", output.send_audio);

	obs_data_set_obj(parent, key, data);
	obs_data_release(data);
}

void load_canvas_senders(obs_data_t *parent, std::vector<CanvasSenderConfig> &senders)
{
	senders.clear();

	obs_data_array_t *array = obs_data_get_array(parent, "canvas_senders");
	if (!array)
		return;

	const size_t count = obs_data_array_count(array);
	for (size_t index = 0; index < count; ++index) {
		obs_data_t *data = obs_data_array_item(array, index);

		CanvasSenderConfig sender;
		sender.id = static_cast<int>(obs_data_get_int(data, "id"));
		sender.enabled = obs_data_get_bool(data, "enabled");

		const char *uuid = obs_data_get_string(data, "canvas_uuid");
		sender.canvas_uuid = uuid ? uuid : "";
		const char *canvas_name = obs_data_get_string(data, "canvas_name");
		sender.canvas_name = canvas_name ? canvas_name : "";
		const char *name = obs_data_get_string(data, "name");
		sender.name = name ? name : "";

		Protocol protocol = Protocol::NDI;
		if (protocol_from_id(obs_data_get_string(data, "protocol"), protocol))
			sender.protocol = protocol;

		Quality quality = Quality::Default;
		if (quality_from_id(obs_data_get_string(data, "quality"), quality))
			sender.quality = quality;

		sender.audio_track = static_cast<int>(obs_data_get_int(data, "audio_track"));
		if (sender.audio_track < 0 || sender.audio_track > MAX_AUDIO_MIXES)
			sender.audio_track = 0;

		obs_data_release(data);

		// Ids are what keeps a sender's identity across edits, so a hand-edited file with
		// a missing or repeated one gets a fresh id rather than two senders sharing one.
		bool duplicate = sender.id <= 0;
		for (const CanvasSenderConfig &existing : senders)
			duplicate = duplicate || existing.id == sender.id;
		if (duplicate) {
			int next = 1;
			for (const CanvasSenderConfig &existing : senders)
				next = std::max(next, existing.id + 1);
			sender.id = next;
		}

		senders.push_back(std::move(sender));
	}

	obs_data_array_release(array);
}

void save_canvas_senders(obs_data_t *parent, const std::vector<CanvasSenderConfig> &senders)
{
	obs_data_array_t *array = obs_data_array_create();

	for (const CanvasSenderConfig &sender : senders) {
		obs_data_t *data = obs_data_create();

		obs_data_set_int(data, "id", sender.id);
		obs_data_set_bool(data, "enabled", sender.enabled);
		obs_data_set_string(data, "canvas_uuid", sender.canvas_uuid.c_str());
		obs_data_set_string(data, "canvas_name", sender.canvas_name.c_str());
		obs_data_set_string(data, "name", sender.name.c_str());
		obs_data_set_string(data, "protocol", protocol_id(sender.protocol));
		obs_data_set_string(data, "quality", quality_id(sender.quality));
		obs_data_set_int(data, "audio_track", sender.audio_track);

		obs_data_array_push_back(array, data);
		obs_data_release(data);
	}

	obs_data_set_array(parent, "canvas_senders", array);
	obs_data_array_release(array);
}

} // namespace

Config &Config::instance()
{
	static Config config;
	return config;
}

void Config::load()
{
	const std::string path = config_path();
	if (path.empty())
		return;

	obs_data_t *data = obs_data_create_from_json_file(path.c_str());
	if (!data) {
		// No config yet: first run. Defaults, plus sensible sender names.
		program.name = "OBS Program";
		preview.name = "OBS Preview";
		obs_log(LOG_INFO, "no saved configuration, using defaults");
		return;
	}

	load_output(data, "program", program, "Program");
	load_output(data, "preview", preview, "Preview");
	load_canvas_senders(data, canvas_senders);

	const char *discovery_server = obs_data_get_string(data, "omt_discovery_server");
	omt_discovery_server = discovery_server ? discovery_server : "";

	if (obs_data_has_user_value(data, "omt_port_start"))
		omt_port_start = static_cast<int>(obs_data_get_int(data, "omt_port_start"));
	if (obs_data_has_user_value(data, "omt_port_end"))
		omt_port_end = static_cast<int>(obs_data_get_int(data, "omt_port_end"));

	const char *groups = obs_data_get_string(data, "ndi_groups");
	ndi_groups = groups ? groups : "";

	distroav_import_offered = obs_data_get_bool(data, "distroav_import_offered");

	dock_tab = static_cast<int>(obs_data_get_int(data, "dock_tab"));
	const char *hidden_columns = obs_data_get_string(data, "feed_hidden_columns");
	feed_hidden_columns = hidden_columns ? hidden_columns : "";

	obs_data_release(data);
}

void Config::save() const
{
	char *directory = obs_module_config_path(nullptr);
	if (directory) {
		os_mkdirs(directory);
		bfree(directory);
	}

	const std::string path = config_path();
	if (path.empty())
		return;

	obs_data_t *data = obs_data_create();

	save_output(data, "program", program);
	save_output(data, "preview", preview);
	save_canvas_senders(data, canvas_senders);

	obs_data_set_string(data, "omt_discovery_server", omt_discovery_server.c_str());
	obs_data_set_int(data, "omt_port_start", omt_port_start);
	obs_data_set_int(data, "omt_port_end", omt_port_end);
	obs_data_set_string(data, "ndi_groups", ndi_groups.c_str());
	obs_data_set_bool(data, "distroav_import_offered", distroav_import_offered);
	obs_data_set_int(data, "dock_tab", dock_tab);
	obs_data_set_string(data, "feed_hidden_columns", feed_hidden_columns.c_str());

	if (!obs_data_save_json_safe(data, path.c_str(), "tmp", "bak"))
		obs_log(LOG_WARNING, "could not write configuration to %s", path.c_str());

	obs_data_release(data);
}

int Config::next_canvas_sender_id() const
{
	int next = 1;
	for (const CanvasSenderConfig &sender : canvas_senders)
		next = std::max(next, sender.id + 1);
	return next;
}

} // namespace satellite
