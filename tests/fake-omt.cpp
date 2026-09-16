/*
Satellite - NDI and OMT transport for OBS Studio
Copyright (C) 2026 Voidscape Development

SPDX-License-Identifier: GPL-2.0-or-later
*/

// A fake libomt, built against Satellite's vendored libomt.h.
//
// libomt is built from C# with .NET NativeAOT and upstream publishes no binaries, so on most
// machines - and in CI - there is nothing real to load. This exports the same flat C symbols
// the backend binds, compiled against the same header, so a mistake in how Satellite uses
// the ABI surfaces here rather than on a user's machine.
//
// It is deliberately built as libomt.so next to the test binaries, so the backend's real
// "bundled beside the plugin" search path is what finds it.

#include <cstdlib>
#include <cstring>

#include <libomt.h>

namespace {

struct FakeReceive {
	int capture_count = 0;
	OMTFrameType frame_types = OMTFrameType_None;
	OMTPreferredVideoFormat format = OMTPreferredVideoFormat_UYVY;
	OMTReceiveFlags flags = OMTReceiveFlags_None;
	OMTQuality suggested = OMTQuality_Default;
	OMTTally tally = {};
	OMTMediaFrame frame = {};
	unsigned char *video = nullptr;
	float *audio = nullptr;
};

struct FakeSend {
	int dummy = 0;
};

constexpr int kWidth = 1280;
constexpr int kHeight = 720;

} // namespace

