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

#include "transport/ndi/ndi-backend.hpp"

#include "config/config.hpp"
#include "transport/library-loader.hpp"
#include "transport/rate-meter.hpp"

#include <Processing.NDI.Lib.h>

#include <obs-module.h>
#include <plugin-support.h>

namespace satellite {

const char *ndi_install_url()
{
	return NDILIB_REDIST_URL;
}

std::vector<std::string> ndi_runtime_candidates()
{
	std::vector<std::string> candidates;

	// NDILIB_REDIST_FOLDER is the environment variable the v6 runtime installer sets, and
	// NDILIB_LIBRARY_NAME is the platform's library name. Both come from the SDK headers, so
	// they track whichever SDK version is vendored in lib/ndi rather than being hard-coded.
	const std::string redist = environment_variable(NDILIB_REDIST_FOLDER);
	if (!redist.empty()) {
#ifdef _WIN32
		candidates.push_back(redist + "\\" + NDILIB_LIBRARY_NAME);
#else
		candidates.push_back(redist + "/" + NDILIB_LIBRARY_NAME);
#endif
	}

	// Then the default search path, then the usual install locations.
	candidates.push_back(NDILIB_LIBRARY_NAME);

#ifdef __APPLE__
	candidates.push_back("/usr/local/lib/libndi.dylib");
	candidates.push_back("/Library/NDI SDK for Apple/lib/macOS/libndi.dylib");
#elif !defined(_WIN32)
	candidates.push_back("/usr/local/lib/libndi.so.6");
	candidates.push_back("/usr/lib/libndi.so.6");
#endif

	return candidates;
}

namespace {

/// Satellite targets the NDI 6 runtime only, so this is the single entry point we bind.
constexpr const char *kLoadSymbol = "NDIlib_v6_load";
using NdiLoadFn = const NDIlib_v6 *(*)(void);

NDIlib_recv_bandwidth_e bandwidth_for(Quality quality)
{
	switch (quality) {
	case Quality::AudioOnly:
		return NDIlib_recv_bandwidth_audio_only;
	case Quality::PreviewOnly:
	case Quality::Low:
		return NDIlib_recv_bandwidth_lowest;
	case Quality::Default:
	case Quality::Medium:
	case Quality::High:
		break;
	}
	return NDIlib_recv_bandwidth_highest;
}

/// Maps an NDI FourCC onto the matching OBS format and fills in the plane pointers.
///
/// Satellite asks for UYVY_BGRA, so in practice this sees UYVY for opaque feeds and BGRA
/// when the sender has an alpha channel - both of which OBS takes directly, with no pixel
/// conversion on the common path. The remaining cases are handled defensively because a
/// sender is free to hand us something else.
bool fill_video_frame(const NDIlib_video_frame_v2_t &source, VideoFrame &out)
{
	const int stride = source.line_stride_in_bytes;
	const uint32_t height = static_cast<uint32_t>(source.yres);

	out.width = static_cast<uint32_t>(source.xres);
	out.height = height;
	out.data[0] = source.p_data;
	out.linesize[0] = static_cast<uint32_t>(stride);

	switch (source.FourCC) {
	case NDIlib_FourCC_video_type_UYVY:
		out.format = VIDEO_FORMAT_UYVY;
		out.has_alpha = false;
		break;

	case NDIlib_FourCC_video_type_BGRA:
		out.format = VIDEO_FORMAT_BGRA;
		out.has_alpha = true;
		break;

	case NDIlib_FourCC_video_type_BGRX:
		out.format = VIDEO_FORMAT_BGRX;
		out.has_alpha = false;
		break;

	case NDIlib_FourCC_video_type_RGBA:
	case NDIlib_FourCC_video_type_RGBX:
		// OBS has no RGBX, and an opaque RGBA renders identically.
		out.format = VIDEO_FORMAT_RGBA;
		out.has_alpha = source.FourCC == NDIlib_FourCC_video_type_RGBA;
		break;

	case NDIlib_FourCC_video_type_NV12:
		out.format = VIDEO_FORMAT_NV12;
		out.has_alpha = false;
		out.data[1] = source.p_data + static_cast<size_t>(stride) * height;
		out.linesize[1] = static_cast<uint32_t>(stride);
		break;

	case NDIlib_FourCC_video_type_I420:
	case NDIlib_FourCC_video_type_YV12: {
		out.format = VIDEO_FORMAT_I420;
		out.has_alpha = false;

		uint8_t *const second = source.p_data + static_cast<size_t>(stride) * height;
		uint8_t *const third = second + static_cast<size_t>(stride / 2) * (height / 2);

		// I420 orders the planes Y, U, V and YV12 orders them Y, V, U. OBS wants Y, U, V,
		// so YV12 is the same memory with the last two pointers swapped - still no copy.
		const bool is_yv12 = source.FourCC == NDIlib_FourCC_video_type_YV12;
		out.data[1] = is_yv12 ? third : second;
		out.data[2] = is_yv12 ? second : third;
		out.linesize[1] = static_cast<uint32_t>(stride / 2);
		out.linesize[2] = static_cast<uint32_t>(stride / 2);
		break;
	}

	default:
		// P216, PA16 and the compressed types. We never request a colour format that
		// produces these, so reaching here means a sender ignored the request.
		return false;
	}

	const bool is_rgb = out.format == VIDEO_FORMAT_BGRA || out.format == VIDEO_FORMAT_BGRX ||
			    out.format == VIDEO_FORMAT_RGBA;

	// NDI follows the usual convention: BT.601 below 720 lines, BT.709 at or above it.
	out.colorspace = is_rgb ? VIDEO_CS_DEFAULT : (height >= 720 ? VIDEO_CS_709 : VIDEO_CS_601);
	out.range = is_rgb ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;

	out.framerate_num = static_cast<uint32_t>(source.frame_rate_N);
	out.framerate_den = static_cast<uint32_t>(source.frame_rate_D);
	out.interlaced = source.frame_format_type != NDIlib_frame_format_type_progressive;

	// NDI timestamps are 100ns units; OBS wants nanoseconds. timestamp is present from
	// v2.5 and is the more accurate of the two, so prefer it and fall back to timecode.
	const int64_t ticks = source.timestamp != NDIlib_send_timecode_synthesize ? source.timestamp : source.timecode;
	out.timestamp_ns = ticks > 0 ? static_cast<uint64_t>(ticks) * 100u : 0u;

	return true;
}

size_t video_frame_bytes(const NDIlib_video_frame_v2_t &frame)
{
	return static_cast<size_t>(frame.line_stride_in_bytes) * static_cast<size_t>(frame.yres);
}

/// The reverse of fill_video_frame, for the send path.
///
/// Senders ask OBS to convert to UYVY before the frame ever reaches us, so the first case is
/// the one that runs; the rest exist so an alpha-bearing or unconverted frame is not silently
/// dropped.
bool fourcc_for_format(video_format format, NDIlib_FourCC_video_type_e &out)
{
	switch (format) {
	case VIDEO_FORMAT_UYVY:
		out = NDIlib_FourCC_video_type_UYVY;
		return true;
	case VIDEO_FORMAT_BGRA:
		out = NDIlib_FourCC_video_type_BGRA;
		return true;
	case VIDEO_FORMAT_BGRX:
		out = NDIlib_FourCC_video_type_BGRX;
		return true;
	case VIDEO_FORMAT_RGBA:
		out = NDIlib_FourCC_video_type_RGBA;
		return true;
	case VIDEO_FORMAT_NV12:
		out = NDIlib_FourCC_video_type_NV12;
		return true;
	case VIDEO_FORMAT_I420:
		out = NDIlib_FourCC_video_type_I420;
		return true;
	default:
		return false;
	}
}

class NdiSender final : public ISender {
public:
	NdiSender(const NDIlib_v6 *ndi, NDIlib_send_instance_t instance) : ndi_(ndi), instance_(instance) {}

