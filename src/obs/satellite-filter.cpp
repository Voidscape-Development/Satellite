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

#include "obs/satellite-filter.hpp"

#include "metrics/feed-registry.hpp"
#include "transport/transport.hpp"

#include <memory>
#include <string>

#include <obs-module.h>
#include <plugin-support.h>

namespace satellite {

namespace {

constexpr const char *kSettingProtocol = "protocol";
constexpr const char *kSettingName = "name";
constexpr const char *kSettingQuality = "quality";
constexpr const char *kSettingAudio = "send_audio";

struct FilterContext {
	obs_source_t *filter = nullptr;

	Protocol protocol = Protocol::NDI;
	std::string name;
	Quality quality = Quality::Default;
	bool send_audio = true;

	std::unique_ptr<ISender> sender;
	uint64_t feed_id = 0;
};

void stop_sending(FilterContext *context)
{
	context->sender.reset();

	if (context->feed_id) {
		FeedRegistry::instance().unregister_feed(context->feed_id);
		context->feed_id = 0;
	}
}

void start_sending(FilterContext *context)
{
	if (context->name.empty())
		return;

	IBackend *backend = backend_for(context->protocol);
	if (!backend || !backend->available())
		return;

	SenderConfig config;
	config.name = context->name;
	config.quality = context->quality;
	config.send_audio = context->send_audio;

	context->sender = backend->create_sender(config);
	if (!context->sender)
		return;

	context->feed_id =
		FeedRegistry::instance().register_feed(context->protocol, FeedDirection::Send, context->name);
}

const char *filter_get_name(void *)
{
	return obs_module_text("Satellite.Filter");
}

void filter_update(void *data, obs_data_t *settings)
{
	auto *context = static_cast<FilterContext *>(data);

	Protocol protocol = Protocol::NDI;
	protocol_from_id(obs_data_get_string(settings, kSettingProtocol), protocol);

	Quality quality = Quality::Default;
	quality_from_id(obs_data_get_string(settings, kSettingQuality), quality);

	const char *name = obs_data_get_string(settings, kSettingName);

	stop_sending(context);

	context->protocol = protocol;
	context->name = name ? name : "";
	context->quality = quality;
	context->send_audio = obs_data_get_bool(settings, kSettingAudio);

	start_sending(context);
}

void *filter_create(obs_data_t *settings, obs_source_t *filter)
{
	auto *context = new FilterContext();
	context->filter = filter;

	filter_update(context, settings);
	return context;
}

void filter_destroy(void *data)
{
	auto *context = static_cast<FilterContext *>(data);
	stop_sending(context);
	delete context;
}

void filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, kSettingProtocol, protocol_id(Protocol::NDI));
	obs_data_set_default_string(settings, kSettingQuality, quality_id(Quality::Default));
	obs_data_set_default_bool(settings, kSettingAudio, true);
}

obs_properties_t *filter_properties(void *)
{
	obs_properties_t *properties = obs_properties_create();

	obs_property_t *protocol = obs_properties_add_list(properties, kSettingProtocol,
							   obs_module_text("Satellite.Protocol"), OBS_COMBO_TYPE_LIST,
							   OBS_COMBO_FORMAT_STRING);
	for (Protocol value : kAllProtocols)
		obs_property_list_add_string(protocol, protocol_display_name(value), protocol_id(value));

	obs_properties_add_text(properties, kSettingName, obs_module_text("Satellite.Filter.Name"), OBS_TEXT_DEFAULT);

	obs_property_t *quality = obs_properties_add_list(properties, kSettingQuality,
							  obs_module_text("Satellite.Quality"), OBS_COMBO_TYPE_LIST,
							  OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(quality, obs_module_text("Satellite.Quality.Default"),
				     quality_id(Quality::Default));
	obs_property_list_add_string(quality, obs_module_text("Satellite.Quality.Low"), quality_id(Quality::Low));
	obs_property_list_add_string(quality, obs_module_text("Satellite.Quality.Medium"), quality_id(Quality::Medium));
	obs_property_list_add_string(quality, obs_module_text("Satellite.Quality.High"), quality_id(Quality::High));

	obs_properties_add_bool(properties, kSettingAudio, obs_module_text("Satellite.Filter.Audio"));

	return properties;
}

/// Pass-through: the filter publishes frames on the network, it does not alter them.
///
/// M2: tee the frame into the sender's queue here before returning it untouched.
obs_source_frame *filter_video(void *, obs_source_frame *frame)
{
	return frame;
}

obs_audio_data *filter_audio(void *, obs_audio_data *audio)
{
	return audio;
}

} // namespace

void register_satellite_filter()
{
	static obs_source_info info = {};

	info.id = "satellite_filter";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO;
	info.get_name = filter_get_name;
	info.create = filter_create;
	info.destroy = filter_destroy;
	info.update = filter_update;
	info.get_defaults = filter_defaults;
	info.get_properties = filter_properties;
	info.filter_video = filter_video;
	info.filter_audio = filter_audio;

	obs_register_source(&info);
}

} // namespace satellite
