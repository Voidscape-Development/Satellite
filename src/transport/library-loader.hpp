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

#include <string>
#include <vector>

namespace satellite {

/// Thin RAII wrapper over dlopen/LoadLibrary.
///
/// Satellite never link-time-links either transport runtime: NDI because it is proprietary
/// and may not be installed, OMT because bundling it as a runtime load keeps both backends
/// on the same code path. A failure here is reported to the user, not fatal to the module.
class LibraryHandle {
public:
	LibraryHandle() = default;
	~LibraryHandle();

	LibraryHandle(const LibraryHandle &) = delete;
	LibraryHandle &operator=(const LibraryHandle &) = delete;

	/// Try each candidate path in order; the first that loads wins.
	bool open(const std::vector<std::string> &candidates);
	void close();

	bool is_open() const { return handle_ != nullptr; }
	const std::string &path() const { return path_; }
	/// Platform error text from the last failed open().
	const std::string &last_error() const { return last_error_; }

	void *symbol(const char *name) const;

	template<typename Fn> bool bind(Fn &out, const char *name) const
	{
		out = reinterpret_cast<Fn>(symbol(name));
		return out != nullptr;
	}

private:
	void *handle_ = nullptr;
	std::string path_;
	std::string last_error_;
};

/// Directory the plugin's own binary lives in, used to find bundled runtimes.
std::string plugin_binary_directory();

/// Value of an environment variable, or an empty string.
std::string environment_variable(const char *name);

} // namespace satellite
