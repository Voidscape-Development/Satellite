/*
Satellite - NDI and OMT transport for OBS Studio
Copyright (C) 2026 Voidscape Development

SPDX-License-Identifier: GPL-2.0-or-later
*/

// Drives Satellite's NDI send path against the fake runtime in fake-ndi.cpp.
//
// Covers the backend sender (what actually reaches the NDI ABI) and SenderSession (the
// bounded queue, the buffer copies, and the send thread), neither of which can be exercised
// against the real runtime in CI.

#include "config/config.hpp"
#include "metrics/feed-registry.hpp"
#include "obs/sender-session.hpp"
#include "transport/transport.hpp"

#include <Processing.NDI.Lib.h>

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

// Mirrors the struct the fake exports; see tests/fake-ndi.cpp.
struct FakeSendLog {
	int created;
	int destroyed;
	char last_name[256];
	int video_frames;
	int audio_frames;
	int last_fourcc;
	int last_xres;
	int last_yres;
	int last_stride;
	long long last_video_timecode;
	int last_audio_fourcc;
	int last_audio_channels;
	int last_audio_samples;
	int last_audio_stride;
	int last_audio_rate;
};

using namespace satellite;

static int g_failures = 0;

static void check(bool condition, const char *what)
{
	printf("  %s %s\n", condition ? "PASS" : "FAIL", what);
	if (!condition)
		++g_failures;
}

static const FakeSendLog *open_send_log()
{
	// dlopen of an already-loaded library returns the same handle the backend holds, so
	// this reads the very globals the backend's calls have been writing.
	std::string path =
		std::string(getenv("NDI_RUNTIME_DIR_V6") ? getenv("NDI_RUNTIME_DIR_V6") : ".") + "/libndi.so.6";

	void *handle = dlopen(path.c_str(), RTLD_LOCAL | RTLD_LAZY);
	if (!handle) {
		printf("    dlopen failed: %s\n", dlerror());
		return nullptr;
	}

	auto fn = reinterpret_cast<const FakeSendLog *(*)(void)>(dlsym(handle, "fake_ndi_send_log"));
	return fn ? fn() : nullptr;
}

// A 4:2:2 UYVY frame is two bytes per pixel.
static constexpr uint32_t kWidth = 1280;
static constexpr uint32_t kHeight = 720;
static constexpr uint32_t kStride = kWidth * 2;

static VideoFrame make_video(std::vector<uint8_t> &storage, uint64_t timestamp_ns, uint8_t fill)
{
	storage.assign(static_cast<size_t>(kStride) * kHeight, fill);

	VideoFrame frame = {};
	frame.width = kWidth;
	frame.height = kHeight;
	frame.format = VIDEO_FORMAT_UYVY;
	frame.framerate_num = 60000;
	frame.framerate_den = 1001;
	frame.timestamp_ns = timestamp_ns;
	frame.data[0] = storage.data();
	frame.linesize[0] = kStride;
	return frame;
}

