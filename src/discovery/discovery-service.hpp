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

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace satellite {

/// One process-wide background thread that keeps the network source list current for every
/// protocol.
///
/// The thread holds a long-lived finder per backend and blocks in
/// IBackend::wait_for_sources(); it does not create and tear down finders on a duty cycle.
/// Creating the finder is the expensive part, a blocked finder is just a socket wait, and a
/// duty cycle makes sources invisible for the length of its sleep. The *UI* refresh is what
/// gets throttled instead. See docs/ARCHITECTURE.md section 2.1.
///
/// It is also the only legal caller of OMT's omt_discovery_getaddresses(), whose returned
/// buffer stays valid only until its next call.
class DiscoveryService {
public:
	static DiscoveryService &instance();

	void start();
	void stop();

	/// Copied snapshot of everything currently visible, across all protocols.
	std::vector<SourceRef> sources() const;
	/// Copied snapshot filtered to one protocol.
	std::vector<SourceRef> sources_for(Protocol protocol) const;

	/// Bumped every time the source list changes, so the UI can skip redundant repaints.
	uint64_t generation() const { return generation_.load(std::memory_order_acquire); }

private:
	DiscoveryService() = default;
	~DiscoveryService();

	DiscoveryService(const DiscoveryService &) = delete;
	DiscoveryService &operator=(const DiscoveryService &) = delete;

	void run();
	void publish(std::vector<SourceRef> &&found);

	mutable std::mutex mutex_;
	std::vector<SourceRef> sources_;

	std::thread thread_;
	std::mutex wake_mutex_;
	std::condition_variable wake_;
	std::atomic<bool> running_{false};
	std::atomic<uint64_t> generation_{0};
};

} // namespace satellite
