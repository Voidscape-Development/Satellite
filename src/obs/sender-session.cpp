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

#include "obs/sender-session.hpp"

#include <chrono>
#include <cstring>

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

namespace satellite {

namespace {

/// Video frames are large, so the queue is short: a backlog means the network cannot keep
/// up, and the freshest frame is worth more than a queued stale one.
constexpr size_t kMaxVideoQueue = 3;

/// Audio buffers are tiny and dropping them is audible, so this is deliberately generous.
constexpr size_t kMaxAudioQueue = 32;

/// How often the send thread refreshes tally and the Satellite window's counters.
constexpr uint64_t kPollIntervalNs = 200000000;

/// How long a sender goes on being wanted after its last receiver leaves. A receiver that
/// drops and reconnects - switching inputs, a network blip - should not cost a teardown and
/// rebuild of the capture path each time.
constexpr uint64_t kDemandLingerNs = 3000000000;

/// Whether every pixel of a frame lives in data[0].
bool is_single_plane(video_format format)
{
	switch (format) {
	case VIDEO_FORMAT_UYVY:
	case VIDEO_FORMAT_YUY2:
	case VIDEO_FORMAT_RGBA:
	case VIDEO_FORMAT_BGRA:
	case VIDEO_FORMAT_BGRX:
		return true;
	default:
		return false;
	}
}

/// Copies rows of one plane, repacking to the destination stride when the two differ.
void copy_plane(uint8_t *to, uint32_t to_stride, const uint8_t *from, uint32_t from_stride, uint32_t rows)
{
	if (to_stride == from_stride) {
		memcpy(to, from, static_cast<size_t>(to_stride) * rows);
		return;
	}

	const uint32_t bytes = to_stride < from_stride ? to_stride : from_stride;
	for (uint32_t row = 0; row < rows; ++row)
		memcpy(to + static_cast<size_t>(to_stride) * row, from + static_cast<size_t>(from_stride) * row, bytes);
}

} // namespace

SenderSession::~SenderSession()
{
	stop();
}

/// push_video copies what it can describe to a backend as one contiguous buffer: a single
/// plane, or NV12's two. Anything else would reach the backend describing planes that were
/// never copied, and the runtime would read past the end of the slot - so the senders ask OBS
/// to convert whatever is not on this list, and push_video drops what still is not.
bool SenderSession::can_send(video_format format)
{
	return format == VIDEO_FORMAT_NV12 || is_single_plane(format);
}

bool SenderSession::start(Protocol protocol, const SenderConfig &config, std::function<void()> on_demand_changed)
{
	stop();

	IBackend *backend = backend_for(protocol);
	if (!backend || !backend->available()) {
		obs_log(LOG_INFO, "not starting sender '%s': %s is unavailable", config.name.c_str(),
			protocol_display_name(protocol));
		return false;
	}

	sender_ = backend->create_sender(config);
	if (!sender_)
		return false;

	name_ = config.name;
	feed_id_ = FeedRegistry::instance().register_feed(protocol, FeedDirection::Send, config.name);
	dropped_ = 0;
	warned_format_.store(false, std::memory_order_relaxed);
	wanted_.store(false, std::memory_order_release);
	on_demand_changed_ = std::move(on_demand_changed);

	running_.store(true, std::memory_order_release);
	thread_ = std::thread(&SenderSession::run, this);

	obs_log(LOG_INFO, "sending '%s' over %s", config.name.c_str(), protocol_display_name(protocol));
	return true;
}

void SenderSession::stop()
{
	if (!running_.exchange(false))
		return;

	wake_.notify_all();
	if (thread_.joinable())
		thread_.join();

	sender_.reset();
	wanted_.store(false, std::memory_order_release);
	on_demand_changed_ = nullptr;

	{
		std::lock_guard<std::mutex> lock(mutex_);
		video_queue_.clear();
		audio_queue_.clear();
		video_pool_.clear();
		audio_pool_.clear();
	}

	if (feed_id_) {
		FeedRegistry::instance().unregister_feed(feed_id_);
		feed_id_ = 0;
	}

	if (!name_.empty()) {
		obs_log(LOG_INFO, "stopped sending '%s'", name_.c_str());
		name_.clear();
	}
}

SenderSession::Slot SenderSession::take_free(std::vector<Slot> &pool)
{
	// Caller holds mutex_. Reusing a slot keeps the per-frame cost to a memcpy instead of
	// an allocation of several megabytes on the OBS video thread.
	if (pool.empty())
		return {};

	Slot slot = std::move(pool.back());
	pool.pop_back();
	return slot;
}

void SenderSession::recycle(std::vector<Slot> &pool, Slot &&slot)
{
	// Keeps the buffer's capacity, so the next frame of the same size needs no allocation.
	if (pool.size() < kMaxVideoQueue + kMaxAudioQueue)
		pool.push_back(std::move(slot));
}

void SenderSession::push_video(const VideoFrame &frame)
{
	if (!running_.load(std::memory_order_acquire) || !frame.data[0] || frame.linesize[0] == 0)
		return;

	// NV12 is OBS's own output format on nearly every setup, and both protocols take it as
	// it is, so it travels as two planes rather than being converted first.
	const bool nv12 = frame.format == VIDEO_FORMAT_NV12;

	if (!can_send(frame.format) || (nv12 && (!frame.data[1] || frame.linesize[1] == 0))) {
		// Once per session: this cannot fix itself, and it arrives at frame rate.
		if (!warned_format_.exchange(true, std::memory_order_relaxed))
			obs_log(LOG_WARNING, "dropping '%s' video: OBS format %d cannot be sent as it is",
				name_.c_str(), static_cast<int>(frame.format));
		return;
	}

	const uint32_t stride = frame.linesize[0];
	const size_t luma_bytes = static_cast<size_t>(stride) * frame.height;
	const uint32_t chroma_rows = nv12 ? (frame.height + 1) / 2 : 0;
	const size_t bytes = luma_bytes + static_cast<size_t>(stride) * chroma_rows;

	std::unique_lock<std::mutex> lock(mutex_);

	Slot slot = take_free(video_pool_);
	slot.video = frame;
	slot.data.resize(bytes);
	memcpy(slot.data.data(), frame.data[0], luma_bytes);

	// The pointers have to follow the copy, not the caller's buffer, which is only valid
	// for the duration of the OBS callback.
	slot.video.data[0] = slot.data.data();
	for (size_t plane = 1; plane < MAX_AV_PLANES; ++plane) {
		slot.video.data[plane] = nullptr;
		slot.video.linesize[plane] = 0;
	}

	if (nv12) {
		// Straight after the luma and at the luma's stride: the one contiguous buffer
		// both runtimes expect an NV12 frame to be.
		slot.video.data[1] = slot.data.data() + luma_bytes;
		slot.video.linesize[1] = stride;
		copy_plane(slot.video.data[1], stride, frame.data[1], frame.linesize[1], chroma_rows);
	}

	while (video_queue_.size() >= kMaxVideoQueue) {
		recycle(video_pool_, std::move(video_queue_.front()));
		video_queue_.pop_front();
		++dropped_;
	}

	video_queue_.push_back(std::move(slot));
	lock.unlock();
	wake_.notify_one();
}

void SenderSession::push_audio(const AudioFrame &frame)
{
	if (!running_.load(std::memory_order_acquire) || frame.channels == 0 || frame.frames == 0)
		return;

	const size_t plane_bytes = static_cast<size_t>(frame.frames) * sizeof(float);
	const size_t bytes = plane_bytes * frame.channels;

	std::unique_lock<std::mutex> lock(mutex_);

	Slot slot = take_free(audio_pool_);
	slot.audio = frame;
	slot.data.resize(bytes);

	// Packed back to back, which is exactly the contiguous planar layout the backends
	// describe with a channel stride.
	for (uint32_t channel = 0; channel < frame.channels; ++channel) {
		if (!frame.data[channel])
			return;
		memcpy(slot.data.data() + plane_bytes * channel, frame.data[channel], plane_bytes);
	}

	for (uint32_t channel = 0; channel < frame.channels; ++channel)
		slot.audio.data[channel] = slot.data.data() + plane_bytes * channel;

	while (audio_queue_.size() >= kMaxAudioQueue) {
		recycle(audio_pool_, std::move(audio_queue_.front()));
		audio_queue_.pop_front();
	}

	audio_queue_.push_back(std::move(slot));
	lock.unlock();
	wake_.notify_one();
}

Tally SenderSession::tally() const
{
	std::lock_guard<std::mutex> lock(tally_mutex_);
	return tally_;
}

void SenderSession::send_slot(Slot &slot, bool is_video)
{
	if (is_video)
		sender_->send_video(slot.video);
	else
		sender_->send_audio(slot.audio);
}

void SenderSession::run()
{
	uint64_t last_poll_ns = 0;
	uint64_t last_receiver_ns = 0;

	while (running_.load(std::memory_order_acquire)) {
		Slot slot;
		bool is_video = false;
		bool have_work = false;

		{
			std::unique_lock<std::mutex> lock(mutex_);
			wake_.wait_for(lock, std::chrono::milliseconds(50), [this] {
				return !running_.load(std::memory_order_acquire) || !audio_queue_.empty() ||
				       !video_queue_.empty();
			});

			// Audio first: it is latency-sensitive and cheap, and a video frame that
			// waits one turn is far less noticeable than an audio gap.
			if (!audio_queue_.empty()) {
				slot = std::move(audio_queue_.front());
				audio_queue_.pop_front();
				have_work = true;
			} else if (!video_queue_.empty()) {
				slot = std::move(video_queue_.front());
				video_queue_.pop_front();
				is_video = true;
				have_work = true;
			}
		}

		if (have_work && running_.load(std::memory_order_acquire)) {
			send_slot(slot, is_video);

			std::lock_guard<std::mutex> lock(mutex_);
			recycle(is_video ? video_pool_ : audio_pool_, std::move(slot));
		}

		const uint64_t now = os_gettime_ns();
		if (now - last_poll_ns < kPollIntervalNs)
			continue;
		last_poll_ns = now;

		const Tally current = sender_->tally();
		{
			std::lock_guard<std::mutex> lock(tally_mutex_);
			tally_ = current;
		}

		FeedStats stats = sender_->stats();
		{
			std::lock_guard<std::mutex> lock(mutex_);
			stats.frames_dropped = dropped_;
		}

		FeedRegistry &registry = FeedRegistry::instance();
		registry.update_stats(feed_id_, stats);
		registry.set_tally(feed_id_, current);
		registry.set_state(feed_id_, stats.connections > 0 ? FeedState::Connected : FeedState::Idle);

		// A backend that cannot count its receivers reports -1, and is always wanted.
		if (stats.connections != 0)
			last_receiver_ns = now;

		const bool wanted = stats.connections != 0 ||
				    (last_receiver_ns != 0 && now - last_receiver_ns < kDemandLingerNs);
		if (wanted_.exchange(wanted, std::memory_order_acq_rel) != wanted && on_demand_changed_)
			on_demand_changed_();
	}
}

} // namespace satellite
