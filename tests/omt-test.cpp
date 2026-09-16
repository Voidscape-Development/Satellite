/*
Satellite - NDI and OMT transport for OBS Studio
Copyright (C) 2026 Voidscape Development

SPDX-License-Identifier: GPL-2.0-or-later
*/

// Drives Satellite's OMT backend against the fake libomt in fake-omt.cpp.

#include "config/config.hpp"
#include "metrics/feed-registry.hpp"
#include "transport/transport.hpp"

#include <libomt.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <thread>
#include <vector>

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

// Mirrors the struct the fake exports; see tests/fake-omt.cpp.
struct FakeOmtLog {
	int discovery_calls;
	int receivers_created;
	int receivers_destroyed;
	char last_address[256];
	int last_frame_types;
	int last_format;
	int last_receive_flags;
	int last_suggested_quality;
	int last_recv_tally_program;
	int last_recv_tally_preview;

	int senders_created;
	int senders_destroyed;
	char last_sender_name[256];
	int last_sender_quality;
	int video_sent;
	int audio_sent;
	int last_sent_codec;
	int last_sent_width;
	int last_sent_height;
	int last_sent_stride;
	int last_sent_flags;
	long long last_sent_timestamp;
	int last_sent_audio_channels;
	int last_sent_audio_samples;
	int last_sent_audio_rate;
	int last_sent_audio_length;

	char last_discovery_server[256];
	int last_port_start;
	int last_port_end;
	int shutdown_calls;
};

using namespace satellite;

static int g_failures = 0;

static void check(bool condition, const char *what)
{
	printf("  %s %s\n", condition ? "PASS" : "FAIL", what);
	if (!condition)
		++g_failures;
}

static const FakeOmtLog *open_log()
{
	// The backend finds the fake through plugin_binary_directory(), which for a test binary
	// is the directory the executable sits in - the same place CMake puts libomt.so.
	void *handle = dlopen("libomt.so", RTLD_LOCAL | RTLD_LAZY | RTLD_NOLOAD);
	if (!handle)
		handle = dlopen("libomt.so", RTLD_LOCAL | RTLD_LAZY);
	if (!handle) {
		printf("    dlopen failed: %s\n", dlerror());
		return nullptr;
	}

	auto fn = reinterpret_cast<const FakeOmtLog *(*)(void)>(dlsym(handle, "fake_omt_log"));
	return fn ? fn() : nullptr;
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
	case VIDEO_FORMAT_I420:
		return "I420";
	default:
		return "other";
	}
}

