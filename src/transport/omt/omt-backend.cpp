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

#include "transport/omt/omt-backend.hpp"

#include "config/config.hpp"
#include "transport/library-loader.hpp"
#include "transport/omt/omt-api.hpp"

#include <chrono>
#include <thread>

#ifdef __linux__
#include <unistd.h>
#endif

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

namespace satellite {

std::vector<std::string> omt_runtime_candidates()
{
	std::vector<std::string> candidates;
	const std::string plugin_dir = plugin_binary_directory();

#ifdef _WIN32
	static const char *const kLibraryName = "libomt.dll";
	const char separator = '\\';
#elif defined(__APPLE__)
	static const char *const kLibraryName = "libomt.dylib";
	const char separator = '/';
#else
	static const char *const kLibraryName = "libomt.so";
	const char separator = '/';
#endif

	// A copy shipped next to the plugin binary takes precedence, so a packaged Satellite
	// uses the build it was tested against rather than whatever else is on the system.
	if (!plugin_dir.empty())
		candidates.push_back(plugin_dir + separator + kLibraryName);

	// Then a user-built or system-wide install.
	candidates.push_back(kLibraryName);
#ifndef _WIN32
	candidates.push_back(std::string("/usr/local/lib/") + kLibraryName);
	candidates.push_back(std::string("/usr/lib/") + kLibraryName);
#endif

	return candidates;
}

namespace {

#ifdef __linux__
/// Whether the Avahi daemon is reachable.
///
/// On Linux libomt does DNS-SD discovery through avahi-client, and avahi-client does not
/// report a missing daemon as an error - it fails an assertion and calls abort(), which in
/// OBS means the whole application dies. It is not catchable, and libomt starts discovery
/// lazily the first time a sender or receiver is created, so the abort would land in the
/// middle of a show rather than at load.
///
/// The daemon's client socket is what avahi-client connects to, so its presence is the same
/// question, asked safely. Verified the hard way: without a daemon,
/// `omt_send_create` aborts with "avahi_service_browser_new: Assertion `client' failed".
bool avahi_daemon_reachable()
{
	return access("/run/avahi-daemon/socket", F_OK) == 0 || access("/var/run/avahi-daemon/socket", F_OK) == 0;
}
#endif

/// OMT timestamps are 100ns units, the same as NDI's.
constexpr int64_t kOmtTicksPerNs = 100;

/// Tells an OMT sender to generate its own timestamps and throttle to the declared rate. We
/// pass real timestamps instead, so this is only the fallback.
constexpr int64_t kOmtSynthesizeTimestamp = -1;

OMTQuality quality_for(Quality quality)
{
	switch (quality) {
	case Quality::Low:
	case Quality::PreviewOnly:
		return OMTQuality_Low;
	case Quality::Medium:
		return OMTQuality_Medium;
	case Quality::High:
		return OMTQuality_High;
	case Quality::Default:
	case Quality::AudioOnly:
		break;
	}
	return OMTQuality_Default;
}

OMTFrameType frame_types_for(const ReceiverConfig &config)
{
	int types = OMTFrameType_Metadata;
	if (config.want_video && config.quality != Quality::AudioOnly)
		types |= OMTFrameType_Video;
	if (config.want_audio)
		types |= OMTFrameType_Audio;
	return static_cast<OMTFrameType>(types);
}

/// Maps a decoded OMT frame onto the matching OBS format.
///
/// Satellite asks for UYVYorBGRA, so this normally sees UYVY, and BGRA where the sender has
/// an alpha channel - both of which OBS takes with no pixel conversion. Requesting that pair
/// also keeps us away from P216/PA16 and UYVA, none of which OBS has a format for.
bool fill_video_frame(const OMTMediaFrame &source, VideoFrame &out)
{
	const uint32_t height = static_cast<uint32_t>(source.Height);
	const bool has_alpha = (source.Flags & OMTVideoFlags_Alpha) != 0;

	out.width = static_cast<uint32_t>(source.Width);
	out.height = height;
	out.data[0] = static_cast<uint8_t *>(source.Data);
	out.linesize[0] = static_cast<uint32_t>(source.Stride);
	out.has_alpha = has_alpha;

	switch (source.Codec) {
	case OMTCodec_UYVY:
		out.format = VIDEO_FORMAT_UYVY;
		out.has_alpha = false;
		break;

	case OMTCodec_BGRA:
		// The codec stays BGRA either way; the alpha flag is what says whether the fourth
		// channel carries anything, which is the difference between BGRA and BGRX in OBS.
		out.format = has_alpha ? VIDEO_FORMAT_BGRA : VIDEO_FORMAT_BGRX;
		break;

	case OMTCodec_YUY2:
		out.format = VIDEO_FORMAT_YUY2;
		out.has_alpha = false;
		break;

	case OMTCodec_NV12:
		out.format = VIDEO_FORMAT_NV12;
		out.has_alpha = false;
		out.data[1] = out.data[0] + static_cast<size_t>(source.Stride) * height;
		out.linesize[1] = static_cast<uint32_t>(source.Stride);
		break;

	case OMTCodec_YV12: {
		// Y, then V, then U - OBS wants Y, U, V, so the last two pointers are swapped.
		out.format = VIDEO_FORMAT_I420;
		out.has_alpha = false;

		uint8_t *const second = out.data[0] + static_cast<size_t>(source.Stride) * height;
		uint8_t *const third = second + static_cast<size_t>(source.Stride / 2) * (height / 2);
		out.data[1] = third;
		out.data[2] = second;
		out.linesize[1] = static_cast<uint32_t>(source.Stride / 2);
		out.linesize[2] = static_cast<uint32_t>(source.Stride / 2);
		break;
	}

	default:
		// UYVA, P216, PA16, or raw VMX1. We never request a format that decodes to these.
		return false;
	}

	const bool is_rgb = out.format == VIDEO_FORMAT_BGRA || out.format == VIDEO_FORMAT_BGRX;

	// OMT states the colour space outright, unlike NDI. Undefined carries the same
	// convention the header documents: BT.601 below 720 lines, BT.709 at or above.
	if (source.ColorSpace == OMTColorSpace_BT601)
		out.colorspace = VIDEO_CS_601;
	else if (source.ColorSpace == OMTColorSpace_BT709)
		out.colorspace = VIDEO_CS_709;
	else
		out.colorspace = height >= 720 ? VIDEO_CS_709 : VIDEO_CS_601;

	if (is_rgb) {
		out.colorspace = VIDEO_CS_DEFAULT;
		out.range = VIDEO_RANGE_FULL;
	} else {
		out.range = VIDEO_RANGE_PARTIAL;
	}

	out.framerate_num = static_cast<uint32_t>(source.FrameRateN);
	out.framerate_den = static_cast<uint32_t>(source.FrameRateD);
	out.interlaced = (source.Flags & OMTVideoFlags_Interlaced) != 0;
	out.timestamp_ns = source.Timestamp > 0 ? static_cast<uint64_t>(source.Timestamp) * kOmtTicksPerNs : 0u;

	return true;
}

bool fill_audio_frame(const OMTMediaFrame &source, AudioFrame &out)
{
	if (source.Codec != OMTCodec_FPA1 || source.Channels <= 0 || source.SamplesPerChannel <= 0)
		return false;

	const uint32_t channels = static_cast<uint32_t>(source.Channels) > MAX_AUDIO_CHANNELS
					  ? MAX_AUDIO_CHANNELS
					  : static_cast<uint32_t>(source.Channels);

	out.channels = channels;
	out.frames = static_cast<uint32_t>(source.SamplesPerChannel);
	out.sample_rate = static_cast<uint32_t>(source.SampleRate);
	out.timestamp_ns = source.Timestamp > 0 ? static_cast<uint64_t>(source.Timestamp) * kOmtTicksPerNs : 0u;

	// FPA1 is 32-bit planar float laid out back to back, which is OBS's own
	// AUDIO_FORMAT_FLOAT_PLANAR - so the planes are addressed, not converted.
	const size_t plane_bytes = static_cast<size_t>(source.SamplesPerChannel) * sizeof(float);
	auto *base = static_cast<uint8_t *>(source.Data);
	for (uint32_t channel = 0; channel < channels; ++channel)
		out.data[channel] = base + plane_bytes * channel;

	return true;
}

/// Turns OMT's cumulative counters into the per-interval figures the dock shows.
///
/// Unlike NDI, OMT reports real byte counts, so the bitrate here is exact rather than
/// measured by us - which is why FeedStats::bitrate_estimated stays false.
FeedStats stats_from(const OMTStatistics &stats, uint64_t &last_sample_ns)
{
	FeedStats out;

	const uint64_t now = os_gettime_ns();
	const uint64_t elapsed_ns = last_sample_ns ? now - last_sample_ns : 0;
	last_sample_ns = now;

	const int64_t bytes = stats.BytesSentSinceLast + stats.BytesReceivedSinceLast;
	if (elapsed_ns > 0) {
		const double seconds = static_cast<double>(elapsed_ns) / 1e9;
		out.bitrate_mbps = static_cast<double>(bytes) * 8.0 / seconds / 1e6;
		out.fps = static_cast<double>(stats.FramesSinceLast) / seconds;
	}

	out.bitrate_estimated = false;
	out.frames = stats.Frames;
	out.frames_dropped = stats.FramesDropped;
	out.codec_ms = static_cast<double>(stats.CodecTimeSinceLast);

	return out;
}

class OmtReceiver final : public IReceiver {
public:
	OmtReceiver(const OmtApi *api, omt_receive_t *instance, OMTFrameType frame_types)
		: api_(api),
		  instance_(instance),
		  frame_types_(frame_types)
	{
	}