int main()
{
	printf("== backend ==\n");
	register_backends();

	IBackend *ndi = backend_for(Protocol::NDI);
	if (!ndi || !ndi->available()) {
		printf("  FAIL NDI backend unavailable: %s\n", ndi ? ndi->unavailable_reason().c_str() : "missing");
		return 1;
	}

	const FakeSendLog *log = open_send_log();
	check(log != nullptr, "fake send log reachable");
	if (!log)
		return 1;

	printf("== direct sender ==\n");
	SenderConfig config;
	config.name = "Satellite Test";
	config.quality = Quality::Default;

	std::unique_ptr<ISender> sender = ndi->create_sender(config);
	check(sender != nullptr, "sender created");
	if (!sender)
		return 1;

	// The bare name goes to NDI; the machine prefix is the runtime's doing, not ours.
	check(strcmp(log->last_name, "Satellite Test") == 0, "sender name passed through unprefixed");

	std::vector<uint8_t> storage;
	VideoFrame video = make_video(storage, 1234567800, 0x80);
	check(sender->send_video(video), "video accepted");
	check(log->video_frames == 1, "video reached the runtime");
	check(log->last_fourcc == NDI_LIB_FOURCC('U', 'Y', 'V', 'Y'), "UYVY mapped to the NDI FourCC");
	check(log->last_xres == static_cast<int>(kWidth) && log->last_yres == static_cast<int>(kHeight),
	      "resolution passed through");
	check(log->last_stride == static_cast<int>(kStride), "stride passed through");

	// OBS nanoseconds to NDI's 100ns units.
	check(log->last_video_timecode == 12345678, "timestamp converted to 100ns units");

	std::vector<float> left(480, 0.25f);
	std::vector<float> right(480, -0.25f);
	AudioFrame audio = {};
	audio.channels = 2;
	audio.frames = 480;
	audio.sample_rate = 48000;
	audio.timestamp_ns = 1000000000;
	audio.data[0] = reinterpret_cast<uint8_t *>(left.data());
	audio.data[1] = reinterpret_cast<uint8_t *>(right.data());

	check(sender->send_audio(audio), "audio accepted");
	check(log->last_audio_fourcc == NDI_LIB_FOURCC('F', 'L', 'T', 'p'), "audio sent as planar float");
	check(log->last_audio_channels == 2 && log->last_audio_samples == 480, "audio shape passed through");
	check(log->last_audio_stride == 480 * 4, "audio channel stride in bytes");
	check(log->last_audio_rate == 48000, "sample rate passed through");

	Tally tally = sender->tally();
	check(tally.program && !tally.preview, "tally read back from the runtime");
	check(sender->connections() == 2, "connection count read back");

	const int before_destroy = log->destroyed;
	sender.reset();
	check(log->destroyed == before_destroy + 1, "sender destroyed on release");

	printf("== session queue, sink keeping up ==\n");
	int video_before = log->video_frames;
	int audio_before = log->audio_frames;

	{
		SenderSession session;
		check(session.start(Protocol::NDI, config), "session started");
		check(session.running(), "session reports running");

		for (int i = 0; i < 40; ++i) {
			VideoFrame frame = make_video(storage, 100000000ULL * (i + 1), static_cast<uint8_t>(i));
			session.push_video(frame);
			// Slower than the sink, so nothing should need dropping.
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		for (int i = 0; i < 10; ++i)
			session.push_audio(audio);

		std::this_thread::sleep_for(std::chrono::milliseconds(400));

		const int video_sent = log->video_frames - video_before;
		const int audio_sent = log->audio_frames - audio_before;
		printf("    pushed 40 video / 10 audio, sent %d video / %d audio\n", video_sent, audio_sent);

		check(video_sent == 40, "every video frame forwarded when the sink keeps up");
		check(audio_sent == 10, "every audio frame forwarded");

		// The pushed buffers are local to this scope, so anything the thread sent must have
		// been its own copy rather than a dangling pointer into them.
		check(log->last_stride == static_cast<int>(kStride), "queued frame kept its stride through the copy");

		check(session.tally().program, "session polled tally");

		std::vector<FeedSnapshot> feeds = FeedRegistry::instance().snapshot();
		check(feeds.size() == 1, "session registered one feed");
		if (feeds.size() == 1) {
			check(feeds[0].direction == FeedDirection::Send, "feed registered as a sender");
			check(feeds[0].stats.connections == 2, "feed carries the connection count");
			check(feeds[0].stats.frames_dropped == 0, "nothing dropped while the sink keeps up");
		}

		session.stop();
		check(!session.running(), "session stopped");
		check(FeedRegistry::instance().snapshot().empty(), "feed unregistered on stop");
	}

	printf("== session queue, sink too slow ==\n");
	video_before = log->video_frames;
	audio_before = log->audio_frames;

	// A sink slow enough that pushing at full tilt must overflow the queue. This is the only
	// way to reach the drop policy, and it is the condition that policy exists for: keep the
	// freshest frames rather than growing an unbounded backlog of stale ones.
	setenv("FAKE_NDI_SEND_DELAY_MS", "20", 1);

	{
		SenderSession session;
		check(session.start(Protocol::NDI, config), "session started under backpressure");

		for (int i = 0; i < 40; ++i) {
			VideoFrame frame = make_video(storage, 100000000ULL * (i + 1), static_cast<uint8_t>(i));
			session.push_video(frame);
		}
		for (int i = 0; i < 10; ++i)
			session.push_audio(audio);

		std::this_thread::sleep_for(std::chrono::milliseconds(600));

		const int video_sent = log->video_frames - video_before;
		const int audio_sent = log->audio_frames - audio_before;

		std::vector<FeedSnapshot> feeds = FeedRegistry::instance().snapshot();
		const int64_t dropped = feeds.empty() ? -1 : feeds[0].stats.frames_dropped;
		printf("    pushed 40 video / 10 audio, sent %d video / %d audio, dropped %lld\n", video_sent,
		       audio_sent, (long long)dropped);

		check(video_sent < 40, "video dropped rather than buffered when the sink stalls");
		check(dropped > 0, "dropped frames counted");
		check(audio_sent == 10, "audio never dropped, even under video backpressure");

		session.stop();
	}

	unsetenv("FAKE_NDI_SEND_DELAY_MS");

	unregister_backends();

	printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASSED" : "FAILURES", g_failures,
	       g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
