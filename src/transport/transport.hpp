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

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <media-io/media-io-defs.h>
#include <media-io/video-io.h>
#include <media-io/audio-io.h>

namespace satellite {

enum class Protocol {
	NDI,
	OMT,
};

/// Every protocol Satellite knows about, in UI display order.
extern const Protocol kAllProtocols[2];

const char *protocol_id(Protocol protocol); //< stable, used in saved settings
const char *protocol_display_name(Protocol protocol);
bool protocol_from_id(const char *id, Protocol &out);

/// Shared quality/bandwidth knob. NDI and OMT shape this differently (OMT has
/// OMTQuality Default/Low/Medium/High, NDI has bandwidth modes), so each backend maps
/// this onto its own vocabulary rather than exposing the native enum.
enum class Quality {
	Default,
	Low,
	Medium,
	High,
	/// 1/8th preview feed. OMTReceiveFlags_Preview, NDIlib_recv_bandwidth_lowest.
	PreviewOnly,
	/// No video at all.
	AudioOnly,
};

const char *quality_id(Quality quality);
bool quality_from_id(const char *id, Quality &out);

struct Tally {
	bool program = false;
	bool preview = false;

	bool operator==(const Tally &other) const { return program == other.program && preview == other.preview; }
	bool operator!=(const Tally &other) const { return !(*this == other); }
};

/// A source seen on the network by the discovery service.
struct SourceRef {
	Protocol protocol = Protocol::NDI;
	/// Human-readable name, e.g. "STUDIO-PC (Camera 1)".
	std::string name;
	/// What gets handed back to the backend to connect. For OMT this is either the
	/// discovery name or an "omt://host:port" URL; for NDI it is the source name or a
	/// machine address.
	std::string address;

	bool operator==(const SourceRef &other) const
	{
		return protocol == other.protocol && name == other.name && address == other.address;
	}
};

/// Normalized video frame, shaped to match obs_source_frame so the common UYVY path is a
/// field copy rather than a pixel conversion.
struct VideoFrame {
	uint8_t *data[MAX_AV_PLANES] = {};
	uint32_t linesize[MAX_AV_PLANES] = {};
	uint32_t width = 0;
	uint32_t height = 0;
	video_format format = VIDEO_FORMAT_UYVY;
	video_colorspace colorspace = VIDEO_CS_DEFAULT;
	video_range_type range = VIDEO_RANGE_DEFAULT;
	/// Nanoseconds. Both protocols use 100ns units on the wire, so backends multiply by 100.
	uint64_t timestamp_ns = 0;
	uint32_t framerate_num = 0;
	uint32_t framerate_den = 0;
	bool has_alpha = false;
	bool interlaced = false;
};

/// Normalized audio frame. Both protocols carry 32-bit planar float, which is OBS's
/// internal AUDIO_FORMAT_FLOAT_PLANAR, so this path also needs no conversion.
struct AudioFrame {
	uint8_t *data[MAX_AUDIO_CHANNELS] = {};
	uint32_t frames = 0;
	uint32_t sample_rate = 0;
	uint32_t channels = 0;
	uint64_t timestamp_ns = 0;
};

enum class FrameType {
	None,
	Video,
	Audio,
	Metadata,
};

/// A frame handed back by IReceiver::capture().
///
/// IMPORTANT: this is *borrowed*, not owned. OMT's contract is that frame data stays valid
/// only until the next omt_receive() on the same instance and frame type; NDI requires an
/// explicit free. Hand the payload to obs_source_output_video/_audio (both copy internally)
/// and let this go out of scope before capturing again on the same receiver.
class IReceiver;

class CapturedFrame {
public:
	CapturedFrame() = default;
	CapturedFrame(IReceiver *owner, FrameType type) : owner_(owner), type_(type) {}
	~CapturedFrame();

	CapturedFrame(const CapturedFrame &) = delete;
	CapturedFrame &operator=(const CapturedFrame &) = delete;
	CapturedFrame(CapturedFrame &&other) noexcept { move_from(other); }
	CapturedFrame &operator=(CapturedFrame &&other) noexcept;

	FrameType type() const { return type_; }
	explicit operator bool() const { return type_ != FrameType::None; }

	VideoFrame video;
	AudioFrame audio;
	std::string metadata;

private:
	void move_from(CapturedFrame &other) noexcept;
	void release();