/// What the fake has been asked to do. The test dlopen's this same library to read it.
extern "C" struct FakeOmtLog {
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

namespace {

FakeOmtLog g_log;

char *g_addresses[2] = {const_cast<char *>("STUDIO-PC (OMT Camera 1)"), const_cast<char *>("STUDIO-PC (OMT Camera 2)")};

} // namespace

// libomt.h does not annotate its declarations for visibility - the real library exports them
// through the NativeAOT toolchain instead - so under the project's -fvisibility=hidden every
// symbol below would be invisible to dlsym. The pragma makes this translation unit export
// them the way the real libomt does, independent of compiler flags.
#pragma GCC visibility push(default)

extern "C" {

char **omt_discovery_getaddresses(int *count)
{
	++g_log.discovery_calls;
	if (count)
		*count = 2;
	return g_addresses;
}

omt_receive_t *omt_receive_create(const char *address, OMTFrameType frameTypes, OMTPreferredVideoFormat format,
				  OMTReceiveFlags flags)
{
	++g_log.receivers_created;
	if (address) {
		strncpy(g_log.last_address, address, sizeof(g_log.last_address) - 1);
		g_log.last_address[sizeof(g_log.last_address) - 1] = '\0';
	}
	g_log.last_frame_types = static_cast<int>(frameTypes);
	g_log.last_format = static_cast<int>(format);
	g_log.last_receive_flags = static_cast<int>(flags);

	auto *recv = new FakeReceive();
	recv->frame_types = frameTypes;
	recv->format = format;
	recv->flags = flags;
	return reinterpret_cast<omt_receive_t *>(recv);
}

void omt_receive_destroy(omt_receive_t *instance)
{
	auto *recv = reinterpret_cast<FakeReceive *>(instance);
	if (!recv)
		return;
	++g_log.receivers_destroyed;
	free(recv->video);
	free(recv->audio);
	delete recv;
}

OMTMediaFrame *omt_receive(omt_receive_t *instance, OMTFrameType, int)
{
	auto *recv = reinterpret_cast<FakeReceive *>(instance);
	if (!recv)
		return nullptr;

	const int step = recv->capture_count++;

	// The returned struct stays valid only until the next call for this instance, which is
	// exactly what the real library promises - so it is reused rather than reallocated.
	recv->frame = {};

	if (step % 4 == 2) {
		const int channels = 2;
		const int samples = 480;
		if (!recv->audio)
			recv->audio = static_cast<float *>(calloc(channels * samples, sizeof(float)));

		recv->frame.Type = OMTFrameType_Audio;
		recv->frame.Codec = OMTCodec_FPA1;
		recv->frame.SampleRate = 48000;
		recv->frame.Channels = channels;
		recv->frame.SamplesPerChannel = samples;
		recv->frame.Timestamp = 10000LL * (step + 1);
		recv->frame.Data = recv->audio;
		recv->frame.DataLength = channels * samples * static_cast<int>(sizeof(float));
		return &recv->frame;
	}

	if (!recv->video)
		recv->video = static_cast<unsigned char *>(calloc(1, static_cast<size_t>(kWidth) * kHeight * 4));

	recv->frame.Type = OMTFrameType_Video;
	recv->frame.Width = kWidth;
	recv->frame.Height = kHeight;
	recv->frame.FrameRateN = 60000;
	recv->frame.FrameRateD = 1001;
	recv->frame.AspectRatio = 16.0f / 9.0f;
	recv->frame.ColorSpace = OMTColorSpace_BT709;
	recv->frame.Timestamp = 10000LL * (step + 1);
	recv->frame.Data = recv->video;

	// Cycle the codecs the backend claims to decode, including the alpha-flag distinction
	// that decides BGRA from BGRX.
	switch (step % 4) {
	case 0:
		recv->frame.Codec = OMTCodec_UYVY;
		recv->frame.Stride = kWidth * 2;
		recv->frame.Flags = OMTVideoFlags_None;
		break;
	case 1:
		recv->frame.Codec = OMTCodec_BGRA;
		recv->frame.Stride = kWidth * 4;
		recv->frame.Flags = OMTVideoFlags_Alpha;
		break;
	default:
		recv->frame.Codec = OMTCodec_BGRA;
		recv->frame.Stride = kWidth * 4;
		recv->frame.Flags = OMTVideoFlags_None; // encoded as BGRX
		break;
	}

	recv->frame.DataLength = recv->frame.Stride * kHeight;
	return &recv->frame;
}

int omt_receive_send(omt_receive_t *, OMTMediaFrame *)
{
	return 0;
}

void omt_receive_settally(omt_receive_t *instance, OMTTally *tally)
{
	auto *recv = reinterpret_cast<FakeReceive *>(instance);
	if (!recv || !tally)
		return;
	recv->tally = *tally;
	g_log.last_recv_tally_program = tally->program;
	g_log.last_recv_tally_preview = tally->preview;
}

int omt_receive_gettally(omt_receive_t *, int, OMTTally *)
{
	return 0;
}

void omt_receive_setflags(omt_receive_t *, OMTReceiveFlags) {}

void omt_receive_setsuggestedquality(omt_receive_t *instance, OMTQuality quality)
{
	auto *recv = reinterpret_cast<FakeReceive *>(instance);
	if (recv)
		recv->suggested = quality;
	g_log.last_suggested_quality = static_cast<int>(quality);
}

void omt_receive_getsenderinformation(omt_receive_t *, OMTSenderInfo *) {}

void omt_receive_getvideostatistics(omt_receive_t *instance, OMTStatistics *stats)
{
	auto *recv = reinterpret_cast<FakeReceive *>(instance);
	if (!stats)
		return;

	*stats = {};
	stats->Frames = recv ? recv->capture_count : 0;
	stats->FramesSinceLast = 30;
	stats->FramesDropped = 7;
	stats->BytesReceived = 5000000;
	stats->BytesReceivedSinceLast = 1250000;
	stats->CodecTimeSinceLast = 4;
}

void omt_receive_getaudiostatistics(omt_receive_t *, OMTStatistics *stats)
{
	if (stats)
		*stats = {};
}

omt_send_t *omt_send_create(const char *name, OMTQuality quality)
{
	++g_log.senders_created;
	if (name) {
		strncpy(g_log.last_sender_name, name, sizeof(g_log.last_sender_name) - 1);
		g_log.last_sender_name[sizeof(g_log.last_sender_name) - 1] = '\0';
	}
	g_log.last_sender_quality = static_cast<int>(quality);
	return reinterpret_cast<omt_send_t *>(new FakeSend());
}

void omt_send_destroy(omt_send_t *instance)
{
	auto *send = reinterpret_cast<FakeSend *>(instance);
	if (!send)
		return;
	++g_log.senders_destroyed;
	delete send;
}

int omt_send(omt_send_t *, OMTMediaFrame *frame)
{
	if (!frame)
		return -1;

	if (frame->Type == OMTFrameType_Audio) {
		++g_log.audio_sent;
		g_log.last_sent_audio_channels = frame->Channels;
		g_log.last_sent_audio_samples = frame->SamplesPerChannel;
		g_log.last_sent_audio_rate = frame->SampleRate;
		g_log.last_sent_audio_length = frame->DataLength;
		return 0;
	}

	++g_log.video_sent;
	g_log.last_sent_codec = static_cast<int>(frame->Codec);
	g_log.last_sent_width = frame->Width;
	g_log.last_sent_height = frame->Height;
	g_log.last_sent_stride = frame->Stride;
	g_log.last_sent_flags = static_cast<int>(frame->Flags);
	g_log.last_sent_timestamp = static_cast<long long>(frame->Timestamp);
	return 0;
}

int omt_send_connections(omt_send_t *)
{
	return 3;
}

int omt_send_gettally(omt_send_t *, int, OMTTally *tally)
{
	if (tally) {
		tally->program = 1;
		tally->preview = 0;
	}
	return 0;
}

void omt_send_getvideostatistics(omt_send_t *, OMTStatistics *stats)
{
	if (!stats)
		return;

	*stats = {};
	stats->Frames = 120;
	stats->FramesSinceLast = 60;
	stats->FramesDropped = 2;
	stats->BytesSent = 8000000;
	stats->BytesSentSinceLast = 2500000;
	stats->CodecTimeSinceLast = 3;
}

void omt_send_getaudiostatistics(omt_send_t *, OMTStatistics *stats)
{
	if (stats)
		*stats = {};
}

void omt_send_setsenderinformation(omt_send_t *, OMTSenderInfo *) {}
void omt_send_addconnectionmetadata(omt_send_t *, const char *) {}
void omt_send_clearconnectionmetadata(omt_send_t *) {}
void omt_send_setredirect(omt_send_t *, const char *) {}

int omt_send_getaddress(omt_send_t *, char *address, int maxLength)
{
	const char *value = "FAKEHOST (Satellite Test)";
	if (address && maxLength > 0) {
		strncpy(address, value, static_cast<size_t>(maxLength) - 1);
		address[maxLength - 1] = '\0';
	}
	return static_cast<int>(strlen(value)) + 1;
}

void omt_setloggingfilename(const char *) {}
void omt_setloggingcallback(OMTLoggingCallback) {}

int omt_settings_get_string(const char *, char *value, int maxLength)
{
	if (value && maxLength > 0)
		value[0] = '\0';
	return 1;
}

void omt_settings_set_string(const char *name, const char *value)
{
	if (name && strcmp(name, "DiscoveryServer") == 0 && value) {
		strncpy(g_log.last_discovery_server, value, sizeof(g_log.last_discovery_server) - 1);
		g_log.last_discovery_server[sizeof(g_log.last_discovery_server) - 1] = '\0';
	}
}

int omt_settings_get_integer(const char *)
{
	return 0;
}

void omt_settings_set_integer(const char *name, int value)
{
	if (!name)
		return;
	if (strcmp(name, "NetworkPortStart") == 0)
		g_log.last_port_start = value;
	else if (strcmp(name, "NetworkPortEnd") == 0)
		g_log.last_port_end = value;
}

void omt_shutdown()
{
	++g_log.shutdown_calls;
}

/// Inspection hook for the test; not part of the libomt ABI.
const FakeOmtLog *fake_omt_log(void)
{
	return &g_log;
}

} // extern "C"

#pragma GCC visibility pop
