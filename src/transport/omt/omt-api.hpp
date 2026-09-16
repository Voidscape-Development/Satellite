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

#include "transport/library-loader.hpp"

#include <libomt.h>

namespace satellite {

/// The libomt entry points Satellite uses, bound by name at run time.
///
/// libomt exports flat C functions rather than NDI's single load-the-struct entry point, so
/// unlike the NDI backend this table is assembled here. Every member is required; a missing
/// one means the library is not the libomt we expect, and the backend reports itself
/// unavailable rather than crashing on a null call later.
struct OmtApi {
	// Discovery
	char **(*discovery_getaddresses)(int *count) = nullptr;

	// Receive
	omt_receive_t *(*receive_create)(const char *address, OMTFrameType frameTypes, OMTPreferredVideoFormat format,
					 OMTReceiveFlags flags) = nullptr;
	void (*receive_destroy)(omt_receive_t *instance) = nullptr;
	OMTMediaFrame *(*receive)(omt_receive_t *instance, OMTFrameType frameTypes, int timeoutMilliseconds) = nullptr;
	void (*receive_settally)(omt_receive_t *instance, OMTTally *tally) = nullptr;
	void (*receive_setsuggestedquality)(omt_receive_t *instance, OMTQuality quality) = nullptr;
	void (*receive_getvideostatistics)(omt_receive_t *instance, OMTStatistics *stats) = nullptr;

	// Send
	omt_send_t *(*send_create)(const char *name, OMTQuality quality) = nullptr;
	void (*send_destroy)(omt_send_t *instance) = nullptr;
	int (*send)(omt_send_t *instance, OMTMediaFrame *frame) = nullptr;
	int (*send_connections)(omt_send_t *instance) = nullptr;
	int (*send_gettally)(omt_send_t *instance, int timeoutMilliseconds, OMTTally *tally) = nullptr;
	void (*send_getvideostatistics)(omt_send_t *instance, OMTStatistics *stats) = nullptr;

	// Settings and lifecycle
	void (*settings_set_string)(const char *name, const char *value) = nullptr;
	void (*settings_set_integer)(const char *name, int value) = nullptr;
	void (*shutdown)() = nullptr;

	/// Binds every entry point. Returns false and names the first missing symbol in
	/// missing_symbol if the library does not export the full set.
	bool bind(const LibraryHandle &library, std::string &missing_symbol);
};

} // namespace satellite
