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
#include "metrics/feed-registry.hpp"
#include "transport/transport.hpp"

#include <memory>
#include <string>

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

namespace satellite {

namespace {

struct OutputContext {
	obs_output_t *output = nullptr;
	std::unique_ptr<ISender> sender;
	uint64_t feed_id = 0;
	bool active = false;
};

/// The two frontend senders. Preview only runs while Studio Mode is on.
struct FrontendSender {
	OutputConfig config;
	uint64_t feed_id = 0;
	std::unique_ptr<ISender> sender;
	bool running = false;
};

FrontendSender g_program;
FrontendSender g_preview;

void stop_sender(FrontendSender &target)
{
	if (!target.running)
		return;

	target.sender.reset();

	if (target.feed_id) {
		FeedRegistry::instance().unregister_feed(target.feed_id);
		target.feed_id = 0;
	}

	target.running = false;
}

void start_sender(FrontendSender &target, const OutputConfig &config)
{
	if (!config.enabled || config.name.empty())
		return;

	IBackend *backend = backend_for(config.protocol);
	if (!backend || !backend->available()) {
		obs_log(LOG_INFO, "not starting '%s': %s is unavailable", config.name.c_str(),
			protocol_display_name(config.protocol));
		return;
	}

	SenderConfig sender_config;
	sender_config.name = config.name;
	sender_config.quality = config.quality;
	sender_config.send_audio = config.send_audio;

	target.sender = backend->create_sender(sender_config);
	if (!target.sender)
		return;

	target.feed_id = FeedRegistry::instance().register_feed(config.protocol, FeedDirection::Send, config.name);
	target.config = config;
	target.running = true;
}

bool config_changed(const OutputConfig &a, const OutputConfig &b)
{
	return a.enabled != b.enabled || a.name != b.name || a.protocol != b.protocol || a.quality != b.quality ||
	       a.send_audio != b.send_audio;
}

void apply(FrontendSender &target, const OutputConfig &desired, bool allowed)
{
	const bool want_running = allowed && desired.enabled;

	if (target.running && (!want_running || config_changed(target.config, desired)))
		stop_sender(target);

	if (want_running && !target.running)
		start_sender(target, desired);
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

	// M2: negotiate raw video/audio conversion with obs_output_set_video_conversion, start
	// the send thread, and only then report started.
	if (!obs_output_can_begin_data_capture(context->output, 0))
		return false;

	context->active = obs_output_begin_data_capture(context->output, 0);
	return context->active;
}

void output_stop(void *data, uint64_t)
{
	auto *context = static_cast<OutputContext *>(data);
	if (!context->active)
		return;

	obs_output_end_data_capture(context->output);
	context->active = false;
}

void output_raw_video(void *, video_data *)
{
	// M2: convert to the backend's preferred format (UYVY for both protocols, so the
	// common path is a field copy) and hand to ISender::send_video.
}

void output_raw_audio(void *, audio_data *)
{
	// M2: both protocols carry 32-bit planar float, matching OBS's internal format, so
	// this is a field copy too.
}

} // namespace

void register_satellite_output()
{
	static obs_output_info info = {};

	info.id = "satellite_output";
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

	apply(g_program, config.program, true);
	// The Preview sender is meaningless outside Studio Mode, so it only runs there.
	apply(g_preview, config.preview, obs_frontend_preview_program_mode_active());
}

void shutdown_frontend_outputs()
{
	stop_sender(g_program);
	stop_sender(g_preview);
}

void broadcast_tally()
{
	// M2: derive program/preview state from the frontend and publish it on every active
	// sender, so remote sources know whether they are live in this OBS.
}

} // namespace satellite