	~OmtReceiver() override
	{
		if (instance_)
			api_->receive_destroy(instance_);
	}

	CapturedFrame capture(int timeout_ms) override
	{
		OMTMediaFrame *frame = api_->receive(instance_, frame_types_, timeout_ms);
		if (!frame)
			return {};

		switch (frame->Type) {
		case OMTFrameType_Video: {
			CapturedFrame captured(this, FrameType::Video);
			if (!fill_video_frame(*frame, captured.video))
				obs_log(LOG_WARNING, "unsupported OMT codec 0x%08x",
					static_cast<unsigned>(frame->Codec));
			connected_ = true;
			return captured;
		}

		case OMTFrameType_Audio: {
			CapturedFrame captured(this, FrameType::Audio);
			fill_audio_frame(*frame, captured.audio);
			connected_ = true;
			return captured;
		}

		case OMTFrameType_Metadata: {
			CapturedFrame captured(this, FrameType::Metadata);
			if (frame->Data && frame->DataLength > 0)
				captured.metadata.assign(static_cast<const char *>(frame->Data));
			return captured;
		}

		default:
			return {};
		}
	}

	void set_tally(const Tally &tally) override
	{
		OMTTally omt_tally = {};
		omt_tally.program = tally.program ? 1 : 0;
		omt_tally.preview = tally.preview ? 1 : 0;
		api_->receive_settally(instance_, &omt_tally);
	}

