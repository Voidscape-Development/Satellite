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
#include "obs/obs-compat.hpp"
#include "obs/sender-session.hpp"
#include "transport/transport.hpp"

#include <algorithm>
#include <atomic>
#include <map>
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
		desired_ = desired;
		allowed_ = allowed;

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

	SenderStatus status() const
	{
		SenderStatus status;
		if (!desired_.enabled)
			return status;

		if (running_) {
			status.state = SenderState::Live;
		} else if (desired_.name.empty()) {
			status.state = SenderState::Failed;
			status.detail = obs_module_text("Satellite.Sender.NeedsName");
		} else if (!allowed_) {
			status.state = SenderState::Waiting;
			status.detail = obs_module_text("Satellite.Sender.WaitingStudioMode");
		} else {
			status.state = SenderState::Failed;
			status.detail = last_error_;
		}

		return status;
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
		last_error_.clear();

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
			const char *error = obs_output_get_last_error(output_);
			obs_log(LOG_WARNING, "could not start the %s sender: %s", is_preview_ ? "Preview" : "Program",
				error ? error : "unknown error");
			stop();
			last_error_ = error ? error : obs_module_text("Satellite.Sender.CouldNotStart");
			return;
		}

		config_ = desired;
		running_ = true;
	}

	bool is_preview_ = false;
	bool running_ = false;
	bool allowed_ = false;
	OutputConfig config_;
	OutputConfig desired_;
	std::string last_error_;

	obs_output_t *output_ = nullptr;
	obs_view_t *view_ = nullptr;
	video_t *view_video_ = nullptr;
};

FrontendSender g_program(false);
FrontendSender g_preview(true);

/// Set once the frontend senders have been torn down, so that an update queued by a canvas
/// signal just before shutdown cannot start them again.
std::atomic<bool> g_shut_down{false};

#if SATELLITE_HAVE_CANVASES

std::atomic<bool> g_update_queued{false};
bool g_canvas_signals_connected = false;

/// Canvas signals arrive on whichever thread changed the canvas, often with the canvas's own
/// locks held. Nothing is done there: the update is deferred to the UI thread, where the
/// senders are owned, and coalesced so a burst of scene changes costs one update.
void queue_update()
{
	if (g_shut_down.load() || g_update_queued.exchange(true))
		return;

	obs_queue_task(
		OBS_TASK_UI,
		[](void *) {
			g_update_queued.store(false);
			if (!g_shut_down.load())
				update_frontend_outputs();
		},
		nullptr, false);
}

void on_canvas_signal(void *, calldata_t *)
{
	queue_update();
}

/// A canvas a sender can carry: not the main one (that is Program), not removed, and with
/// a video mix of its own. A canvas can exist without one.
bool sendable_canvas(obs_canvas_t *canvas)
{
	obs_video_info ovi = {};
	return canvas && !(obs_canvas_get_flags(canvas) & MAIN) && !obs_canvas_removed(canvas) &&
	       obs_canvas_get_video_info(canvas, &ovi);
}

/// A strong reference to the canvas a sender names, or null. Matched by UUID first; the name
/// is the fallback for a canvas that is recreated, with a new UUID, on every launch.
obs_canvas_t *find_canvas(const CanvasSenderConfig &config)
{
	if (!config.canvas_uuid.empty()) {
		obs_canvas_t *canvas = obs_get_canvas_by_uuid(config.canvas_uuid.c_str());
		if (sendable_canvas(canvas))
			return canvas;
		obs_canvas_release(canvas);
	}

	if (!config.canvas_name.empty()) {
		obs_canvas_t *canvas = obs_get_canvas_by_name(config.canvas_name.c_str());
		if (sendable_canvas(canvas))
			return canvas;
		obs_canvas_release(canvas);
	}

	return nullptr;
}

bool same_video(const obs_video_info &a, const obs_video_info &b)
{
	return a.base_width == b.base_width && a.base_height == b.base_height && a.output_width == b.output_width &&
	       a.output_height == b.output_height && a.fps_num == b.fps_num && a.fps_den == b.fps_den &&
	       a.output_format == b.output_format && a.colorspace == b.colorspace && a.range == b.range;
}

/// Sends one of OBS's additional canvases - a vertical canvas, say.
///
/// The canvas is rendered through a view of Satellite's own, mirroring the canvas's
/// channels, rather than by borrowing the canvas's video_t. That costs a second render of
/// the canvas, the same price the Preview sender pays, and it is what makes the sender safe:
/// whoever owns a canvas can reset its video at any moment, which frees the canvas's video_t
/// with no warning beforehand, and an output still attached to it would then be torn down
/// through freed memory. A view of our own has a video_t that only we free.
class CanvasSender {
public:
	explicit CanvasSender(int id) : id_(id) {}
	~CanvasSender() { stop(); }