	IReceiver *owner_ = nullptr;
	FrameType type_ = FrameType::None;
};

/// Live counters for one feed, sampled by the Satellite window.
///
/// OMT fills most of these from OMTStatistics. NDI exposes frame counts but no byte
/// counters, so bitrate is measured by Satellite from frame sizes and flagged estimated.
struct FeedStats {
	double bitrate_mbps = 0.0;
	bool bitrate_estimated = false;
	double fps = 0.0;
	int64_t frames = 0;
	int64_t frames_dropped = 0;
	/// Milliseconds spent in the codec on the last frame. OMT only; -1 when unknown.
	double codec_ms = -1.0;
	/// Senders only; -1 when not applicable.
	int connections = -1;
};

struct ReceiverConfig {
	std::string address;
	Quality quality = Quality::Default;
	bool want_video = true;
	bool want_audio = true;
	bool want_alpha = false;
};

struct SenderConfig {
	std::string name;
	Quality quality = Quality::Default;
	bool send_audio = true;
	bool send_alpha = false;
};

class IReceiver {
public:
	virtual ~IReceiver() = default;

	/// Blocks up to timeout_ms. Returns a frame of type None on timeout.
	virtual CapturedFrame capture(int timeout_ms) = 0;

	/// Tell the upstream sender whether we have it on program/preview.
	virtual void set_tally(const Tally &tally) = 0;

	virtual bool connected() const = 0;
	virtual FeedStats stats() const = 0;

protected:
	friend class CapturedFrame;
	/// Release whatever capture() handed out. Called by ~CapturedFrame.
	virtual void release_frame(FrameType type) = 0;
};

class ISender {
public:
	virtual ~ISender() = default;

	virtual bool send_video(const VideoFrame &frame) = 0;
	virtual bool send_audio(const AudioFrame &frame) = 0;

	/// Tally as reported by our receivers.
	virtual Tally tally() const = 0;
	virtual int connections() const = 0;
	virtual FeedStats stats() const = 0;
};

/// One per protocol, created at module load. Owns the runtime library handle and the
/// discovery finder.
///
/// Availability is first-class state rather than a load failure: the NDI runtime is
/// proprietary and may simply not be installed, in which case the Satellite window shows
/// the protocol as unavailable with an install link, and the rest of the plugin keeps
/// working.
class IBackend {
public:
	virtual ~IBackend() = default;

	virtual Protocol protocol() const = 0;

	/// Try to locate and load the runtime. Safe to call repeatedly; returns available().
	virtual bool load() = 0;
	virtual void unload() = 0;

	virtual bool available() const = 0;
	/// Why not, when !available(). Shown verbatim in the Satellite window.
	virtual std::string unavailable_reason() const = 0;
	virtual std::string runtime_version() const = 0;
	/// Where to get the runtime. Empty when Satellite bundles it (OMT).
	virtual std::string install_url() const = 0;

	/// Re-read the plugin configuration and apply whatever can change while running.
	///
	/// Called from the UI thread when the user edits the advanced settings. Backends must
	/// not do anything here that the discovery or media threads could be using - the NDI
	/// backend, for instance, only marks its finder for rebuilding and lets the discovery
	/// thread do the work, because that thread is the finder's only user.
	virtual void settings_changed() = 0;

	/// Block until the source list may have changed, or timeout_ms elapses.
	///
	/// NDI blocks inside the SDK (NDIlib_find_wait_for_sources); OMT has no blocking
	/// discovery call, so its backend sleeps. Either way this is called from the single
	/// discovery thread only - omt_discovery_getaddresses() returns a buffer valid only
	/// until its next call, so it has exactly one legal caller.
	virtual bool wait_for_sources(int timeout_ms) = 0;

	/// Snapshot of currently visible sources. Discovery thread only.
	virtual std::vector<SourceRef> poll_sources() = 0;

	virtual std::unique_ptr<IReceiver> create_receiver(const ReceiverConfig &config) = 0;
	virtual std::unique_ptr<ISender> create_sender(const SenderConfig &config) = 0;
};

/// Backends are owned by the registry and live for the module's lifetime.
void register_backends();
void unregister_backends();
IBackend *backend_for(Protocol protocol);
std::vector<IBackend *> all_backends();

} // namespace satellite