	~NdiSender() override
	{
		if (instance_)
			ndi_->send_destroy(instance_);
	}

	bool send_video(const VideoFrame &frame) override
	{
		NDIlib_FourCC_video_type_e fourcc;
		if (!fourcc_for_format(frame.format, fourcc)) {
			obs_log(LOG_WARNING, "cannot send OBS video format %d over NDI",
				static_cast<int>(frame.format));
			return false;
		}

		NDIlib_video_frame_v2_t ndi_frame = {};
		ndi_frame.xres = static_cast<int>(frame.width);
		ndi_frame.yres = static_cast<int>(frame.height);
		ndi_frame.FourCC = fourcc;
		ndi_frame.frame_rate_N = static_cast<int>(frame.framerate_num);
		ndi_frame.frame_rate_D = static_cast<int>(frame.framerate_den);
		ndi_frame.picture_aspect_ratio = 0.0f; // 0 means "derive from resolution".
		ndi_frame.frame_format_type = frame.interlaced ? NDIlib_frame_format_type_interleaved
							       : NDIlib_frame_format_type_progressive;
		ndi_frame.p_data = frame.data[0];
		ndi_frame.line_stride_in_bytes = static_cast<int>(frame.linesize[0]);

		// OBS timestamps are nanoseconds and NDI wants 100ns units. Handing NDI the real
		// timestamp keeps a receiver's A/V sync tied to OBS's own clock; synthesize is the
		// fallback that lets NDI stamp it instead.
		ndi_frame.timecode = frame.timestamp_ns > 0 ? static_cast<int64_t>(frame.timestamp_ns / 100)
							    : NDIlib_send_timecode_synthesize;

		// Synchronous rather than send_send_video_async_v2: the async variant requires the
		// buffer to stay valid until the *next* async call, and this already runs on a
		// dedicated send thread where blocking costs nothing. Worth revisiting with double
		// buffering if profiling ever says so.
		ndi_->send_send_video_v2(instance_, &ndi_frame);

		rate_.add_frame(video_frame_bytes(ndi_frame));
		return true;
	}

