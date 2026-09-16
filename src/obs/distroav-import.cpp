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

#include "obs/distroav-import.hpp"

#include "config/config.hpp"
#include "obs/obs-compat.hpp"
#include "transport/transport.hpp"

#include <cstring>

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/config-file.h>

namespace satellite {

// PROP_BW_* in DistroAV's ndi-source.cpp.
constexpr int kDistroBwHighest = 0;
constexpr int kDistroBwLowest = 1;
constexpr int kDistroBwAudioOnly = 2;

namespace {

// DistroAV's identifiers and settings keys, read from its source rather than guessed.
constexpr const char *kDistroSourceId = "ndi_source";
constexpr const char *kDistroFilterId = "ndi_filter";
constexpr const char *kDistroAudioFilterId = "ndi_audiofilter";

constexpr const char *kDistroSourceName = "ndi_source_name";
constexpr const char *kDistroBandwidth = "ndi_bw_mode";
constexpr const char *kDistroAudio = "ndi_audio";
constexpr const char *kDistroFilterName = "ndi_filter_ndiname";

// DistroAV keeps its output settings in OBS's global config, not in a file of its own.
constexpr const char *kDistroConfigSection = "NDIPlugin";
constexpr const char *kDistroMainEnabled = "MainOutputEnabled";
constexpr const char *kDistroMainName = "MainOutputName";
constexpr const char *kDistroPreviewEnabled = "PreviewOutputEnabled";
constexpr const char *kDistroPreviewName = "PreviewOutputName";

struct ScanState {
	std::vector<ImportItem> *items = nullptr;
};

void scan_filters(obs_source_t *parent, ScanState *state)
{
	obs_source_enum_filters(
		parent,
		[](obs_source_t *source, obs_source_t *filter, void *param) {
			auto *inner = static_cast<ScanState *>(param);

			const char *id = obs_source_get_id(filter);
			if (!id)
				return;

			const bool is_video_filter = strcmp(id, kDistroFilterId) == 0;
			const bool is_audio_filter = strcmp(id, kDistroAudioFilterId) == 0;
			if (!is_video_filter && !is_audio_filter)
				return;

			obs_data_t *settings = obs_source_get_settings(filter);

			ImportItem item;
			item.kind = ImportItem::Kind::Filter;
			item.source_id = id;
			item.owner = obs_source_get_name(source) ? obs_source_get_name(source) : "";
			item.filter_name = obs_source_get_name(filter) ? obs_source_get_name(filter) : "";

			const char *name = obs_data_get_string(settings, kDistroFilterName);
			item.network_name = name ? name : item.owner;
			item.quality_id = quality_id(Quality::Default);
			item.audio = true;

			item.label = item.owner + " → " + item.filter_name;
			item.detail = "Satellite Sender publishing '" + item.network_name + "' over NDI";

			if (is_audio_filter) {
				// Satellite has one sender filter, which carries video and audio.
				// Converting an audio-only filter therefore adds video to that feed,
				// which is a real change in what goes on the network.
				item.caveat = "DistroAV's audio-only filter has no exact equivalent; "
					      "the Satellite Sender will publish video as well.";
			}

			obs_data_release(settings);
			inner->items->push_back(std::move(item));
		},
		state);
}

bool scan_source(void *param, obs_source_t *source)
{
	auto *state = static_cast<ScanState *>(param);

	const char *id = obs_source_get_id(source);
	if (id && strcmp(id, kDistroSourceId) == 0) {
		obs_data_t *settings = obs_source_get_settings(source);

		ImportItem item;
		item.kind = ImportItem::Kind::Source;
		item.source_id = id;
		item.owner = obs_source_get_name(source) ? obs_source_get_name(source) : "";

		const char *name = obs_data_get_string(settings, kDistroSourceName);
		item.network_name = name ? name : "";

		const int bandwidth = static_cast<int>(obs_data_get_int(settings, kDistroBandwidth));
		item.quality_id = quality_id(distroav_quality_from_bandwidth(bandwidth));
		item.audio = obs_data_get_bool(settings, kDistroAudio);

		item.label = item.owner;
		item.detail = "Satellite Source receiving '" + item.network_name + "' over NDI";

		obs_data_release(settings);
		state->items->push_back(std::move(item));
	}

	scan_filters(source, state);
	return true;
}

void scan_outputs(std::vector<ImportItem> *items)
{
	config_t *config = obs_user_config();
	if (!config)
		return;

	if (!config_has_user_value(config, kDistroConfigSection, kDistroMainName) &&
	    !config_has_user_value(config, kDistroConfigSection, kDistroPreviewName))
		return;

	const char *main_name = config_get_string(config, kDistroConfigSection, kDistroMainName);
	if (main_name && *main_name) {
		ImportItem item;
		item.kind = ImportItem::Kind::ProgramOutput;
		item.network_name = main_name;
		item.audio = true;
		item.quality_id = quality_id(Quality::Default);
		item.label = "Program output";
		item.detail = "Satellite Program sender named '" + item.network_name + "'";
		if (!config_get_bool(config, kDistroConfigSection, kDistroMainEnabled))
			item.detail += " (currently disabled in DistroAV, and will stay disabled)";
		items->push_back(std::move(item));
	}

	const char *preview_name = config_get_string(config, kDistroConfigSection, kDistroPreviewName);
	if (preview_name && *preview_name) {
		ImportItem item;
		item.kind = ImportItem::Kind::PreviewOutput;
		item.network_name = preview_name;
		item.audio = true;
		item.quality_id = quality_id(Quality::Default);
		item.label = "Preview output";
		item.detail = "Satellite Preview sender named '" + item.network_name + "'";
		if (!config_get_bool(config, kDistroConfigSection, kDistroPreviewEnabled))
			item.detail += " (currently disabled in DistroAV, and will stay disabled)";
		items->push_back(std::move(item));
	}
}

/// Builds the Satellite settings blob for a converted item.
obs_data_t *satellite_settings_for(const ImportItem &item, bool is_source)
{
	obs_data_t *settings = obs_data_create();

	obs_data_set_string(settings, "protocol", protocol_id(Protocol::NDI));
	obs_data_set_string(settings, "quality", item.quality_id.c_str());

	if (is_source) {
		obs_data_set_string(settings, "source", item.network_name.c_str());
		obs_data_set_bool(settings, "audio", item.audio);
	} else {
		obs_data_set_string(settings, "name", item.network_name.c_str());
		obs_data_set_bool(settings, "send_audio", item.audio);
	}

	return settings;
}

struct AddToScenes {
	obs_source_t *original = nullptr;
	obs_source_t *replacement = nullptr;
	bool remove_original = false;
	int placements = 0;
};

/// Mirrors every scene item of the DistroAV source with one for the Satellite source, keeping
/// the transform so the converted feed lands exactly where the old one was.
bool mirror_in_scene(void *param, obs_source_t *scene_source)
{
	auto *state = static_cast<AddToScenes *>(param);

	obs_scene_t *scene = obs_scene_from_source(scene_source);
	if (!scene)
		return true;

	struct ItemState {
		AddToScenes *outer;
		obs_scene_t *scene;
	} item_state{state, scene};

	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *inner_param) {
			auto *inner = static_cast<ItemState *>(inner_param);

			if (obs_sceneitem_get_source(item) != inner->outer->original)
				return true;

			obs_transform_info transform = {};
			sceneitem_get_transform(item, &transform);

			obs_sceneitem_t *added = obs_scene_add(inner->scene, inner->outer->replacement);
			if (added) {
				sceneitem_set_transform(added, &transform);
				obs_sceneitem_set_visible(added, obs_sceneitem_visible(item));
				++inner->outer->placements;
			}

			if (inner->outer->remove_original)
				obs_sceneitem_remove(item);

			return true;
		},
		&item_state);

