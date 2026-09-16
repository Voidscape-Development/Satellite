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

	const char *discovery_server = obs_data_get_string(data, "omt_discovery_server");
	omt_discovery_server = discovery_server ? discovery_server : "";

	if (obs_data_has_user_value(data, "omt_port_start"))
		omt_port_start = static_cast<int>(obs_data_get_int(data, "omt_port_start"));
	if (obs_data_has_user_value(data, "omt_port_end"))
		omt_port_end = static_cast<int>(obs_data_get_int(data, "omt_port_end"));

	const char *groups = obs_data_get_string(data, "ndi_groups");
	ndi_groups = groups ? groups : "";

	distroav_import_offered = obs_data_get_bool(data, "distroav_import_offered");

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

	obs_data_set_string(data, "omt_discovery_server", omt_discovery_server.c_str());
	obs_data_set_int(data, "omt_port_start", omt_port_start);
	obs_data_set_int(data, "omt_port_end", omt_port_end);
	obs_data_set_string(data, "ndi_groups", ndi_groups.c_str());
	obs_data_set_bool(data, "distroav_import_offered", distroav_import_offered);

	if (!obs_data_save_json_safe(data, path.c_str(), "tmp", "bak"))
		obs_log(LOG_WARNING, "could not write configuration to %s", path.c_str());

	obs_data_release(data);
}

} // namespace satellite