	bool send_audio(const AudioFrame &frame) override
	{
		if (frame.channels == 0 || frame.frames == 0)
			return false;

		NDIlib_audio_frame_v3_t ndi_frame = {};
		ndi_frame.sample_rate = static_cast<int>(frame.sample_rate);
		ndi_frame.no_channels = static_cast<int>(frame.channels);
		ndi_frame.no_samples = static_cast<int>(frame.frames);
		ndi_frame.FourCC = NDIlib_FourCC_audio_type_FLTP;
		ndi_frame.p_data = frame.data[0];

		// OBS planar float is already NDI's FLTP layout, so the planes go out untouched.
		// They are contiguous in the queue buffer, which is what channel_stride describes.
		ndi_frame.channel_stride_in_bytes = static_cast<int>(frame.frames * sizeof(float));

		ndi_frame.timecode = frame.timestamp_ns > 0 ? static_cast<int64_t>(frame.timestamp_ns / 100)
							    : NDIlib_send_timecode_synthesize;

		ndi_->send_send_audio_v3(instance_, &ndi_frame);
		return true;
	}

	Tally tally() const override
	{
		NDIlib_tally_t ndi_tally = {};

		// Zero timeout: this is polled from the send thread between frames, so it must not
		// block waiting for a downstream receiver to say something.
		ndi_->send_get_tally(instance_, &ndi_tally, 0);

		Tally tally;
		tally.program = ndi_tally.on_program;
		tally.preview = ndi_tally.on_preview;
		return tally;
	}

	int connections() const override { return ndi_->send_get_no_connections(instance_, 0); }

