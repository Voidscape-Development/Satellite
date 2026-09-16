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

#include "obs/satellite-source.hpp"

#include "discovery/discovery-service.hpp"
#include "metrics/feed-registry.hpp"
#include "transport/transport.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include <obs-module.h>
#include <plugin-support.h>

namespace satellite {

namespace {

constexpr const char *kSettingProtocol = "protocol";
constexpr const char *kSettingSource = "source";
constexpr const char *kSettingQuality = "quality";
constexpr const char *kSettingAudio = "audio";
constexpr const char *kSettingAlpha = "alpha";

struct SourceContext {
	obs_source_t *source = nullptr;

	Protocol protocol = Protocol::NDI;
	std::string address;
	Quality quality = Quality::Default;
	bool want_audio = true;
	bool want_alpha = false;

	std::unique_ptr<IReceiver> receiver;
	std::thread thread;
	std::atomic<bool> running{false};
	uint64_t feed_id = 0;
};

void stop_receiving(SourceContext *context)
{
	context->running.store(false, std::memory_order_release);
	if (context->thread.joinable())
		context->thread.join();

	context->receiver.reset();

	if (context->feed_id) {
		FeedRegistry::instance().unregister_feed(context->feed_id);
		context->feed_id = 0;
	}
}

void receive_loop(SourceContext *context)
{
	// M1/M3: pull frames from context->receiver and push them into OBS.
	//
	// A CapturedFrame is borrowed, not owned: OMT keeps frame data valid only until the
	// next omt_receive() on the same instance and frame type, and NDI needs an explicit
	// free. obs_source_output_video/_audio both copy, so the frame must be released before
	// the next capture - which is what letting it leave scope each iteration does.
	while (context->running.load(std::memory_order_acquire)) {
		CapturedFrame frame = context->receiver->capture(100);
		switch (frame.type()) {
		case FrameType::Video:
			break;
		case FrameType::Audio:
			break;
		case FrameType::Metadata:
		case FrameType::None:
			break;
		}
	}
}

void start_receiving(SourceContext *context)
{
	if (context->address.empty())
		return;

	IBackend *backend = backend_for(context->protocol);
	if (!backend || !backend->available()) {
		obs_log(LOG_INFO, "'%s' is configured for %s, which is not available: %s",
			obs_source_get_name(context->source), protocol_display_name(context->protocol),
			backend ? backend->unavailable_reason().c_str() : "no such backend");
		return;
	}

	ReceiverConfig config;
	config.address = context->address;
	config.quality = context->quality;
	config.want_video = context->quality != Quality::AudioOnly;
	config.want_audio = context->want_audio;
	config.want_alpha = context->want_alpha;

	context->receiver = backend->create_receiver(config);
	if (!context->receiver) {
		obs_log(LOG_WARNING, "could not create %s receiver for '%s'", protocol_display_name(context->protocol),
			context->address.c_str());
		return;
	}

	context->feed_id =
		FeedRegistry::instance().register_feed(context->protocol, FeedDirection::Receive, context->address);

	context->running.store(true, std::memory_order_release);
	context->thread = std::thread(receive_loop, context);
}

const char *source_get_name(void *)
{
	return obs_module_text("Satellite.Source");
}

void source_update(void *data, obs_data_t *settings)
{
	auto *context = static_cast<SourceContext *>(data);

	Protocol protocol = Protocol::NDI;
	protocol_from_id(obs_data_get_string(settings, kSettingProtocol), protocol);

	Quality quality = Quality::Default;
	quality_from_id(obs_data_get_string(settings, kSettingQuality), quality);

	const char *address = obs_data_get_string(settings, kSettingSource);

	SourceContext desired;
	desired.protocol = protocol;
	desired.address = address ? address : "";
	desired.quality = quality;
	desired.want_audio = obs_data_get_bool(settings, kSettingAudio);
	desired.want_alpha = obs_data_get_bool(settings, kSettingAlpha);

	const bool unchanged = context->protocol == desired.protocol && context->address == desired.address &&
			       context->quality == desired.quality && context->want_audio == desired.want_audio &&
			       context->want_alpha == desired.want_alpha;
	if (unchanged && context->receiver)
		return;

	stop_receiving(context);

	context->protocol = desired.protocol;
	context->address = desired.address;
	context->quality = desired.quality;
	context->want_audio = desired.want_audio;
	context->want_alpha = desired.want_alpha;

	start_receiving(context);
}

void *source_create(obs_data_t *settings, obs_source_t *source)
{
	auto *context = new SourceContext();
	context->source = source;

	source_update(context, settings);
	return context;
}

void source_destroy(void *data)
{
	auto *context = static_cast<SourceContext *>(data);
	stop_receiving(context);
	delete context;
}

void source_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, kSettingProtocol, protocol_id(Protocol::NDI));
	obs_data_set_default_string(settings, kSettingQuality, quality_id(Quality::Default));
	obs_data_set_default_bool(settings, kSettingAudio, true);
	obs_data_set_default_bool(settings, kSettingAlpha, false);
}

