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

#include "metrics/feed-registry.hpp"
#include "transport/transport.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace satellite {

/// Owns one outbound feed: the backend sender, the frame queue, and the thread that drains
/// it. Shared by the Program and Preview outputs and by the Sender filter, which differ only
/// in where their frames come from.
///
/// Frames arrive on OBS's own threads - the video output thread for video, the audio thread
/// for audio - and those must not block. So push_video/push_audio only copy into a bounded
/// queue and return; everything that can stall on the network happens on the send thread.
class SenderSession {
public:
	SenderSession() = default;
	~SenderSession();

	SenderSession(const SenderSession &) = delete;
	SenderSession &operator=(const SenderSession &) = delete;

	bool start(Protocol protocol, const SenderConfig &config);
	void stop();
	bool running() const { return running_.load(std::memory_order_acquire); }

	/// Copies one single-plane video frame into the queue. Safe on any thread.
	void push_video(const VideoFrame &frame);
	/// Copies one planar-float audio frame into the queue. Safe on any thread.
	void push_audio(const AudioFrame &frame);

	/// Tally as last reported by downstream receivers. Safe on any thread.
	Tally tally() const;

private:
	struct Slot {
		std::vector<uint8_t> data;
		VideoFrame video;
		AudioFrame audio;
	};

	void run();
	void send_slot(Slot &slot, bool is_video);

	Slot take_free(std::vector<Slot> &pool);
	static void recycle(std::vector<Slot> &pool, Slot &&slot);

	std::unique_ptr<ISender> sender_;
	uint64_t feed_id_ = 0;
	std::string name_;

	std::thread thread_;
	std::atomic<bool> running_{false};

	mutable std::mutex mutex_;
	std::condition_variable wake_;

	std::deque<Slot> video_queue_;
	std::deque<Slot> audio_queue_;
	std::vector<Slot> video_pool_;
	std::vector<Slot> audio_pool_;

	int64_t dropped_ = 0;

	mutable std::mutex tally_mutex_;
	Tally tally_;
};

} // namespace satellite
