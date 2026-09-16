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

#include "metrics/feed-registry.hpp"

#include <algorithm>

namespace satellite {

const char *feed_state_display_name(FeedState state)
{
	switch (state) {
	case FeedState::Idle:
		return "Idle";
	case FeedState::Connecting:
		return "Connecting";
	case FeedState::Connected:
		return "Connected";
	case FeedState::Error:
		return "Error";
	}
	return "Idle";
}

FeedRegistry &FeedRegistry::instance()
{
	static FeedRegistry registry;
	return registry;
}

uint64_t FeedRegistry::register_feed(Protocol protocol, FeedDirection direction, const std::string &name)
{
	std::lock_guard<std::mutex> lock(mutex_);

	Entry entry;
	entry.snapshot.id = next_id_++;
	entry.snapshot.protocol = protocol;
	entry.snapshot.direction = direction;
	entry.snapshot.state = FeedState::Connecting;
	entry.snapshot.name = name;

	feeds_.push_back(std::move(entry));
	return feeds_.back().snapshot.id;
}

void FeedRegistry::unregister_feed(uint64_t id)
{
	std::lock_guard<std::mutex> lock(mutex_);
	feeds_.erase(std::remove_if(feeds_.begin(), feeds_.end(),
				    [id](const Entry &entry) { return entry.snapshot.id == id; }),
		     feeds_.end());
}

FeedRegistry::Entry *FeedRegistry::find(uint64_t id)
{
	for (Entry &entry : feeds_) {
		if (entry.snapshot.id == id)
			return &entry;
	}
	return nullptr;
}

void FeedRegistry::set_state(uint64_t id, FeedState state)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (Entry *entry = find(id))
		entry->snapshot.state = state;
}

void FeedRegistry::set_name(uint64_t id, const std::string &name)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (Entry *entry = find(id))
		entry->snapshot.name = name;
}

void FeedRegistry::set_video_format(uint64_t id, const std::string &format)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (Entry *entry = find(id))
		entry->snapshot.video_format = format;
}

void FeedRegistry::set_audio_format(uint64_t id, const std::string &format)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (Entry *entry = find(id))
		entry->snapshot.audio_format = format;
}

void FeedRegistry::set_tally(uint64_t id, const Tally &tally)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (Entry *entry = find(id))
		entry->snapshot.tally = tally;
}

void FeedRegistry::update_stats(uint64_t id, const FeedStats &stats)
{
	std::lock_guard<std::mutex> lock(mutex_);

	Entry *entry = find(id);
	if (!entry)
		return;

	entry->snapshot.stats = stats;
	entry->history.push_back(stats.bitrate_mbps);
	while (entry->history.size() > kFeedHistoryLength)
		entry->history.pop_front();
}

std::vector<FeedSnapshot> FeedRegistry::snapshot() const
{
	std::lock_guard<std::mutex> lock(mutex_);

	std::vector<FeedSnapshot> out;
	out.reserve(feeds_.size());
	for (const Entry &entry : feeds_) {
		FeedSnapshot copy = entry.snapshot;
		copy.bitrate_history.assign(entry.history.begin(), entry.history.end());
		out.push_back(std::move(copy));
	}
	return out;
}

size_t FeedRegistry::active_count() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return feeds_.size();
}

void FeedRegistry::clear()
{
	std::lock_guard<std::mutex> lock(mutex_);
	feeds_.clear();
}

} // namespace satellite
