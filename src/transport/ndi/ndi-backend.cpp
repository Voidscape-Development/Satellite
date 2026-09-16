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
#include "transport/library-loader.hpp"

#include <chrono>
#include <thread>

namespace satellite {

const char *ndi_install_url()
{
	return "https://ndi.video/tools/";
}

std::vector<std::string> ndi_runtime_candidates()
{
	std::vector<std::string> candidates;

	// The SDK documents these environment variables as the way to find an install that is
	// not in a default location. Newest first.
	static const char *const kRuntimeDirVars[] = {"NDI_RUNTIME_DIR_V6", "NDI_RUNTIME_DIR_V5", "NDI_RUNTIME_DIR_V4"};

#ifdef _WIN32
	static const char *const kLibraryName = "Processing.NDI.Lib.x64.dll";

	// The redist installer sets this rather than the versioned variables.
	std::string redist = environment_variable("NDI_RUNTIME_DIR_V6");
	if (redist.empty())
		redist = environment_variable("NDILIB_REDIST_FOLDER");
	if (!redist.empty())
		candidates.push_back(redist + "\\" + kLibraryName);

	for (const char *var : kRuntimeDirVars) {
		std::string dir = environment_variable(var);
		if (!dir.empty())
			candidates.push_back(dir + "\\" + kLibraryName);
	}

	// Last resort: whatever is on the default DLL search path.
	candidates.push_back(kLibraryName);
#elif defined(__APPLE__)
	static const char *const kLibraryName = "libndi.dylib";

	for (const char *var : kRuntimeDirVars) {
		std::string dir = environment_variable(var);
		if (!dir.empty())
			candidates.push_back(dir + "/" + kLibraryName);
	}

	candidates.push_back("/usr/local/lib/libndi.dylib");
	candidates.push_back("/Library/NDI SDK for Apple/lib/macOS/libndi.dylib");
	candidates.push_back(kLibraryName);
#else
	for (const char *var : kRuntimeDirVars) {
		std::string dir = environment_variable(var);
		if (!dir.empty()) {
			candidates.push_back(dir + "/libndi.so.6");
			candidates.push_back(dir + "/libndi.so.5");
			candidates.push_back(dir + "/libndi.so");
		}
	}

	candidates.push_back("libndi.so.6");
	candidates.push_back("libndi.so.5");
	candidates.push_back("libndi.so");
	candidates.push_back("/usr/local/lib/libndi.so.6");
	candidates.push_back("/usr/local/lib/libndi.so.5");
#endif

	return candidates;
}

namespace {

class NdiBackend final : public IBackend {
public:
	Protocol protocol() const override { return Protocol::NDI; }

	bool load() override
	{
		if (available_)
			return true;

		if (!library_.open(ndi_runtime_candidates())) {
			runtime_found_ = false;
			unavailable_reason_ = "NDI runtime not found. Install the NDI Tools or NDI "
					      "runtime to enable NDI sources and outputs.";
			return false;
		}

		runtime_found_ = true;

		// M1: bind NDIlib_v5_load, initialize the SDK, create the long-lived finder, and
		// flip available_ to true. Until then the runtime is released again so we do not
		// hold a handle we are not using.
		found_path_ = library_.path();
		library_.close();
		unavailable_reason_ =
			"NDI runtime found at " + found_path_ + ", but Satellite's NDI backend is not implemented yet.";
		return false;
	}

	void unload() override
	{
		library_.close();
		available_ = false;
	}

	bool available() const override { return available_; }
	std::string unavailable_reason() const override { return unavailable_reason_; }
	std::string runtime_version() const override { return runtime_version_; }
	std::string install_url() const override { return ndi_install_url(); }

	bool wait_for_sources(int timeout_ms) override
	{
		// M1: NDIlib_find_wait_for_sources() on the long-lived finder, which blocks in the
		// SDK until the source list changes. Deliberately *not* a create/destroy cycle -
		// see docs/ARCHITECTURE.md section 2.1.
		if (timeout_ms > 0)
			std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
		return false;
	}

	std::vector<SourceRef> poll_sources() override { return {}; }

	std::unique_ptr<IReceiver> create_receiver(const ReceiverConfig &) override { return nullptr; }
	std::unique_ptr<ISender> create_sender(const SenderConfig &) override { return nullptr; }

	/// True when the runtime was located on disk, even if the backend is not usable yet.
	bool runtime_found() const { return runtime_found_; }

private:
	LibraryHandle library_;
	bool available_ = false;
	bool runtime_found_ = false;
	std::string found_path_;
	std::string runtime_version_;
	std::string unavailable_reason_ = "not loaded";
};

} // namespace

std::unique_ptr<IBackend> create_ndi_backend()
{
	return std::make_unique<NdiBackend>();
}

} // namespace satellite