	bool connected() const override { return connected_; }

	FeedStats stats() const override
	{
		OMTStatistics raw = {};
		api_->receive_getvideostatistics(instance_, &raw);

		FeedStats stats = stats_from(raw, last_sample_ns_);
		stats.connections = -1;
		return stats;
	}

protected:
	/// Nothing to do: OMT owns the frame and keeps it valid until the next omt_receive for
	/// this instance and frame type, which is precisely the borrow CapturedFrame models.
	void release_frame(FrameType) override {}

private:
	const OmtApi *api_ = nullptr;
	omt_receive_t *instance_ = nullptr;
	OMTFrameType frame_types_ = OMTFrameType_None;
	bool connected_ = false;
	mutable uint64_t last_sample_ns_ = 0;
};

class OmtSender final : public ISender {
public:
	OmtSender(const OmtApi *api, omt_send_t *instance, OMTQuality quality)
		: api_(api),
		  instance_(instance),
		  quality_(quality)
	{
	}

	~OmtSender() override
	{
		if (instance_)
			api_->send_destroy(instance_);
	}

	bool send_video(const VideoFrame &frame) override
	{
		OMTMediaFrame omt_frame = {};

		omt_frame.Type = OMTFrameType_Video;
		omt_frame.Timestamp = frame.timestamp_ns > 0 ? static_cast<int64_t>(frame.timestamp_ns) / kOmtTicksPerNs
							     : kOmtSynthesizeTimestamp;

		switch (frame.format) {
		case VIDEO_FORMAT_UYVY:
			omt_frame.Codec = OMTCodec_UYVY;
			break;
		case VIDEO_FORMAT_YUY2:
			omt_frame.Codec = OMTCodec_YUY2;
			break;
		case VIDEO_FORMAT_BGRA:
		case VIDEO_FORMAT_BGRX:
			omt_frame.Codec = OMTCodec_BGRA;
			break;
		case VIDEO_FORMAT_NV12:
			omt_frame.Codec = OMTCodec_NV12;
			break;
		case VIDEO_FORMAT_I420:
			omt_frame.Codec = OMTCodec_YV12;
			break;
		default:
			obs_log(LOG_WARNING, "cannot send OBS video format %d over OMT",
				static_cast<int>(frame.format));
			return false;
		}

		omt_frame.Width = static_cast<int>(frame.width);
		omt_frame.Height = static_cast<int>(frame.height);
		omt_frame.Stride = static_cast<int>(frame.linesize[0]);
		omt_frame.FrameRateN = static_cast<int>(frame.framerate_num);
		omt_frame.FrameRateD = static_cast<int>(frame.framerate_den);
		omt_frame.AspectRatio =
			frame.height ? static_cast<float>(frame.width) / static_cast<float>(frame.height) : 0.0f;

		int flags = OMTVideoFlags_None;
		if (frame.interlaced)
			flags |= OMTVideoFlags_Interlaced;

		// Without this flag OMT encodes BGRA as BGRX and discards the alpha channel, so
		// an opaque frame is exactly the case where we must not set it.
		if (frame.has_alpha)
			flags |= OMTVideoFlags_Alpha;
		omt_frame.Flags = static_cast<OMTVideoFlags>(flags);

		omt_frame.ColorSpace = frame.colorspace == VIDEO_CS_601 ? OMTColorSpace_BT601 : OMTColorSpace_BT709;

		omt_frame.Data = frame.data[0];
		omt_frame.DataLength = static_cast<int>(frame.linesize[0] * frame.height);

		return api_->send(instance_, &omt_frame) >= 0;
	}