	FeedStats stats() const override
	{
		FeedStats stats;

		// NDI reports nothing about what a sender has sent, so everything here is measured
		// locally and flagged, exactly as on the receive side.
		stats.bitrate_mbps = rate_.mbps();
		stats.bitrate_estimated = true;
		stats.fps = rate_.fps();
		stats.connections = connections();
		return stats;
	}

private:
	const NDIlib_v6 *ndi_ = nullptr;
	NDIlib_send_instance_t instance_ = nullptr;
	RateMeter rate_;
};

class NdiReceiver final : public IReceiver {
public:
	NdiReceiver(const NDIlib_v6 *ndi, NDIlib_recv_instance_t instance) : ndi_(ndi), instance_(instance) {}

	~NdiReceiver() override
	{
		if (instance_)
			ndi_->recv_destroy(instance_);
	}

	CapturedFrame capture(int timeout_ms) override
	{
		video_ = {};
		audio_ = {};
		metadata_ = {};

		const NDIlib_frame_type_e type = ndi_->recv_capture_v3(instance_, &video_, &audio_, &metadata_,
								       static_cast<uint32_t>(timeout_ms));

		switch (type) {
		case NDIlib_frame_type_video: {
			CapturedFrame frame(this, FrameType::Video);
			if (!fill_video_frame(video_, frame.video)) {
				// Unusable pixel format. Returning an empty frame here would leak the
				// NDI allocation, so hand back a typed frame whose destructor frees it
				// and let the caller skip it.
				obs_log(LOG_WARNING, "unsupported NDI pixel format 0x%08x",
					static_cast<unsigned>(video_.FourCC));
				return frame;
			}

			rate_.add_frame(video_frame_bytes(video_));
			return frame;
		}

		case NDIlib_frame_type_audio: {
			CapturedFrame frame(this, FrameType::Audio);

			// NDI audio v3 is FLTP: 32-bit planar float, which is exactly OBS's internal
			// AUDIO_FORMAT_FLOAT_PLANAR, so this is pointer arithmetic rather than a
			// conversion.
			if (audio_.FourCC != NDIlib_FourCC_audio_type_FLTP)
				return frame;

			const uint32_t channels = static_cast<uint32_t>(audio_.no_channels) > MAX_AUDIO_CHANNELS
							  ? MAX_AUDIO_CHANNELS
							  : static_cast<uint32_t>(audio_.no_channels);

			frame.audio.channels = channels;
			frame.audio.frames = static_cast<uint32_t>(audio_.no_samples);
			frame.audio.sample_rate = static_cast<uint32_t>(audio_.sample_rate);
			for (uint32_t channel = 0; channel < channels; ++channel)
				frame.audio.data[channel] =
					audio_.p_data + static_cast<size_t>(audio_.channel_stride_in_bytes) * channel;

			const int64_t ticks = audio_.timestamp != NDIlib_send_timecode_synthesize ? audio_.timestamp
												  : audio_.timecode;
			frame.audio.timestamp_ns = ticks > 0 ? static_cast<uint64_t>(ticks) * 100u : 0u;

			return frame;
		}

		case NDIlib_frame_type_metadata: {
			CapturedFrame frame(this, FrameType::Metadata);
			if (metadata_.p_data)
				frame.metadata.assign(metadata_.p_data);
			return frame;
		}

		default:
			// Timeout, status change, or error. Nothing was allocated, so nothing to free.
			return {};
		}
	}

	void set_tally(const Tally &tally) override
	{
		NDIlib_tally_t ndi_tally = {};
		ndi_tally.on_program = tally.program;
		ndi_tally.on_preview = tally.preview;
		ndi_->recv_set_tally(instance_, &ndi_tally);
	}

	bool connected() const override { return ndi_->recv_get_no_connections(instance_) > 0; }

