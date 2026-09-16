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
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

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

	// Outbound tally. OBS calls activate/deactivate when a source goes on and off program,
	// and show/hide when it becomes visible anywhere - which includes the Studio Mode
	// preview. Tracking both is how a remote camera learns it is live in this OBS, and it
	// is event-driven, so nothing has to walk the scene graph on a timer.
	std::atomic<bool> on_program{false};
	std::atomic<bool> on_preview{false};
};

/// Pushes this source's program/preview state upstream to whoever is sending to us.
void publish_tally(SourceContext *context)
{
	if (!context->receiver)
		return;

	Tally tally;
	tally.program = context->on_program.load(std::memory_order_acquire);

	// "Showing but not active" is the Studio Mode preview. A source on program is also
	// showing, so without this a live source would report both at once.
	tally.preview = context->on_preview.load(std::memory_order_acquire) && !tally.program;

	context->receiver->set_tally(tally);

	if (context->feed_id)
		FeedRegistry::instance().set_tally(context->feed_id, tally);
}

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

/// How long a single capture may block. Short enough that stopping the source is responsive,
/// long enough that an idle feed is not a busy loop.
constexpr int kCaptureTimeoutMs = 100;

/// OBS's speaker_layout values are defined so that each one equals its channel count, which
/// is why the usual idiom is a straight cast. Seven channels is the gap in that scheme and
/// has no layout, so it is reported as unknown and the caller drops the frame rather than
/// handing OBS a layout that would make it read a plane that is not there.
speaker_layout speakers_for(uint32_t channels)
{
	switch (channels) {
	case 1:
		return SPEAKERS_MONO;
	case 2:
		return SPEAKERS_STEREO;
	case 3:
		return SPEAKERS_2POINT1;
	case 4:
		return SPEAKERS_4POINT0;
	case 5:
		return SPEAKERS_4POINT1;
	case 6:
		return SPEAKERS_5POINT1;
	case 8:
		return SPEAKERS_7POINT1;
	default:
		return SPEAKERS_UNKNOWN;
	}
}

std::string describe_video(const VideoFrame &frame)
{
	const char *format = get_video_format_name(frame.format);

	char buffer[128];
	if (frame.framerate_den > 0) {
		const double fps = static_cast<double>(frame.framerate_num) / static_cast<double>(frame.framerate_den);
		snprintf(buffer, sizeof(buffer), "%ux%u%s%.2f %s", frame.width, frame.height,
			 frame.interlaced ? "i" : "p", fps, format);
	} else {
		snprintf(buffer, sizeof(buffer), "%ux%u %s", frame.width, frame.height, format);
	}

	return buffer;
}

std::string describe_audio(const AudioFrame &frame)
{
	char buffer[64];
	snprintf(buffer, sizeof(buffer), "%.4g kHz %uch", static_cast<double>(frame.sample_rate) / 1000.0,
		 frame.channels);
	return buffer;
}

void publish_video(SourceContext *context, const VideoFrame &frame)
{
	obs_source_frame obs_frame = {};

	obs_frame.width = frame.width;
	obs_frame.height = frame.height;
	obs_frame.format = frame.format;
	obs_frame.timestamp = frame.timestamp_ns;
	obs_frame.flip = false;

	for (size_t plane = 0; plane < MAX_AV_PLANES; ++plane) {
		obs_frame.data[plane] = frame.data[plane];
		obs_frame.linesize[plane] = frame.linesize[plane];
	}

	// Fills in the YUV->RGB matrix and the colour range for the format. Skipped for RGB
	// formats, where OBS does not use a conversion matrix.
	if (frame.colorspace != VIDEO_CS_DEFAULT)
		video_format_get_parameters_for_format(frame.colorspace, frame.range, frame.format,
						       obs_frame.color_matrix, obs_frame.color_range_min,
						       obs_frame.color_range_max);

	obs_frame.full_range = frame.range == VIDEO_RANGE_FULL;

	obs_source_output_video(context->source, &obs_frame);
}

void publish_audio(SourceContext *context, const AudioFrame &frame)
{
	if (frame.channels == 0 || frame.frames == 0)
		return;

	const speaker_layout speakers = speakers_for(frame.channels);
	if (speakers == SPEAKERS_UNKNOWN)
		return;

	obs_source_audio obs_audio = {};

	obs_audio.frames = frame.frames;
	obs_audio.samples_per_sec = frame.sample_rate;
	obs_audio.timestamp = frame.timestamp_ns;

	// Both protocols carry 32-bit planar float, which is what OBS uses internally, so the
	// samples are handed over as-is.
	obs_audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
	obs_audio.speakers = speakers;

	for (size_t channel = 0; channel < MAX_AUDIO_CHANNELS; ++channel)
		obs_audio.data[channel] = frame.data[channel];

	obs_source_output_audio(context->source, &obs_audio);
}

/// Keeps the Satellite window's row for this feed current.
void publish_stats(SourceContext *context, const FeedStats &stats, bool connected)
{
	FeedRegistry &registry = FeedRegistry::instance();

	registry.update_stats(context->feed_id, stats);
	registry.set_state(context->feed_id, connected ? FeedState::Connected : FeedState::Connecting);
}