	bool send_audio(const AudioFrame &frame) override
	{
		if (frame.channels == 0 || frame.frames == 0 || !frame.data[0])
			return false;

		OMTMediaFrame omt_frame = {};

		omt_frame.Type = OMTFrameType_Audio;
		omt_frame.Timestamp = frame.timestamp_ns > 0 ? static_cast<int64_t>(frame.timestamp_ns) / kOmtTicksPerNs
							     : kOmtSynthesizeTimestamp;

		// FPA1 is the only audio codec OMT accepts, and it is OBS's own planar float.
		omt_frame.Codec = OMTCodec_FPA1;
		omt_frame.SampleRate = static_cast<int>(frame.sample_rate);
		omt_frame.Channels = static_cast<int>(frame.channels);
		omt_frame.SamplesPerChannel = static_cast<int>(frame.frames);

		// SenderSession packs the planes contiguously, which is the layout OMT expects.
		omt_frame.Data = frame.data[0];
		omt_frame.DataLength = static_cast<int>(frame.frames * frame.channels * sizeof(float));

		return api_->send(instance_, &omt_frame) >= 0;
	}

	Tally tally() const override
	{
		OMTTally omt_tally = {};

		// Zero timeout: polled from the send thread between frames, so it must not block.
		api_->send_gettally(instance_, 0, &omt_tally);

		Tally tally;
		tally.program = omt_tally.program != 0;
		tally.preview = omt_tally.preview != 0;
		return tally;
	}

	int connections() const override { return api_->send_connections(instance_); }

	FeedStats stats() const override
	{
		OMTStatistics raw = {};
		api_->send_getvideostatistics(instance_, &raw);

		FeedStats stats = stats_from(raw, last_sample_ns_);
		stats.connections = connections();
		return stats;
	}

private:
	const OmtApi *api_ = nullptr;
	omt_send_t *instance_ = nullptr;
	OMTQuality quality_ = OMTQuality_Default;
	mutable uint64_t last_sample_ns_ = 0;
};

class OmtBackend final : public IBackend {
public:
	~OmtBackend() override { unload(); }

	Protocol protocol() const override { return Protocol::OMT; }

	bool load() override
	{
		if (available_)
			return true;

		if (!library_.open(omt_runtime_candidates())) {
			unavailable_reason_ = "libomt not found. See the Satellite documentation for "
					      "how to install or build the OMT libraries.";
			return false;
		}

#ifdef __linux__
		// Only DNS-SD discovery goes through Avahi. With a discovery server configured
		// libomt talks to that over TCP instead, so the daemon is not needed and refusing
		// to load would be wrong.
		if (Config::instance().omt_discovery_server.empty() && !avahi_daemon_reachable()) {
			unavailable_reason_ = "the Avahi daemon is not running. libomt discovers sources "
					      "through Avahi on Linux and aborts the process if it cannot "
					      "reach the daemon, so OMT is disabled rather than risk taking "
					      "OBS down. Start avahi-daemon, or set an OMT discovery server.";
			library_.close();
			return false;
		}
#endif

		std::string missing;
		if (!api_.bind(library_, missing)) {
			unavailable_reason_ = "found " + library_.path() + ", but it does not export " + missing +
					      ". It may not be libomt, or may be too old.";
			library_.close();
			return false;
		}

		apply_settings();

		unavailable_reason_.clear();
		runtime_version_ = library_.path();
		available_ = true;
		return true;
	}

