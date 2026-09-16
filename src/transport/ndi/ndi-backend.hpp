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

#include <memory>
#include <string>
#include <vector>

namespace satellite {

/// Where the NDI runtime is expected to live on this platform, in search order.
///
/// The NDI SDK is proprietary and cannot be redistributed, so Satellite never ships it and
/// never links it at build time - it is located and dlopen'd at run time, and its absence
/// is reported to the user with an install link.
std::vector<std::string> ndi_runtime_candidates();

/// Where to send a user who does not have the runtime installed.
const char *ndi_install_url();

std::unique_ptr<IBackend> create_ndi_backend();

} // namespace satellite