int main()
{
	// Settings must be in place before the backend loads, because it pushes them into
	// libomt at load time.
	Config &config = Config::instance();
	config.omt_discovery_server = "omt://discovery.example:9999";
	config.omt_port_start = 6500;
	config.omt_port_end = 6600;

	printf("== backend load ==\n");
	register_backends();

	IBackend *omt = backend_for(Protocol::OMT);
	check(omt != nullptr, "OMT backend exists");
	if (!omt)
		return 1;

	check(omt->available(), "OMT backend reports available");
	if (!omt->available()) {
		printf("    reason: %s\n", omt->unavailable_reason().c_str());
		return 1;
	}
	check(omt->install_url().empty(), "no install link offered for OMT");

	const FakeOmtLog *log = open_log();
	check(log != nullptr, "fake log reachable");
	if (!log)
		return 1;

	check(strcmp(log->last_discovery_server, "omt://discovery.example:9999") == 0,
	      "discovery server pushed into libomt");
	check(log->last_port_start == 6500 && log->last_port_end == 6600, "port range pushed into libomt");

	printf("== discovery ==\n");
	std::vector<SourceRef> sources = omt->poll_sources();
	check(sources.size() == 2, "two sources discovered");
	if (sources.size() == 2) {
		check(sources[0].protocol == Protocol::OMT, "source tagged OMT");
		check(sources[0].name == "STUDIO-PC (OMT Camera 1)", "name copied out of the discovery array");
		printf("    found: '%s', '%s'\n", sources[0].name.c_str(), sources[1].name.c_str());
	}

	// The array libomt returns is only valid until the next call, so the copies made above
	// must survive a second call that would invalidate it.
	omt->poll_sources();
	check(sources.size() == 2 && sources[0].name == "STUDIO-PC (OMT Camera 1)",
	      "copies survive a later discovery call");

	printf("== receiver ==\n");
	ReceiverConfig receiver_config;
	receiver_config.address = "STUDIO-PC (OMT Camera 1)";
	receiver_config.quality = Quality::High;

	std::unique_ptr<IReceiver> receiver = omt->create_receiver(receiver_config);
	check(receiver != nullptr, "receiver created");
	if (!receiver)
		return 1;

	check(strcmp(log->last_address, "STUDIO-PC (OMT Camera 1)") == 0, "address passed through");
	check(log->last_format == OMTPreferredVideoFormat_UYVYorBGRA, "asked for UYVY-or-BGRA");
	check((log->last_frame_types & OMTFrameType_Video) && (log->last_frame_types & OMTFrameType_Audio),
	      "requested video and audio");
	check(log->last_suggested_quality == OMTQuality_High, "suggested quality passed through");

	int video_frames = 0;
	int audio_frames = 0;
	bool saw_uyvy = false, saw_bgra = false, saw_bgrx = false;
	bool layout_ok = true;

	for (int i = 0; i < 12; ++i) {
		CapturedFrame frame = receiver->capture(10);

		if (frame.type() == FrameType::Video) {
			++video_frames;
			const VideoFrame &v = frame.video;

			if (v.width != 1280 || v.height != 720)
				layout_ok = false;
			if (!v.data[0])
				layout_ok = false;
			// 10000 ticks of 100ns is 1ms.
			if (v.timestamp_ns == 0)
				layout_ok = false;

			switch (v.format) {
			case VIDEO_FORMAT_UYVY:
				saw_uyvy = true;
				if (v.linesize[0] != 1280 * 2 || v.has_alpha)
					layout_ok = false;
				break;
			case VIDEO_FORMAT_BGRA:
				saw_bgra = true;
				if (!v.has_alpha)
					layout_ok = false;
				break;
			case VIDEO_FORMAT_BGRX:
				saw_bgrx = true;
				if (v.has_alpha)
					layout_ok = false;
				break;
			default:
				layout_ok = false;
				break;
			}

			if (i < 4)
				printf("    frame %d: %s %ux%u stride %u ts %lluns\n", i, format_name(v.format),
				       v.width, v.height, v.linesize[0],
				       static_cast<unsigned long long>(v.timestamp_ns));
		} else if (frame.type() == FrameType::Audio) {
			++audio_frames;
			const AudioFrame &a = frame.audio;
			if (a.channels != 2 || a.frames != 480 || a.sample_rate != 48000)
				layout_ok = false;
			// Planar float packed back to back.
			if (a.data[1] != a.data[0] + 480 * 4)
				layout_ok = false;
		}
	}

	check(video_frames == 9 && audio_frames == 3, "frames captured in the expected mix");
	check(saw_uyvy && saw_bgra && saw_bgrx, "UYVY, BGRA and BGRX all decoded");
	check(layout_ok, "resolution, strides, alpha flag, timestamps and audio layout correct");

	receiver->set_tally({true, false});
	check(log->last_recv_tally_program == 1 && log->last_recv_tally_preview == 0, "tally sent upstream");

	// The first stats call has no interval to divide by, so rates only appear on the second.
	receiver->stats();
	std::this_thread::sleep_for(std::chrono::milliseconds(120));
	FeedStats stats = receiver->stats();

	check(!stats.bitrate_estimated, "OMT bitrate reported exact, unlike NDI");
	check(stats.bitrate_mbps > 0.0, "bitrate derived from OMT byte counters");
	check(stats.frames_dropped == 7, "dropped counter read from libomt");
	check(stats.codec_ms == 4.0, "codec time read from libomt");
	printf("    stats: %.1f Mb/s, %.0f fps, %lld dropped, %.0f ms codec\n", stats.bitrate_mbps, stats.fps,
	       (long long)stats.frames_dropped, stats.codec_ms);

	const int destroyed_before = log->receivers_destroyed;
	receiver.reset();
	check(log->receivers_destroyed == destroyed_before + 1, "receiver destroyed on release");

	printf("== sender ==\n");
	SenderConfig sender_config;
	sender_config.name = "Satellite OMT Test";
	sender_config.quality = Quality::Medium;

	std::unique_ptr<ISender> sender = omt->create_sender(sender_config);
	check(sender != nullptr, "sender created");
	if (!sender)
		return 1;

	check(strcmp(log->last_sender_name, "Satellite OMT Test") == 0, "sender name passed through");
	check(log->last_sender_quality == OMTQuality_Medium, "sender quality mapped");

	std::vector<uint8_t> pixels(static_cast<size_t>(1280) * 2 * 720, 0x40);
	VideoFrame video = {};
	video.width = 1280;
	video.height = 720;
	video.format = VIDEO_FORMAT_UYVY;
	video.framerate_num = 60000;
	video.framerate_den = 1001;
	video.colorspace = VIDEO_CS_709;
	video.timestamp_ns = 1234567800;
	video.data[0] = pixels.data();
	video.linesize[0] = 1280 * 2;

	check(sender->send_video(video), "video accepted");
	check(log->last_sent_codec == OMTCodec_UYVY, "UYVY mapped to the OMT codec");
	check(log->last_sent_stride == 1280 * 2, "stride passed through");
	check(log->last_sent_timestamp == 12345678, "timestamp converted to 100ns units");
	check((log->last_sent_flags & OMTVideoFlags_Alpha) == 0,
	      "alpha flag left clear for an opaque frame, so OMT keeps BGRX semantics");

	std::vector<float> samples(960, 0.1f);
	AudioFrame audio = {};
	audio.channels = 2;
	audio.frames = 480;
	audio.sample_rate = 48000;
	audio.timestamp_ns = 1000000000;
	audio.data[0] = reinterpret_cast<uint8_t *>(samples.data());
	audio.data[1] = reinterpret_cast<uint8_t *>(samples.data() + 480);

	check(sender->send_audio(audio), "audio accepted");
	check(log->last_sent_audio_channels == 2 && log->last_sent_audio_samples == 480, "audio shape passed through");
	check(log->last_sent_audio_rate == 48000, "sample rate passed through");
	check(log->last_sent_audio_length == 480 * 2 * 4, "audio byte length covers both planes");

	Tally tally = sender->tally();
	check(tally.program && !tally.preview, "tally read back from libomt");
	check(sender->connections() == 3, "connection count read back");

	sender->stats();
	std::this_thread::sleep_for(std::chrono::milliseconds(120));
	FeedStats send_stats = sender->stats();
	check(!send_stats.bitrate_estimated, "sender bitrate also exact");
	check(send_stats.connections == 3, "sender stats carry connections");

	sender.reset();

	printf("== teardown ==\n");
	const int shutdowns_before = log->shutdown_calls;
	unregister_backends();
	check(log->shutdown_calls == shutdowns_before + 1, "omt_shutdown called before the library is released");

	printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASSED" : "FAILURES", g_failures,
	       g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