	CanvasSender(const CanvasSender &) = delete;
	CanvasSender &operator=(const CanvasSender &) = delete;

	void apply(const CanvasSenderConfig &desired)
	{
		desired_ = desired;

		const bool want_running = desired.enabled && !desired.name.empty();
		obs_canvas_t *canvas = want_running ? find_canvas(desired) : nullptr;
		canvas_missing_ = want_running && !canvas;

		if (running_ && (!want_running || changed(desired) || canvas != canvas_ || video_changed()))
			stop();

		if (running_)
			// Same canvas, same settings, but its scene or transition may have changed.
			sync_channels();
		else if (canvas)
			start(desired, canvas);

		obs_canvas_release(canvas);
	}

	void stop()
	{
		if (output_) {
			obs_output_stop(output_);
			obs_output_release(output_);
			output_ = nullptr;
		}

		if (view_) {
			for (uint32_t channel = 0; channel < MAX_CHANNELS; ++channel)
				obs_view_set_source(view_, channel, nullptr);
			obs_view_remove(view_);
			obs_view_destroy(view_);
			view_ = nullptr;
		}

		if (canvas_) {
			signal_handler_t *handler = obs_canvas_get_signal_handler(canvas_);
			for (const char *signal : kCanvasSignals)
				signal_handler_disconnect(handler, signal, on_canvas_signal, this);

			obs_canvas_release(canvas_);
			canvas_ = nullptr;
		}

		running_ = false;
	}

	SenderStatus status() const
	{
		SenderStatus status;
		if (!desired_.enabled)
			return status;

		if (running_) {
			status.state = SenderState::Live;
		} else if (desired_.name.empty()) {
			status.state = SenderState::Failed;
			status.detail = obs_module_text("Satellite.Sender.NeedsName");
		} else if (canvas_missing_) {
			status.state = SenderState::Waiting;
			status.detail = obs_module_text("Satellite.Sender.WaitingCanvas");
		} else {
			status.state = SenderState::Failed;
			status.detail = last_error_;
		}

		return status;
	}

private:
	/// "remove" lets go of a canvas its owner is done with; "video_reset" restarts at the
	/// new size; "channel_change" follows scene and transition changes.
	static constexpr const char *kCanvasSignals[] = {"remove", "video_reset", "channel_change"};

	bool changed(const CanvasSenderConfig &desired) const
	{
		return config_.name != desired.name || config_.protocol != desired.protocol ||
		       config_.quality != desired.quality || config_.audio_track != desired.audio_track;
	}

	bool video_changed() const
	{
		obs_video_info ovi = {};
		return canvas_ && (!obs_canvas_get_video_info(canvas_, &ovi) || !same_video(ovi, ovi_));
	}

	void sync_channels()
	{
		if (!view_ || !canvas_)
			return;

		for (uint32_t channel = 0; channel < MAX_CHANNELS; ++channel) {
			obs_source_t *source = obs_canvas_get_channel(canvas_, channel);
			obs_source_t *current = obs_view_get_source(view_, channel);

			// Setting the same source again would deactivate and reactivate it.
			if (source != current)
				obs_view_set_source(view_, channel, source);

			obs_source_release(current);
			obs_source_release(source);
		}
	}

	void fail(const char *reason)
	{
		obs_log(LOG_WARNING, "could not start the canvas sender '%s': %s", desired_.name.c_str(), reason);
		stop();
		last_error_ = reason;
	}

	void start(const CanvasSenderConfig &desired, obs_canvas_t *canvas)
	{
		last_error_.clear();

		obs_video_info ovi = {};
		if (!obs_canvas_get_video_info(canvas, &ovi)) {
			fail(obs_module_text("Satellite.Sender.CanvasNoVideo"));
			return;
		}

		obs_data_t *settings = obs_data_create();
		obs_data_set_string(settings, kSettingName, desired.name.c_str());
		obs_data_set_string(settings, kSettingProtocol, protocol_id(desired.protocol));
		obs_data_set_string(settings, kSettingQuality, quality_id(desired.quality));
		obs_data_set_bool(settings, kSettingAudio, desired.audio_track > 0);

		const std::string id = "satellite_canvas_" + std::to_string(id_);
		output_ = obs_output_create(kOutputId, id.c_str(), settings, nullptr);
		obs_data_release(settings);

		if (!output_) {
			fail(obs_module_text("Satellite.Sender.CouldNotStart"));
			return;
		}

		// A raw output takes its audio from a single mixer track; this picks which.
		if (desired.audio_track > 0)
			obs_output_set_mixer(output_, static_cast<size_t>(desired.audio_track - 1));

		canvas_ = obs_canvas_get_ref(canvas);
		ovi_ = ovi;

		signal_handler_t *handler = obs_canvas_get_signal_handler(canvas_);
		for (const char *signal : kCanvasSignals)
			signal_handler_connect(handler, signal, on_canvas_signal, this);

		view_ = obs_view_create();
		sync_channels();

		video_t *video = obs_view_add2(view_, &ovi);
		if (!video) {
			fail(obs_module_text("Satellite.Sender.CouldNotStart"));
			return;
		}

		obs_output_set_media(output_, video, obs_get_audio());

		if (!obs_output_start(output_)) {
			const char *error = obs_output_get_last_error(output_);
			fail(error ? error : obs_module_text("Satellite.Sender.CouldNotStart"));
			return;
		}

		config_ = desired;
		running_ = true;
	}

