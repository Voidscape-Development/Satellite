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

#include "obs/satellite-output.hpp"

#include "config/config.hpp"
#include "obs/sender-session.hpp"
#include "transport/transport.hpp"

#include <memory>
#include <string>

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

namespace satellite {

namespace {

constexpr const char *kOutputId = "satellite_output";

constexpr const char *kSettingProtocol = "protocol";
constexpr const char *kSettingName = "name";
constexpr const char *kSettingQuality = "quality";
constexpr const char *kSettingAudio = "send_audio";

struct OutputContext {
	obs_output_t *output = nullptr;
	SenderSession session;
	/// What output_start asked OBS to convert to, which is what the raw_video callback
	/// then receives. Kept here because the video_t still reports its own unconverted
	/// format and nothing else describes the bytes that actually arrive.
	video_scale_info conversion = {};
	bool capturing = false;
};

/// Fills in the requested conversion for this output. Split out so the frame description and
/// the conversion request cannot drift apart.
video_scale_info conversion_for(const video_output_info *info)
{
	video_scale_info conversion = {};

	conversion.format = VIDEO_FORMAT_UYVY;
	conversion.width = info->width;
	conversion.height = info->height;
	conversion.range = VIDEO_RANGE_PARTIAL;
	conversion.colorspace = info->height >= 720 ? VIDEO_CS_709 : VIDEO_CS_601;

	return conversion;
}

VideoFrame to_video_frame(const video_data *data, const video_output_info *info, const video_scale_info &conversion)
{
	VideoFrame frame = {};

	// OBS applies the conversion before the frame reaches this callback, so the conversion
	// - not the video output's own info - is what describes data[0]. Taking the format from
	// info instead would label a UYVY buffer with the mix's format (NV12 by default), and
	// the backend would then read a plane that is not there.
	frame.width = conversion.width;
	frame.height = conversion.height;
	frame.format = conversion.format;
	frame.colorspace = conversion.colorspace;
	frame.range = conversion.range;
	frame.framerate_num = info->fps_num;
	frame.framerate_den = info->fps_den;
	frame.timestamp_ns = data->timestamp;
	frame.data[0] = data->data[0];
	frame.linesize[0] = data->linesize[0];

	return frame;
}

AudioFrame to_audio_frame(const audio_data *data, const audio_output_info *info)
{
	AudioFrame frame = {};

	frame.frames = data->frames;
	frame.sample_rate = info->samples_per_sec;
	frame.channels = get_audio_channels(info->speakers);
	frame.timestamp_ns = data->timestamp;

	for (size_t channel = 0; channel < MAX_AUDIO_CHANNELS; ++channel)
		frame.data[channel] = data->data[channel];

	return frame;
}

const char *output_get_name(void *)
{
	return obs_module_text("Satellite.Output");
}

void *output_create(obs_data_t *, obs_output_t *output)
{
	auto *context = new OutputContext();
	context->output = output;
	return context;
}

void output_destroy(void *data)
{
	delete static_cast<OutputContext *>(data);
}

bool output_start(void *data)
{
	auto *context = static_cast<OutputContext *>(data);

	obs_data_t *settings = obs_output_get_settings(context->output);

	Protocol protocol = Protocol::NDI;
	protocol_from_id(obs_data_get_string(settings, kSettingProtocol), protocol);

	Quality quality = Quality::Default;
	quality_from_id(obs_data_get_string(settings, kSettingQuality), quality);

	SenderConfig config;
	const char *name = obs_data_get_string(settings, kSettingName);
	config.name = name ? name : "";
	config.quality = quality;
	config.send_audio = obs_data_get_bool(settings, kSettingAudio);

	obs_data_release(settings);

	if (!obs_output_can_begin_data_capture(context->output, 0))
		return false;

	// Ask OBS for UYVY. Both protocols take it directly, so the frame that reaches the
	// backend needs no pixel conversion - the same zero-conversion path as receiving.
	video_t *video = obs_output_video(context->output);
	if (!video)
		return false;

	context->conversion = conversion_for(video_output_get_info(video));
	obs_output_set_video_conversion(context->output, &context->conversion);

	if (!context->session.start(protocol, config))
		return false;

	context->capturing = obs_output_begin_data_capture(context->output, 0);
	if (!context->capturing)
		context->session.stop();

	return context->capturing;
}

void output_stop(void *data, uint64_t)
{
	auto *context = static_cast<OutputContext *>(data);

	if (context->capturing) {
		obs_output_end_data_capture(context->output);
		context->capturing = false;
	}

	context->session.stop();
}

void output_raw_video(void *data, video_data *frame)
{
	auto *context = static_cast<OutputContext *>(data);

	video_t *video = obs_output_video(context->output);
	if (!video)
		return;

	context->session.push_video(to_video_frame(frame, video_output_get_info(video), context->conversion));
}

void output_raw_audio(void *data, audio_data *frames)
{
	auto *context = static_cast<OutputContext *>(data);

	audio_t *audio = obs_output_audio(context->output);
	if (!audio)
		return;

	context->session.push_audio(to_audio_frame(frames, audio_output_get_info(audio)));
}

/// One of the two frontend senders. Program follows the main mix; Preview renders the
/// preview scene through its own view, because OBS exposes no video_t for it.
class FrontendSender {
public:
	explicit FrontendSender(bool is_preview) : is_preview_(is_preview) {}
	~FrontendSender() { stop(); }

