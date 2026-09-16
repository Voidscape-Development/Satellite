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

#include <atomic>
#include <cstdint>

#include <util/platform.h>

namespace satellite {

/// Measures throughput and frame rate over a sliding window.
///
/// OMT reports bytes and frame counts itself through OMTStatistics, so this exists for NDI,
/// whose NDIlib_recv_performance_t carries frame counts but no byte counters at all. Feeding
/// it each frame's size is the only way to put a bitrate next to an NDI feed, and the result
/// is flagged estimated in the Satellite window so it is not confused with OMT's exact
/// figure.
///
/// Written from a receive or send thread and read from the UI thread, so the published
/// results are atomics and the accumulators are only ever touched by the writer.
class RateMeter {
public:
	/// Record one frame of the given size. Writer thread only.
	void add_frame(uint64_t bytes)
	{
		const uint64_t now = os_gettime_ns();
		if (window_start_ns_ == 0) {
			window_start_ns_ = now;
			return;
		}

		bytes_ += bytes;
		++frames_;

		const uint64_t elapsed_ns = now - window_start_ns_;
		if (elapsed_ns < kWindowNs)
			return;

		const double seconds = static_cast<double>(elapsed_ns) / 1e9;
		mbps_.store(static_cast<double>(bytes_) * 8.0 / seconds / 1e6, std::memory_order_relaxed);
		fps_.store(static_cast<double>(frames_) / seconds, std::memory_order_relaxed);

		bytes_ = 0;
		frames_ = 0;
		window_start_ns_ = now;
	}

	double mbps() const { return mbps_.load(std::memory_order_relaxed); }
	double fps() const { return fps_.load(std::memory_order_relaxed); }

private:
	/// Long enough that a single late frame does not swing the reading, short enough that
	/// the dock's one-second refresh still shows something current.
	static constexpr uint64_t kWindowNs = 500000000;

	uint64_t window_start_ns_ = 0;
	uint64_t bytes_ = 0;
	uint64_t frames_ = 0;

	std::atomic<double> mbps_{0.0};
	std::atomic<double> fps_{0.0};
};

} // namespace satellite