	const int id_;
	bool running_ = false;
	bool canvas_missing_ = false;
	CanvasSenderConfig config_;
	CanvasSenderConfig desired_;
	std::string last_error_;

	obs_canvas_t *canvas_ = nullptr;
	obs_video_info ovi_ = {};
	obs_output_t *output_ = nullptr;
	obs_view_t *view_ = nullptr;
};

/// Keyed by CanvasSenderConfig::id, so removing one sender leaves the others running.
std::map<int, std::unique_ptr<CanvasSender>> g_canvas_senders;

void update_canvas_senders(const std::vector<CanvasSenderConfig> &configs)
{
	if (!g_canvas_signals_connected) {
		// A configured canvas may not exist yet - its plugin can load after Satellite, or
		// create it only once a scene collection loads - so a new canvas re-runs the update.
		signal_handler_connect(obs_get_signal_handler(), "canvas_create", on_canvas_signal, nullptr);
		g_canvas_signals_connected = true;
	}

	for (auto it = g_canvas_senders.begin(); it != g_canvas_senders.end();) {
		const bool configured =
			std::any_of(configs.begin(), configs.end(),
				    [&](const CanvasSenderConfig &config) { return config.id == it->first; });
		it = configured ? std::next(it) : g_canvas_senders.erase(it);
	}

	for (const CanvasSenderConfig &config : configs) {
		std::unique_ptr<CanvasSender> &sender = g_canvas_senders[config.id];
		if (!sender)
			sender = std::make_unique<CanvasSender>(config.id);
		sender->apply(config);
	}
}

void shutdown_canvas_senders()
{
	if (g_canvas_signals_connected) {
		signal_handler_disconnect(obs_get_signal_handler(), "canvas_create", on_canvas_signal, nullptr);
		g_canvas_signals_connected = false;
	}

	// Each sender holds a canvas reference, which has to go before libobs shuts down.
	g_canvas_senders.clear();
}

#endif

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
	if (g_shut_down.load())
		return;

	const Config &config = Config::instance();

	g_program.apply(config.program, true);
	// The Preview sender is meaningless outside Studio Mode, so it only runs there.
	g_preview.apply(config.preview, obs_frontend_preview_program_mode_active());

#if SATELLITE_HAVE_CANVASES
	update_canvas_senders(config.canvas_senders);
#endif
}

void shutdown_frontend_outputs()
{
	g_shut_down.store(true);

	g_program.stop();
	g_preview.stop();

#if SATELLITE_HAVE_CANVASES
	shutdown_canvas_senders();
#endif
}

SenderStatus program_sender_status()
{
	return g_program.status();
}

SenderStatus preview_sender_status()
{
	return g_preview.status();
}

SenderStatus canvas_sender_status(int id)
{
#if SATELLITE_HAVE_CANVASES
	const auto it = g_canvas_senders.find(id);
	if (it != g_canvas_senders.end())
		return it->second->status();
#else
	(void)id;
#endif
	return {};
}

bool canvases_supported()
{
	return SATELLITE_HAVE_CANVASES != 0;
}

std::vector<CanvasInfo> list_canvases()
{
	std::vector<CanvasInfo> canvases;

#if SATELLITE_HAVE_CANVASES
	obs_enum_canvases(
		[](void *param, obs_canvas_t *canvas) {
			if (!sendable_canvas(canvas))
				return true;

			obs_video_info ovi = {};
			obs_canvas_get_video_info(canvas, &ovi);

			const char *uuid = obs_canvas_get_uuid(canvas);
			const char *name = obs_canvas_get_name(canvas);

			CanvasInfo info;
			info.uuid = uuid ? uuid : "";
			info.name = name ? name : "";
			info.width = ovi.output_width;
			info.height = ovi.output_height;
			static_cast<std::vector<CanvasInfo> *>(param)->push_back(std::move(info));
			return true;
		},
		&canvases);
#endif

	return canvases;
}

} // namespace satellite
