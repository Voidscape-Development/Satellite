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

#include "obs/sender-session.hpp"
#include "transport/transport.hpp"

#include <string>

#include <obs-module.h>
#include <plugin-support.h>

namespace satellite {

namespace {

constexpr const char *kSettingProtocol = "protocol";
constexpr const char *kSettingName = "name";
constexpr const char *kSettingQuality = "quality";
constexpr const char *kSettingAudio = "send_audio";

/// Publishes the source it is attached to, over the selected protocol.
///
/// It does not intercept frames with filter_video. That would only ever see async sources -
/// a camera or a media file - and would miss everything OBS renders, which is most of what
/// people actually want to send: game capture, browser sources, whole scenes. Instead the
/// parent is rendered into a view of its own and the frames are taken from that view's
/// output, which works for every source type.
///
/// Audio comes from an audio capture callback on the parent, so the feed carries that
/// source's own sound rather than the program mix.
struct FilterContext {
	obs_source_t *filter = nullptr;

	Protocol protocol = Protocol::NDI;
	std::string name;
	Quality quality = Quality::Default;
	bool send_audio = true;

	SenderSession session;

	obs_view_t *view = nullptr;
	video_t *view_video = nullptr;
	/// What start_sending asked the view's output to convert to, which is what
	/// on_view_video then receives. The view's own info still reports the unconverted
	/// format, so this is the only description of the bytes that arrive.
	video_scale_info conversion = {};
	obs_source_t *captured_audio_source = nullptr;
	bool connected = false;
};

void on_view_video(void *param, video_data *frame)
{
	auto *context = static_cast<FilterContext *>(param);
	if (!context->view_video)
		return;

	const video_output_info *info = video_output_get_info(context->view_video);
	const video_scale_info &conversion = context->conversion;

	// The conversion, not the view's own info, is what describes data[0]: OBS has already
	// converted the frame by the time it lands here. Describing a UYVY buffer with the
	// view's format (NV12 by default) would send the backend looking for a second plane.
	VideoFrame video = {};
	video.width = conversion.width;
	video.height = conversion.height;
	video.format = conversion.format;
	video.colorspace = conversion.colorspace;
	video.range = conversion.range;
	video.framerate_num = info->fps_num;
	video.framerate_den = info->fps_den;
	video.timestamp_ns = frame->timestamp;
	video.data[0] = frame->data[0];
	video.linesize[0] = frame->linesize[0];

	context->session.push_video(video);
}

void on_source_audio(void *param, obs_source_t *, const audio_data *data, bool muted)
{
	auto *context = static_cast<FilterContext *>(param);
	if (muted || !context->send_audio)
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

void stop_sending(FilterContext *context)
{
	if (context->connected && context->view_video) {
		video_output_disconnect(context->view_video, on_view_video, context);
		context->connected = false;
	}

	if (context->captured_audio_source) {
		obs_source_remove_audio_capture_callback(context->captured_audio_source, on_source_audio, context);
		obs_source_release(context->captured_audio_source);
		context->captured_audio_source = nullptr;
	}

	if (context->view) {
		obs_view_set_source(context->view, 0, nullptr);
		obs_view_remove(context->view);
		obs_view_destroy(context->view);
		context->view = nullptr;
		context->view_video = nullptr;
	}

	context->session.stop();
}

void start_sending(FilterContext *context)
{
	if (context->name.empty())
		return;

	// Only valid once the filter has actually been attached, which is why starting is
	// deferred to the first tick rather than done in update().
	obs_source_t *parent = obs_filter_get_parent(context->filter);
	if (!parent)
		return;

	SenderConfig config;
	config.name = context->name;
	config.quality = context->quality;
	config.send_audio = context->send_audio;

	if (!context->session.start(context->protocol, config))
		return;

	obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi)) {
		context->session.stop();
		return;
	}

	context->view = obs_view_create();
	obs_view_set_source(context->view, 0, parent);
	context->view_video = obs_view_add2(context->view, &ovi);

	if (!context->view_video) {
		stop_sending(context);
		return;
	}

	// UYVY, so the frame reaching the backend needs no pixel conversion.
	context->conversion = {};
	context->conversion.format = VIDEO_FORMAT_UYVY;
	context->conversion.width = ovi.output_width;
	context->conversion.height = ovi.output_height;
	context->conversion.range = VIDEO_RANGE_PARTIAL;
	context->conversion.colorspace = ovi.output_height >= 720 ? VIDEO_CS_709 : VIDEO_CS_601;

	context->connected = video_output_connect(context->view_video, &context->conversion, on_view_video, context);
	if (!context->connected) {
		obs_log(LOG_WARNING, "could not connect to the render output for '%s'", context->name.c_str());
		stop_sending(context);
		return;
	}

	if (context->send_audio) {
		context->captured_audio_source = obs_source_get_ref(parent);
		if (context->captured_audio_source)
			obs_source_add_audio_capture_callback(context->captured_audio_source, on_source_audio, context);
	}
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

void filter_tick(void *data, float)
{
	auto *context = static_cast<FilterContext *>(data);

	// The parent is not attached yet when create() runs, so the first tick is the earliest
	// point at which the view can be built.
	if (!context->session.running() && !context->name.empty())
		start_sending(context);
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