	void unload() override
	{
		if (available_ && api_.shutdown) {
			// Stops libomt's own logging and discovery threads. It has to run before the
			// library handle goes away, or those threads outlive the code they are in.
			api_.shutdown();
		}

		api_ = {};
		library_.close();
		available_ = false;
	}

	bool available() const override { return available_; }
	std::string unavailable_reason() const override { return unavailable_reason_; }
	std::string runtime_version() const override { return runtime_version_; }

	/// Empty: there is nothing for a user to go and install from a vendor page.
	std::string install_url() const override { return {}; }

	bool wait_for_sources(int timeout_ms) override
	{
		// libomt runs its own discovery thread and omt_discovery_getaddresses is a
		// snapshot, so there is nothing to block on. Sleeping keeps this backend on the
		// shared discovery cadence instead of spinning.
		if (timeout_ms > 0)
			std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
		return available_;
	}

	std::vector<SourceRef> poll_sources() override
	{
		if (!available_)
			return {};

		int count = 0;
		char **addresses = api_.discovery_getaddresses(&count);
		if (!addresses || count <= 0)
			return {};

		// This array is only valid until the next call, so every string is copied out here
		// and nothing is retained. That contract is also why the discovery thread is the
		// single caller - see docs/ARCHITECTURE.md section 6.
		std::vector<SourceRef> found;
		found.reserve(static_cast<size_t>(count));
		for (int index = 0; index < count; ++index) {
			if (!addresses[index] || !*addresses[index])
				continue;

			SourceRef ref;
			ref.protocol = Protocol::OMT;
			ref.name = addresses[index];
			ref.address = addresses[index];
			found.push_back(std::move(ref));
		}

		return found;
	}

	std::unique_ptr<IReceiver> create_receiver(const ReceiverConfig &config) override
	{
		if (!available_ || config.address.empty())
			return nullptr;

		const OMTFrameType frame_types = frame_types_for(config);

		// UYVY when the sender is opaque, BGRA when it has alpha - the same pair the NDI
		// backend asks for, and for the same reason.
		const OMTPreferredVideoFormat format = OMTPreferredVideoFormat_UYVYorBGRA;

		// A preview feed is a 1/8th-size frame, which is what backs the shared low
		// bandwidth mode.
		const OMTReceiveFlags flags = config.quality == Quality::PreviewOnly ? OMTReceiveFlags_Preview
										     : OMTReceiveFlags_None;

		omt_receive_t *instance = api_.receive_create(config.address.c_str(), frame_types, format, flags);
		if (!instance) {
			obs_log(LOG_WARNING, "could not create an OMT receiver for '%s'", config.address.c_str());
			return nullptr;
		}

		// Receivers vote on the sender's encoding quality; Default defers to whatever the
		// other receivers have asked for.
		api_.receive_setsuggestedquality(instance, quality_for(config.quality));

		return std::make_unique<OmtReceiver>(&api_, instance, frame_types);
	}

	std::unique_ptr<ISender> create_sender(const SenderConfig &config) override
	{
		if (!available_ || config.name.empty())
			return nullptr;

		const OMTQuality quality = quality_for(config.quality);

		omt_send_t *instance = api_.send_create(config.name.c_str(), quality);
		if (!instance) {
			obs_log(LOG_WARNING, "could not create an OMT sender named '%s'", config.name.c_str());
			return nullptr;
		}

		return std::make_unique<OmtSender>(&api_, instance, quality);
	}

private:
	void apply_settings()
	{
		const Config &config = Config::instance();

		// Empty restores the default DNS-SD behaviour, which is what a blank field means.
		api_.settings_set_string("DiscoveryServer", config.omt_discovery_server.c_str());

		if (config.omt_port_start > 0)
			api_.settings_set_integer("NetworkPortStart", config.omt_port_start);
		if (config.omt_port_end > 0)
			api_.settings_set_integer("NetworkPortEnd", config.omt_port_end);
	}

	LibraryHandle library_;
	OmtApi api_;
	bool available_ = false;
	std::string runtime_version_;
	std::string unavailable_reason_ = "not loaded";
};

} // namespace

std::unique_ptr<IBackend> create_omt_backend()
{
	return std::make_unique<OmtBackend>();
}

} // namespace satellite
