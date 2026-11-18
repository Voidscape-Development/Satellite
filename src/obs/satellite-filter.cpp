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

#include "obs/satellite-output.hpp"
#include "obs/sender-session.hpp"
#include "transport/transport.hpp"

#include <mutex>
#include <string>

#include <obs-module.h>
#include <plugin-support.h>

namespace satellite {

namespace {

constexpr const char *kSettingProtocol = "protocol";
constexpr const char *kSettingName = "name";
constexpr const char *kSettingQuality = "quality";
constexpr const char *kSettingAudio = "send_audio";

/// How long to leave a sender that could not start before trying again. The retry runs from
/// the tick, and a missing runtime would otherwise be retried, and logged, at frame rate.
constexpr float kRetrySeconds = 2.0f;

/// Publishes the source it is attached to, over the selected protocol.
///
/// It does not intercept frames with filter_video. That would only ever see async sources -
/// a camera or a media file - and would miss everything OBS renders, which is most of what
/// people actually want to send: game capture, browser sources, whole scenes. Instead the
/// parent is rendered into a view of its own, the size of the source, and the frames are
/// taken from that view's output, which works for every source type.
///
/// That view is a second render of the source and a GPU readback of every frame, so it
/// exists only while somebody is receiving the feed. The sender itself - the feed's name on
/// the network - is up for as long as the filter is.
///
/// Audio comes from an audio capture callback on the parent, so the feed carries that
/// source's own sound rather than the program mix.
struct FilterContext {
	obs_source_t *filter = nullptr;

	/// update() and destroy() arrive on the UI thread, the tick on the graphics thread, and
	/// both start and stop things. Everything below is guarded by this.
	std::mutex mutex;

	Protocol protocol = Protocol::NDI;
	std::string name;
	Quality quality = Quality::Default;
	bool send_audio = true;

	SenderSession session;
	obs_source_t *captured_audio_source = nullptr;

	SenderCapture capture;
	obs_view_t *view = nullptr;
	uint32_t width = 0;
	uint32_t height = 0;

	/// Seconds until the next attempt to start whatever last failed to.
	float retry_in = 0.0f;
};

void on_source_audio(void *param, obs_source_t *, const audio_data *data, bool muted)
{
	auto *context = static_cast<FilterContext *>(param);

	// send_audio is not read here: the callback is only registered when it is set, and
	// this runs on the audio thread, outside the lock.
	if (muted)
		return;

	const audio_t *audio = obs_get_audio();
	if (!audio)
		return;

	const audio_output_info *info = audio_output_get_info(audio);

	AudioFrame frame = {};
	frame.frames = data->frames;
	frame.sample_rate = info->samples_per_sec;
	frame.channels = get_audio_channels(info->speakers);
	frame.timestamp_ns = data->timestamp;

	for (size_t channel = 0; channel < MAX_AUDIO_CHANNELS; ++channel)
		frame.data[channel] = data->data[channel];

	context->session.push_audio(frame);
}

void stop_capture(FilterContext *context)
{
	// The output first: it holds the view's video_t until it has stopped.
	context->capture.stop();

	if (context->view) {
		obs_view_set_source(context->view, 0, nullptr);
		obs_view_remove(context->view);
		obs_view_destroy(context->view);
		context->view = nullptr;
	}

	context->width = 0;
	context->height = 0;
}

void stop_sending(FilterContext *context)
{
	stop_capture(context);

	if (context->captured_audio_source) {
		obs_source_remove_audio_capture_callback(context->captured_audio_source, on_source_audio, context);
		obs_source_release(context->captured_audio_source);
		context->captured_audio_source = nullptr;
	}

	context->session.stop();
}

/// Puts the feed on the network. No frames flow until sync_capture finds a receiver.
bool start_sending(FilterContext *context, obs_source_t *parent)
{
	SenderConfig config;
	config.name = context->name;
	config.quality = context->quality;
	config.send_audio = context->send_audio;

	if (!context->session.start(context->protocol, config))
		return false;

	if (context->send_audio) {
		context->captured_audio_source = obs_source_get_ref(parent);
		if (context->captured_audio_source)
			obs_source_add_audio_capture_callback(context->captured_audio_source, on_source_audio, context);
	}

	return true;
}

/// Brings the capture in line with whether anybody is receiving, and with the source's size.
bool sync_capture(FilterContext *context, obs_source_t *parent)
{
	// Rounded up to even: the view produces 4:2:0 video, which has no odd sizes.
	const uint32_t width = (obs_source_get_width(parent) + 1) & ~1u;
	const uint32_t height = (obs_source_get_height(parent) + 1) & ~1u;

	// A source with no size yet - a capture still waiting for its window, say - has
	// nothing to send, and is looked at again on the next tick.
	if (!context->session.wanted() || width == 0 || height == 0) {
		stop_capture(context);
		return true;
	}

	if (context->capture.active() && width == context->width && height == context->height)
		return true;

	stop_capture(context);

	// The view is the size of the source rather than of the canvas: a 720p camera on a 4K
	// canvas is a 720p feed, and costs a 720p render and readback.
	obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi))
		return false;

