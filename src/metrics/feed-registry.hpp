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

#include "transport/transport.hpp"

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace satellite {

enum class FeedDirection {
	Send,
	Receive,
};

enum class FeedState {
	Idle,
	Connecting,
	Connected,
	Error,
};

const char *feed_state_display_name(FeedState state);

/// How many bitrate samples the sparkline keeps per feed.
constexpr size_t kFeedHistoryLength = 60;

/// A snapshot of one feed, as shown in a Satellite window row.
struct FeedSnapshot {
	uint64_t id = 0;
	Protocol protocol = Protocol::NDI;
	FeedDirection direction = FeedDirection::Receive;
	FeedState state = FeedState::Idle;

	std::string name;
	/// e.g. "1920x1080p59.94 UYVY"; empty until the first frame arrives.
	std::string video_format;
	/// e.g. "48 kHz 2ch"; empty when the feed carries no audio.
	std::string audio_format;

	FeedStats stats;
	Tally tally;

	/// Bitrate history, oldest first, for the row's sparkline.
	std::vector<double> bitrate_history;
};

/// The live table behind the Satellite window.
///
/// Sources, filters and outputs register themselves here when they go active and update
/// their counters from their own receive/send thread. The UI samples snapshots on a timer,
/// so the dock never adds latency to a frame path and never holds a backend lock.
class FeedRegistry {
public:
	static FeedRegistry &instance();

	/// Returns an id used for every later call about this feed.
	uint64_t register_feed(Protocol protocol, FeedDirection direction, const std::string &name);
	void unregister_feed(uint64_t id);

	void set_state(uint64_t id, FeedState state);
	void set_name(uint64_t id, const std::string &name);
	void set_video_format(uint64_t id, const std::string &format);
	void set_audio_format(uint64_t id, const std::string &format);
	void set_tally(uint64_t id, const Tally &tally);

	/// Records the current counters and appends one sample to the bitrate history.
	void update_stats(uint64_t id, const FeedStats &stats);

	std::vector<FeedSnapshot> snapshot() const;
	size_t active_count() const;

	void clear();

private:
	FeedRegistry() = default;

	FeedRegistry(const FeedRegistry &) = delete;
	FeedRegistry &operator=(const FeedRegistry &) = delete;

	struct Entry {
		FeedSnapshot snapshot;
		std::deque<double> history;
	};

	Entry *find(uint64_t id);

	mutable std::mutex mutex_;
	std::vector<Entry> feeds_;
	uint64_t next_id_ = 1;
};

} // namespace satellite
