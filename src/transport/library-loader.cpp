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

#include "transport/library-loader.hpp"

#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace satellite {

namespace {

#ifdef _WIN32
std::string last_system_error()
{
	DWORD code = GetLastError();
	if (code == 0)
		return {};

	LPSTR buffer = nullptr;
	DWORD length = FormatMessageA(
		FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
		code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPSTR>(&buffer), 0, nullptr);
	std::string message = length && buffer ? std::string(buffer, length) : std::string();
	if (buffer)
		LocalFree(buffer);

	while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
		message.pop_back();
	return message;
}
#endif

} // namespace

LibraryHandle::~LibraryHandle()
{
	close();
}

bool LibraryHandle::open(const std::vector<std::string> &candidates)
{
	close();

	for (const std::string &candidate : candidates) {
		if (candidate.empty())
			continue;

#ifdef _WIN32
		handle_ = static_cast<void *>(LoadLibraryA(candidate.c_str()));
		if (!handle_)
			last_error_ = last_system_error();
#else
		handle_ = dlopen(candidate.c_str(), RTLD_LOCAL | RTLD_LAZY);
		if (!handle_) {
			const char *error = dlerror();
			last_error_ = error ? error : "";
		}
#endif
		if (handle_) {
			path_ = candidate;
			last_error_.clear();
			return true;
		}
	}

	return false;
}

void LibraryHandle::close()
{
	if (!handle_)
		return;

#ifdef _WIN32
	FreeLibrary(static_cast<HMODULE>(handle_));
#else
	dlclose(handle_);
#endif
	handle_ = nullptr;
	path_.clear();
}

void *LibraryHandle::symbol(const char *name) const
{
	if (!handle_ || !name)
		return nullptr;

#ifdef _WIN32
	return reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(handle_), name));
#else
	return dlsym(handle_, name);
#endif
}

std::string plugin_binary_directory()
{
#ifdef _WIN32
	HMODULE module = nullptr;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCSTR>(&plugin_binary_directory), &module))
		return {};

	char path[MAX_PATH] = {};
	DWORD length = GetModuleFileNameA(module, path, MAX_PATH);
	if (length == 0 || length == MAX_PATH)
		return {};

	std::string full(path, length);
	size_t slash = full.find_last_of("\\/");
	return slash == std::string::npos ? std::string() : full.substr(0, slash);
#else
	Dl_info info = {};
	if (dladdr(reinterpret_cast<const void *>(&plugin_binary_directory), &info) == 0 || !info.dli_fname)
		return {};

	std::string full(info.dli_fname);
	size_t slash = full.find_last_of('/');
	return slash == std::string::npos ? std::string() : full.substr(0, slash);
#endif
}

std::string environment_variable(const char *name)
{
	if (!name)
		return {};
	const char *value = std::getenv(name);
	return value ? std::string(value) : std::string();
}

} // namespace satellite