	return true;
}

int convert_source(const ImportItem &item, bool remove_originals)
{
	obs_source_t *original = obs_get_source_by_name(item.owner.c_str());
	if (!original) {
		obs_log(LOG_WARNING, "DistroAV source '%s' vanished before it could be converted", item.owner.c_str());
		return 0;
	}

	// Keep the original's name free only when it is actually going away.
	const std::string name = remove_originals ? item.owner : item.owner + " (Satellite)";

	obs_data_t *settings = satellite_settings_for(item, true);
	obs_source_t *replacement = obs_source_create(
		"satellite_source", remove_originals ? (item.owner + " (Satellite)").c_str() : name.c_str(), settings,
		nullptr);
	obs_data_release(settings);

	if (!replacement) {
		obs_source_release(original);
		return 0;
	}

	AddToScenes state;
	state.original = original;
	state.replacement = replacement;
	state.remove_original = remove_originals;
	obs_enum_scenes(mirror_in_scene, &state);

	if (state.placements == 0)
		obs_log(LOG_INFO, "'%s' is not in any scene; the Satellite source was still created",
			item.owner.c_str());

	if (remove_originals)
		obs_source_remove(original);

	// The scenes hold their own references now.
	obs_source_release(replacement);
	obs_source_release(original);
	return 1;
}