void receive_loop(SourceContext *context)
{
	// A CapturedFrame is borrowed, not owned: OMT keeps frame data valid only until the
	// next omt_receive() on the same instance and frame type, and NDI needs an explicit
	// free. obs_source_output_video/_audio both copy, so the frame must be released before
	// the next capture - which is what letting it leave scope each iteration does.
	uint64_t last_stats_ns = 0;
	std::string last_video_format;
	std::string last_audio_format;

	while (context->running.load(std::memory_order_acquire)) {
		CapturedFrame frame = context->receiver->capture(kCaptureTimeoutMs);

		switch (frame.type()) {
		case FrameType::Video: {
			// An unsupported pixel format yields a typed frame with no planes, so that
			// its destructor still frees the allocation. Skip it rather than publishing.
			if (!frame.video.data[0])
				break;

			publish_video(context, frame.video);

			std::string format = describe_video(frame.video);
			if (format != last_video_format) {
				FeedRegistry::instance().set_video_format(context->feed_id, format);
				last_video_format = std::move(format);
			}
			break;
		}

		case FrameType::Audio: {
			if (!context->want_audio)
				break;

			publish_audio(context, frame.audio);

			std::string format = describe_audio(frame.audio);
			if (format != last_audio_format) {
				FeedRegistry::instance().set_audio_format(context->feed_id, format);
				last_audio_format = std::move(format);
			}
			break;
		}

		case FrameType::Metadata:
		case FrameType::None:
			break;
		}

		// Polling the counters costs a lock and, on NDI, a call into the runtime, so it is
		// throttled to roughly the dock's own refresh rate rather than run per frame.
		const uint64_t now = os_gettime_ns();
		if (now - last_stats_ns >= 500000000ULL) {
			publish_stats(context, context->receiver->stats(), context->receiver->connected());
			last_stats_ns = now;
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

	// A receiver starts with tally off, so re-publish whatever OBS already told us - the
	// source may well have been on program before its settings changed.
	publish_tally(context);
}

void source_activate(void *data)
{
	auto *context = static_cast<SourceContext *>(data);
	context->on_program.store(true, std::memory_order_release);
	publish_tally(context);
}

void source_deactivate(void *data)
{
	auto *context = static_cast<SourceContext *>(data);
	context->on_program.store(false, std::memory_order_release);
	publish_tally(context);
}

void source_show(void *data)
{
	auto *context = static_cast<SourceContext *>(data);
	context->on_preview.store(true, std::memory_order_release);
	publish_tally(context);
}

void source_hide(void *data)
{
	auto *context = static_cast<SourceContext *>(data);
	context->on_preview.store(false, std::memory_order_release);
	publish_tally(context);
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

/// Fills the source combo from the discovery registry, keeping whatever is currently
/// selected in the list even if discovery has not seen it (yet, or at all) - otherwise
/// opening properties on a source whose sender is briefly offline would silently clear it.
void populate_sources(obs_properties_t *properties, Protocol protocol, const char *selected)
{
	obs_property_t *list = obs_properties_get(properties, kSettingSource);
	if (!list)
		return;

	obs_property_list_clear(list);

	bool selected_listed = !selected || !*selected;
	for (const SourceRef &source : DiscoveryService::instance().sources_for(protocol)) {
		obs_property_list_add_string(list, source.name.c_str(), source.address.c_str());
		if (!selected_listed && source.address == selected)
			selected_listed = true;
	}

	if (!selected_listed)
		obs_property_list_add_string(list, selected, selected);
}

/// Repopulates the list whenever the protocol changes, so the combo only ever offers sources
/// the selected protocol can actually connect to.
bool on_protocol_modified(obs_properties_t *properties, obs_property_t *, obs_data_t *settings)
{
	Protocol protocol = Protocol::NDI;
	protocol_from_id(obs_data_get_string(settings, kSettingProtocol), protocol);

	populate_sources(properties, protocol, obs_data_get_string(settings, kSettingSource));
	return true;
}

/// Rescans on demand. Discovery itself runs continuously in the background, so this only
/// re-reads the registry - it does not kick off a scan.
bool on_refresh_clicked(obs_properties_t *properties, obs_property_t *, void *data)
{
	auto *context = static_cast<SourceContext *>(data);
	populate_sources(properties, context ? context->protocol : Protocol::NDI,
			 context ? context->address.c_str() : nullptr);
	return true;
}

obs_properties_t *source_properties(void *data)
{
	auto *context = static_cast<SourceContext *>(data);
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
	populate_sources(properties, context ? context->protocol : Protocol::NDI,
			 context ? context->address.c_str() : nullptr);

	obs_properties_add_button2(properties, "refresh", obs_module_text("Satellite.Source.Refresh"),
				   on_refresh_clicked, context);

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
	info.activate = source_activate;
	info.deactivate = source_deactivate;
	info.show = source_show;
	info.hide = source_hide;

	obs_register_source(&info);
}

} // namespace satellite
