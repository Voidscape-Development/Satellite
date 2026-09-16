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

#include "discovery/discovery-service.hpp"

#include <algorithm>
#include <chrono>

#include <plugin-support.h>
#include <obs-module.h>

namespace satellite {

namespace {

/// How long a single wait_for_sources() call may block. NDI returns early when the list
/// changes, so this is a ceiling rather than a polling interval.
constexpr int kWaitTimeoutMs = 1000;

} // namespace

DiscoveryService &DiscoveryService::instance()
{
	static DiscoveryService service;
	return service;
}

DiscoveryService::~DiscoveryService()
{
	stop();
}

void DiscoveryService::start()
{
	if (running_.exchange(true))
		return;

	thread_ = std::thread(&DiscoveryService::run, this);
	obs_log(LOG_INFO, "discovery thread started");
}

void DiscoveryService::stop()
{
	if (!running_.exchange(false))
		return;

	wake_.notify_all();
	if (thread_.joinable())
		thread_.join();

	{
		std::lock_guard<std::mutex> lock(mutex_);
		sources_.clear();
	}

	obs_log(LOG_INFO, "discovery thread stopped");
}

void DiscoveryService::run()
{
	while (running_.load(std::memory_order_acquire)) {
		std::vector<SourceRef> found;

		for (IBackend *backend : all_backends()) {
			if (!running_.load(std::memory_order_acquire))
				break;
			if (!backend->available())
				continue;

			backend->wait_for_sources(kWaitTimeoutMs);

			std::vector<SourceRef> from_backend = backend->poll_sources();
			found.insert(found.end(), std::make_move_iterator(from_backend.begin()),
				     std::make_move_iterator(from_backend.end()));
		}

		if (found.empty()) {
			// Nothing to wait on yet - either no backend has loaded, or none found
			// anything. Sleep the same cadence so an unavailable runtime does not spin.
			std::unique_lock<std::mutex> lock(wake_mutex_);
			wake_.wait_for(lock, std::chrono::milliseconds(kWaitTimeoutMs),
				       [this] { return !running_.load(std::memory_order_acquire); });
		}

		publish(std::move(found));
	}
}

void DiscoveryService::publish(std::vector<SourceRef> &&found)
{
	std::sort(found.begin(), found.end(), [](const SourceRef &a, const SourceRef &b) {
		if (a.protocol != b.protocol)
			return static_cast<int>(a.protocol) < static_cast<int>(b.protocol);
		return a.name < b.name;
	});

	std::lock_guard<std::mutex> lock(mutex_);
	if (sources_ == found)
		return;

	sources_ = std::move(found);
	generation_.fetch_add(1, std::memory_order_release);
}

std::vector<SourceRef> DiscoveryService::sources() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return sources_;
}

std::vector<SourceRef> DiscoveryService::sources_for(Protocol protocol) const
{
	std::lock_guard<std::mutex> lock(mutex_);

	std::vector<SourceRef> filtered;
	for (const SourceRef &source : sources_) {
		if (source.protocol == protocol)
			filtered.push_back(source);
	}
	return filtered;
}

} // namespace satellite