	ovi.base_width = width;
	ovi.base_height = height;
	ovi.output_width = width;
	ovi.output_height = height;

	context->view = obs_view_create();
	obs_view_set_source(context->view, 0, parent);

	if (!context->capture.start(obs_source_get_name(context->filter), context->session,
				    obs_view_add2(context->view, &ovi), -1)) {
		obs_log(LOG_WARNING, "could not capture video for '%s': %s", context->name.c_str(),
			context->capture.error().c_str());
		stop_capture(context);
		return false;
	}

	context->width = width;
	context->height = height;
	return true;
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

	std::lock_guard<std::mutex> lock(context->mutex);

	stop_sending(context);

	context->protocol = protocol;
	context->name = name ? name : "";
	context->quality = quality;
	context->send_audio = obs_data_get_bool(settings, kSettingAudio);

	// Starting is left to the next tick: the parent is not attached yet when this runs
	// from create().
	context->retry_in = 0.0f;
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
	{
		std::lock_guard<std::mutex> lock(context->mutex);
		stop_sending(context);
	}
	delete context;
}

void filter_tick(void *data, float seconds)
{
	auto *context = static_cast<FilterContext *>(data);

	// If the UI thread is in the middle of an update, this tick has nothing to add - and
	// the render thread never waits on it.
	std::unique_lock<std::mutex> lock(context->mutex, std::try_to_lock);
	if (!lock.owns_lock() || context->name.empty())
		return;

	if (context->retry_in > 0.0f) {
		context->retry_in -= seconds;
		return;
	}

	obs_source_t *parent = obs_filter_get_parent(context->filter);
	if (!parent)
		return;

	// The view holds a reference to the source it renders. Once the source has been
	// removed that reference is the only thing keeping it - and this filter - alive.
	if (obs_source_removed(parent)) {
		stop_sending(context);
		return;
	}

	const bool ok = (context->session.running() || start_sending(context, parent)) && sync_capture(context, parent);
	if (!ok)
		context->retry_in = kRetrySeconds;
}

/// Pass-through: the filter publishes the source on the network, it does not alter what OBS
/// draws.
void filter_render(void *data, gs_effect_t *)
{
	auto *context = static_cast<FilterContext *>(data);
	obs_source_skip_video_filter(context->filter);
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

	obs_property_t *name = obs_properties_add_text(properties, kSettingName,
						       obs_module_text("Satellite.Filter.Name"), OBS_TEXT_DEFAULT);
	obs_property_set_long_description(name, obs_module_text("Satellite.Filter.Name.Hint"));

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

} // namespace

void register_satellite_filter()
{
	static obs_source_info info = {};

	info.id = "satellite_filter";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = filter_get_name;
	info.create = filter_create;
	info.destroy = filter_destroy;
	info.update = filter_update;
	info.get_defaults = filter_defaults;
	info.get_properties = filter_properties;
	info.video_render = filter_render;
	info.video_tick = filter_tick;

	obs_register_source(&info);
}

} // namespace satellite
