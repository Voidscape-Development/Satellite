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

/// Where libomt is expected to live, in search order.
///
/// Unlike NDI, OMT is MIT-licensed, so Satellite bundles libomt and libvmx alongside the
/// plugin binary and looks there first. A system-wide install is still honoured as a
/// fallback for users who build OMT themselves.
std::vector<std::string> omt_runtime_candidates();

std::unique_ptr<IBackend> create_omt_backend();

} // namespace satellite