	FeedStats stats() const override
	{
		NDIlib_recv_performance_t total = {};
		NDIlib_recv_performance_t dropped = {};
		ndi_->recv_get_performance(instance_, &total, &dropped);

		FeedStats stats;
		stats.frames = total.video_frames;
		stats.frames_dropped = dropped.video_frames;
		stats.connections = -1;

		// NDI has no byte counters at all, so this is measured from frame sizes and flagged
		// so the dock does not present it as an exact figure the way it does OMT's.
		stats.bitrate_mbps = rate_.mbps();
		stats.bitrate_estimated = true;
		stats.fps = rate_.fps();

		return stats;
	}

protected:
	void release_frame(FrameType type) override
	{
		switch (type) {
		case FrameType::Video:
			ndi_->recv_free_video_v2(instance_, &video_);
			break;
		case FrameType::Audio:
			ndi_->recv_free_audio_v3(instance_, &audio_);
			break;
		case FrameType::Metadata:
			ndi_->recv_free_metadata(instance_, &metadata_);
			break;
		case FrameType::None:
			break;
		}
	}

private:
	const NDIlib_v6 *ndi_ = nullptr;
	NDIlib_recv_instance_t instance_ = nullptr;

	// Owned by the receiver rather than by CapturedFrame: NDI frees by pointer to the
	// original struct, so the struct has to outlive the borrow.
	NDIlib_video_frame_v2_t video_ = {};
	NDIlib_audio_frame_v3_t audio_ = {};
	NDIlib_metadata_frame_t metadata_ = {};

	RateMeter rate_;
};

class NdiBackend final : public IBackend {
public:
	~NdiBackend() override { unload(); }

	Protocol protocol() const override { return Protocol::NDI; }

	bool load() override
	{
		if (available_)
			return true;

		if (!library_.open(ndi_runtime_candidates())) {
			unavailable_reason_ = "NDI runtime not found. Install the NDI 6 runtime to "
					      "enable NDI sources and outputs.";
			return false;
		}

		NdiLoadFn load_fn = nullptr;
		if (!library_.bind(load_fn, kLoadSymbol)) {
			unavailable_reason_ = std::string("found ") + library_.path() + ", but it does not export " +
					      kLoadSymbol + ". An NDI 6 runtime is required.";
			library_.close();
			return false;
		}

		ndi_ = load_fn();
		if (!ndi_) {
			unavailable_reason_ = std::string(kLoadSymbol) + " failed in " + library_.path() + ".";
			library_.close();
			return false;
		}

		// The runtime uses SSE4.2 and refuses to run without it, so this is a real check
		// rather than a formality on older hardware.
		if (!ndi_->is_supported_CPU()) {
			unavailable_reason_ = "this CPU does not meet the NDI runtime's requirements.";
			ndi_ = nullptr;
			library_.close();
			return false;
		}

		if (!ndi_->initialize()) {
			unavailable_reason_ = "the NDI runtime failed to initialize.";
			ndi_ = nullptr;
			library_.close();
			return false;
		}

		if (const char *version = ndi_->version())
			runtime_version_ = version;

		if (!create_finder()) {
			unavailable_reason_ = "could not create the NDI discovery finder.";
			ndi_->destroy();
			ndi_ = nullptr;
			library_.close();
			return false;
		}

		unavailable_reason_.clear();
		available_ = true;
		return true;
	}

	void unload() override
	{
		if (finder_) {
			ndi_->find_destroy(finder_);
			finder_ = nullptr;
		}

		if (ndi_) {
			ndi_->destroy();
			ndi_ = nullptr;
		}

		library_.close();
		available_ = false;
	}

	bool available() const override { return available_; }
	std::string unavailable_reason() const override { return unavailable_reason_; }
	std::string runtime_version() const override { return runtime_version_; }
	std::string install_url() const override { return ndi_install_url(); }

