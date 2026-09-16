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
#include "transport/library-loader.hpp"

#include <chrono>
#include <thread>

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

	// Bundled copy, shipped next to the plugin binary.
	if (!plugin_dir.empty())
		candidates.push_back(plugin_dir + separator + kLibraryName);

	// A user-built or system-wide install.
	candidates.push_back(kLibraryName);
#ifndef _WIN32
	candidates.push_back(std::string("/usr/local/lib/") + kLibraryName);
	candidates.push_back(std::string("/usr/lib/") + kLibraryName);
#endif

	return candidates;
}

namespace {

class OmtBackend final : public IBackend {
public:
	Protocol protocol() const override { return Protocol::OMT; }

	bool load() override
	{
		if (available_)
			return true;

		if (!library_.open(omt_runtime_candidates())) {
			unavailable_reason_ = "libomt not found. It ships with Satellite, so this "
					      "usually means the plugin package is incomplete.";
			return false;
		}

		// M3: bind omt_discovery_getaddresses, omt_receive_*, omt_send_* and the settings
		// entry points, apply the configured discovery server, and flip available_ to true.
		found_path_ = library_.path();
		library_.close();
		unavailable_reason_ =
			"libomt found at " + found_path_ + ", but Satellite's OMT backend is not implemented yet.";
		return false;
	}

	void unload() override
	{
		// M3: omt_shutdown() stops libomt's logging and discovery background threads. It
		// must run before the library handle is released.
		library_.close();
		available_ = false;
	}

	bool available() const override { return available_; }
	std::string unavailable_reason() const override { return unavailable_reason_; }
	std::string runtime_version() const override { return runtime_version_; }

	/// Empty: Satellite bundles OMT, so there is nothing for the user to go and install.
	std::string install_url() const override { return {}; }

	bool wait_for_sources(int timeout_ms) override
	{
		// OMT has no blocking discovery call - libomt runs its own discovery thread and
		// omt_discovery_getaddresses() is a snapshot - so this backend sleeps for the
		// shared cadence and lets poll_sources() do the work.
		if (timeout_ms > 0)
			std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
		return true;
	}

	std::vector<SourceRef> poll_sources() override
	{
		// M3: omt_discovery_getaddresses() hands back an array that is valid only until
		// its next call, so the returned strings must be copied here, and this must stay
		// the single-caller path (the discovery thread).
		return {};
	}

	std::unique_ptr<IReceiver> create_receiver(const ReceiverConfig &) override { return nullptr; }
	std::unique_ptr<ISender> create_sender(const SenderConfig &) override { return nullptr; }

private:
	LibraryHandle library_;
	bool available_ = false;
	std::string found_path_;
	std::string runtime_version_;
	std::string unavailable_reason_ = "not loaded";
};

} // namespace

std::unique_ptr<IBackend> create_omt_backend()
{
	return std::make_unique<OmtBackend>();
}

} // namespace satellite
