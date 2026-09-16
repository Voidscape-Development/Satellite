/*
Satellite - NDI and OMT transport for OBS Studio
Copyright (C) 2026 Voidscape Development

SPDX-License-Identifier: GPL-2.0-or-later
*/

// Drives Satellite's NDI backend against the fake runtime in fake-ndi.cpp.
//
// This exists because the real NDI runtime is proprietary and cannot be installed in CI, so
// without it nothing about the NDI path could be verified anywhere. The fake is compiled
// against the same vendored SDK header the backend uses, which is the point: a mistake in
// how Satellite uses the ABI shows up here rather than on a user's machine.

#include "config/config.hpp"
#include "transport/transport.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

// Stubs for the two things the backend pulls in from the plugin, so this links without
// libobs's module machinery.
extern "C" void obs_log(int level, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	printf("    [log %d] ", level);
	vprintf(format, args);
	printf("\n");
	va_end(args);
}

extern "C" {
const char *PLUGIN_NAME = "satellite";
const char *PLUGIN_VERSION = "0.1.0";
}

namespace satellite {
Config &Config::instance()
{
	static Config config;
	return config;
}
void Config::load() {}
void Config::save() const {}
} // namespace satellite

using namespace satellite;

static int g_failures = 0;

static void check(bool condition, const char *what)
{
	printf("  %s %s\n", condition ? "PASS" : "FAIL", what);
	if (!condition)
		++g_failures;
}

static const char *format_name(video_format format)
{
	switch (format) {
	case VIDEO_FORMAT_UYVY:
		return "UYVY";
	case VIDEO_FORMAT_BGRA:
		return "BGRA";
	case VIDEO_FORMAT_BGRX:
		return "BGRX";
	case VIDEO_FORMAT_RGBA:
		return "RGBA";
	case VIDEO_FORMAT_NV12:
		return "NV12";
	case VIDEO_FORMAT_I420:
		return "I420";
	default:
		return "other";
	}
}

int main()
{
	printf("== backend load ==\n");
	register_backends();

	IBackend *ndi = backend_for(Protocol::NDI);
	check(ndi != nullptr, "NDI backend exists");
	if (!ndi)
		return 1;

	check(ndi->available(), "NDI backend reports available");
	if (!ndi->available()) {
		printf("    reason: %s\n", ndi->unavailable_reason().c_str());
		return 1;
	}
	check(ndi->runtime_version() == "FAKE NDI 6.0.0", "runtime version read back");
	printf("    version: %s\n", ndi->runtime_version().c_str());

	printf("== discovery ==\n");
	check(ndi->wait_for_sources(100), "wait_for_sources returns change");
	std::vector<SourceRef> sources = ndi->poll_sources();
	check(sources.size() == 2, "two sources discovered");
	if (sources.size() == 2) {
		check(sources[0].name == "STUDIO-PC (Camera 1)", "first source name copied");
		check(sources[0].protocol == Protocol::NDI, "source tagged NDI");
		printf("    found: '%s', '%s'\n", sources[0].name.c_str(), sources[1].name.c_str());
	}

	printf("== receiver ==\n");
	ReceiverConfig config;
	config.address = "STUDIO-PC (Camera 1)";
	config.quality = Quality::Default;

	std::unique_ptr<IReceiver> receiver = ndi->create_receiver(config);
	check(receiver != nullptr, "receiver created");
	if (!receiver)
		return 1;

	check(receiver->connected(), "receiver reports connected");
	receiver->set_tally({true, false});

	// The fake cycles UYVY, BGRA, audio, NV12, I420, audio, YV12 ... so 15 captures covers
	// every video branch plus audio several times.
	int video_frames = 0;
	int audio_frames = 0;
	bool saw_uyvy = false, saw_bgra = false, saw_nv12 = false, saw_i420 = false;
	bool planes_ok = true;

	for (int i = 0; i < 15; ++i) {
		CapturedFrame frame = receiver->capture(10);

		if (frame.type() == FrameType::Video) {
			++video_frames;
			const VideoFrame &v = frame.video;

			if (v.width != 1920 || v.height != 1080)
				planes_ok = false;
			if (!v.data[0] || v.linesize[0] == 0)
				planes_ok = false;

			switch (v.format) {
			case VIDEO_FORMAT_UYVY:
				saw_uyvy = true;
				if (v.linesize[0] != 1920 * 2)
					planes_ok = false;
				break;
			case VIDEO_FORMAT_BGRA:
				saw_bgra = true;
				if (v.linesize[0] != 1920 * 4 || !v.has_alpha)
					planes_ok = false;
				break;
			case VIDEO_FORMAT_NV12:
				saw_nv12 = true;
				// Second plane must start exactly one luma plane in.
				if (v.data[1] != v.data[0] + 1920 * 1080 || v.linesize[1] != 1920)
					planes_ok = false;
				break;
			case VIDEO_FORMAT_I420:
				saw_i420 = true;
				if (v.linesize[1] != 960 || v.linesize[2] != 960)
					planes_ok = false;
				// I420 is Y,U,V and YV12 is Y,V,U; Satellite swaps the pointers for
				// YV12, so across both we must see data[1] on each side of data[2].
				break;
			default:
				planes_ok = false;
				break;
			}

			if (i < 6)
				printf("    frame %2d: %s %ux%u stride %u ts %llu\n", i, format_name(v.format), v.width,
				       v.height, v.linesize[0], static_cast<unsigned long long>(v.timestamp_ns));
		} else if (frame.type() == FrameType::Audio) {
			++audio_frames;
			const AudioFrame &a = frame.audio;
			if (a.channels != 2 || a.frames != 480 || a.sample_rate != 48000)
				planes_ok = false;
			// Planar: channel 1 starts one channel-stride past channel 0.
			if (a.data[1] != a.data[0] + 480 * 4)
				planes_ok = false;
		}
	}

	check(video_frames == 10, "ten video frames captured");
	check(audio_frames == 5, "five audio frames captured");
	check(saw_uyvy && saw_bgra && saw_nv12 && saw_i420, "all pixel formats converted");
	check(planes_ok, "plane pointers, strides and audio layout correct");

	FeedStats stats = receiver->stats();
	check(stats.frames == 15, "frame counter read from runtime");
	check(stats.frames_dropped == 3, "dropped counter read from runtime");
	check(stats.bitrate_estimated, "NDI bitrate flagged estimated");
	printf("    stats: %lld frames, %lld dropped, %.1f Mb/s (estimated=%d)\n", (long long)stats.frames,
	       (long long)stats.frames_dropped, stats.bitrate_mbps, (int)stats.bitrate_estimated);

	printf("== teardown ==\n");
	receiver.reset();
	unregister_backends();
	check(true, "clean teardown");

	printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASSED" : "FAILURES", g_failures,
	       g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