	bool wait_for_sources(int timeout_ms) override
	{
		if (!available_ || !finder_)
			return false;

		// Blocks inside the SDK and returns as soon as the source list changes. The finder
		// is created once in load() and lives until unload() - it is deliberately not torn
		// down between scans. See docs/ARCHITECTURE.md section 2.1.
		return ndi_->find_wait_for_sources(finder_, static_cast<uint32_t>(timeout_ms));
	}

	std::vector<SourceRef> poll_sources() override
	{
		if (!available_ || !finder_)
			return {};

		uint32_t count = 0;
		const NDIlib_source_t *sources = ndi_->find_get_current_sources(finder_, &count);
		if (!sources)
			return {};

		// The returned array belongs to the finder and is only valid until the next call,
		// so everything is copied out here.
		std::vector<SourceRef> found;
		found.reserve(count);
		for (uint32_t index = 0; index < count; ++index) {
			const char *name = sources[index].p_ndi_name;
			if (!name || !*name)
				continue;

			SourceRef ref;
			ref.protocol = Protocol::NDI;
			ref.name = name;
			ref.address = name;
			found.push_back(std::move(ref));
		}

		return found;
	}

	std::unique_ptr<IReceiver> create_receiver(const ReceiverConfig &config) override
	{
		if (!available_ || config.address.empty())
			return nullptr;

		NDIlib_source_t source = {};
		source.p_ndi_name = config.address.c_str();

		NDIlib_recv_create_v3_t settings = {};
		settings.source_to_connect_to = source;
		settings.bandwidth = bandwidth_for(config.quality);
		settings.allow_video_fields = true;

		// UYVY when the sender is opaque, BGRA when it has alpha. Both land in OBS without
		// a pixel conversion, and asking for this pair keeps us away from P216/PA16, which
		// OBS has no format for.
		settings.color_format = NDIlib_recv_color_format_UYVY_BGRA;

		NDIlib_recv_instance_t instance = ndi_->recv_create_v3(&settings);
		if (!instance) {
			obs_log(LOG_WARNING, "could not create an NDI receiver for '%s'", config.address.c_str());
			return nullptr;
		}

		return std::make_unique<NdiReceiver>(ndi_, instance);
	}

	std::unique_ptr<ISender> create_sender(const SenderConfig &config) override
	{
		if (!available_ || config.name.empty())
			return nullptr;

		const Config &plugin_config = Config::instance();

		NDIlib_send_create_t settings = {};

		// NDI presents this on the network as "MACHINE (name)" - the machine prefix is the
		// runtime's doing, not ours, so the bare name is what goes in here.
		settings.p_ndi_name = config.name.c_str();
		settings.p_groups = plugin_config.ndi_groups.empty() ? nullptr : plugin_config.ndi_groups.c_str();

		// OBS already paces frames, so letting NDI clock them as well would fight it.
		settings.clock_video = false;
		settings.clock_audio = false;

		NDIlib_send_instance_t instance = ndi_->send_create(&settings);
		if (!instance) {
			obs_log(LOG_WARNING, "could not create an NDI sender named '%s'", config.name.c_str());
			return nullptr;
		}

		return std::make_unique<NdiSender>(ndi_, instance);
	}

private:
	bool create_finder()
	{
		const Config &config = Config::instance();

		NDIlib_find_create_t settings = {};
		settings.show_local_sources = true;
		settings.p_groups = config.ndi_groups.empty() ? nullptr : config.ndi_groups.c_str();
		settings.p_extra_ips = nullptr;

		finder_ = ndi_->find_create_v2(&settings);
		return finder_ != nullptr;
	}

	LibraryHandle library_;
	const NDIlib_v6 *ndi_ = nullptr;
	NDIlib_find_instance_t finder_ = nullptr;

	bool available_ = false;
	std::string runtime_version_;
	std::string unavailable_reason_ = "not loaded";
};

} // namespace

std::unique_ptr<IBackend> create_ndi_backend()
{
	return std::make_unique<NdiBackend>();
}

} // namespace satellite