/// Repopulates the source list whenever the protocol changes, so the combo only ever offers
/// sources that the selected protocol can actually connect to.
bool on_protocol_modified(obs_properties_t *properties, obs_property_t *, obs_data_t *settings)
{
	Protocol protocol = Protocol::NDI;
	protocol_from_id(obs_data_get_string(settings, kSettingProtocol), protocol);

	obs_property_t *list = obs_properties_get(properties, kSettingSource);
	if (!list)
		return false;

	obs_property_list_clear(list);
	for (const SourceRef &source : DiscoveryService::instance().sources_for(protocol))
		obs_property_list_add_string(list, source.name.c_str(), source.address.c_str());

	return true;
}

obs_properties_t *source_properties(void *)
{
	obs_properties_t *properties = obs_properties_create();

	obs_property_t *protocol = obs_properties_add_list(properties, kSettingProtocol,
							   obs_module_text("Satellite.Protocol"), OBS_COMBO_TYPE_LIST,
							   OBS_COMBO_FORMAT_STRING);
	for (Protocol value : kAllProtocols)
		obs_property_list_add_string(protocol, protocol_display_name(value), protocol_id(value));
	obs_property_set_modified_callback(protocol, on_protocol_modified);

	// Editable so a direct address still works where mDNS is blocked: "omt://host:port"
	// for OMT, or a machine name for NDI.
	obs_properties_add_list(properties, kSettingSource, obs_module_text("Satellite.Source.Name"),
				OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);

	obs_property_t *quality = obs_properties_add_list(properties, kSettingQuality,
							  obs_module_text("Satellite.Quality"), OBS_COMBO_TYPE_LIST,
							  OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(quality, obs_module_text("Satellite.Quality.Default"),
				     quality_id(Quality::Default));
	obs_property_list_add_string(quality, obs_module_text("Satellite.Quality.Preview"),
				     quality_id(Quality::PreviewOnly));
	obs_property_list_add_string(quality, obs_module_text("Satellite.Quality.AudioOnly"),
				     quality_id(Quality::AudioOnly));

	obs_properties_add_bool(properties, kSettingAudio, obs_module_text("Satellite.Source.Audio"));
	obs_properties_add_bool(properties, kSettingAlpha, obs_module_text("Satellite.Source.Alpha"));

	return properties;
}

} // namespace

void register_satellite_source()
{
	static obs_source_info info = {};

	info.id = "satellite_source";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
	info.icon_type = OBS_ICON_TYPE_CAMERA;
	info.get_name = source_get_name;
	info.create = source_create;
	info.destroy = source_destroy;
	info.update = source_update;
	info.get_defaults = source_defaults;
	info.get_properties = source_properties;

	obs_register_source(&info);
}

} // namespace satellite