	void apply(const OutputConfig &desired, bool allowed)
	{
		const bool want_running = allowed && desired.enabled && !desired.name.empty();

		if (running_ && (!want_running || changed(desired))) {
			stop();
		} else if (running_ && is_preview_) {
			// Same configuration, but the preview scene may have changed under us.
			refresh_preview_scene();
			return;
		}

		if (want_running && !running_)
			start(desired);
	}

	void stop()
	{
		if (output_) {
			obs_output_stop(output_);
			obs_output_release(output_);
			output_ = nullptr;
		}

		release_view();
		running_ = false;
	}

private:
	bool changed(const OutputConfig &desired) const
	{
		return config_.enabled != desired.enabled || config_.name != desired.name ||
		       config_.protocol != desired.protocol || config_.quality != desired.quality ||
		       config_.send_audio != desired.send_audio;
	}

	void release_view()
	{
		if (!view_)
			return;

		obs_view_set_source(view_, 0, nullptr);
		obs_view_remove(view_);
		obs_view_destroy(view_);
		view_ = nullptr;
		view_video_ = nullptr;
	}

	void refresh_preview_scene()
	{
		if (!view_)
			return;

		obs_source_t *scene = obs_frontend_get_current_preview_scene();
		obs_view_set_source(view_, 0, scene);
		obs_source_release(scene);
	}

	void start(const OutputConfig &desired)
	{
		obs_data_t *settings = obs_data_create();
		obs_data_set_string(settings, kSettingName, desired.name.c_str());
		obs_data_set_string(settings, kSettingProtocol, protocol_id(desired.protocol));
		obs_data_set_string(settings, kSettingQuality, quality_id(desired.quality));
		obs_data_set_bool(settings, kSettingAudio, desired.send_audio);

		const std::string id = is_preview_ ? "satellite_preview" : "satellite_program";
		output_ = obs_output_create(kOutputId, id.c_str(), settings, nullptr);
		obs_data_release(settings);

		if (!output_)
			return;

		video_t *video = nullptr;
		if (is_preview_) {
			// OBS has no preview video_t to borrow, so the preview scene is rendered
			// into a view of our own at the project's current video settings.
			obs_video_info ovi = {};
			if (!obs_get_video_info(&ovi)) {
				stop();
				return;
			}

			view_ = obs_view_create();
			refresh_preview_scene();

			view_video_ = obs_view_add2(view_, &ovi);
			video = view_video_;
		} else {
			video = obs_get_video();
		}

		if (!video) {
			stop();
			return;
		}

		obs_output_set_media(output_, video, obs_get_audio());

		if (!obs_output_start(output_)) {
			obs_log(LOG_WARNING, "could not start the %s sender: %s", is_preview_ ? "Preview" : "Program",
				obs_output_get_last_error(output_));
			stop();
			return;
		}

		config_ = desired;
		running_ = true;
	}

	bool is_preview_ = false;
	bool running_ = false;
	OutputConfig config_;

	obs_output_t *output_ = nullptr;
	obs_view_t *view_ = nullptr;
	video_t *view_video_ = nullptr;
};

FrontendSender g_program(false);
FrontendSender g_preview(true);

} // namespace

void register_satellite_output()
{
	static obs_output_info info = {};

	info.id = kOutputId;
	info.flags = OBS_OUTPUT_AV;
	info.get_name = output_get_name;
	info.create = output_create;
	info.destroy = output_destroy;
	info.start = output_start;
	info.stop = output_stop;
	info.raw_video = output_raw_video;
	info.raw_audio = output_raw_audio;

	obs_register_output(&info);
}

void update_frontend_outputs()
{
	const Config &config = Config::instance();

	g_program.apply(config.program, true);
	// The Preview sender is meaningless outside Studio Mode, so it only runs there.
	g_preview.apply(config.preview, obs_frontend_preview_program_mode_active());
}

void shutdown_frontend_outputs()
{
	g_program.stop();
	g_preview.stop();
}

} // namespace satellite