int convert_filter(const ImportItem &item, bool remove_originals)
{
	obs_source_t *parent = obs_get_source_by_name(item.owner.c_str());
	if (!parent)
		return 0;

	obs_data_t *settings = satellite_settings_for(item, false);
	const std::string name = item.filter_name + " (Satellite)";
	obs_source_t *filter = obs_source_create_private("satellite_filter", name.c_str(), settings);
	obs_data_release(settings);

	if (!filter) {
		obs_source_release(parent);
		return 0;
	}

	obs_source_filter_add(parent, filter);

	if (remove_originals) {
		obs_source_t *original = obs_source_get_filter_by_name(parent, item.filter_name.c_str());
		if (original) {
			obs_source_filter_remove(parent, original);
			obs_source_release(original);
		}
	}

	obs_source_release(filter);
	obs_source_release(parent);
	return 1;
}

int convert_output(const ImportItem &item)
{
	Config &config = Config::instance();
	OutputConfig &target = item.kind == ImportItem::Kind::ProgramOutput ? config.program : config.preview;

	target.name = item.network_name;
	target.protocol = Protocol::NDI;
	target.send_audio = item.audio;

	Quality quality = Quality::Default;
	if (quality_from_id(item.quality_id.c_str(), quality))
		target.quality = quality;

	// Deliberately not enabled here. Converting settings is one thing; putting a feed on the
	// network without the user asking is another.
	return 1;
}

} // namespace

Quality distroav_quality_from_bandwidth(int bandwidth)
{
	switch (bandwidth) {
	case kDistroBwLowest:
		return Quality::PreviewOnly;
	case kDistroBwAudioOnly:
		return Quality::AudioOnly;
	case kDistroBwHighest:
	default:
		// PROP_BW_UNDEFINED (-1) and anything unrecognised land here. Full quality is the
		// safe reading: it is what DistroAV defaults to, and guessing lower would silently
		// degrade a feed on import.
		return Quality::Default;
	}
}

std::vector<ImportItem> scan_for_distroav()
{
	std::vector<ImportItem> items;

	ScanState state;
	state.items = &items;
	obs_enum_sources(scan_source, &state);

	scan_outputs(&items);
	return items;
}

int apply_distroav_import(const std::vector<ImportItem> &items, bool remove_originals)
{
	int converted = 0;
	bool touched_outputs = false;

	for (const ImportItem &item : items) {
		if (!item.selected)
			continue;

		switch (item.kind) {
		case ImportItem::Kind::Source:
			converted += convert_source(item, remove_originals);
			break;
		case ImportItem::Kind::Filter:
			converted += convert_filter(item, remove_originals);
			break;
		case ImportItem::Kind::ProgramOutput:
		case ImportItem::Kind::PreviewOutput:
			converted += convert_output(item);
			touched_outputs = true;
			break;
		}
	}

	if (touched_outputs)
		Config::instance().save();

	obs_log(LOG_INFO, "converted %d item(s) from DistroAV", converted);
	return converted;
}

} // namespace satellite
