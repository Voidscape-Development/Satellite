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

/// Thin wrappers over the handful of libobs calls that changed between OBS 30 and 31.
///
/// Satellite builds against 31.1.1 in CI, but a 30.x libobs is what most distributions still
/// package, and being able to build against the system libobs is what makes local testing
/// possible at all. These keep both working without scattering version checks through the
/// code that calls them.

#include <obs-config.h>
#include <obs-frontend-api.h>
#include <obs.h>

namespace satellite {

/// OBS's per-user configuration.
///
/// obs_frontend_get_global_config() is deprecated from OBS 30.2 in favour of
/// obs_frontend_get_user_config(), and the deprecation is an error under the project's
/// -Werror. Both name the same store, which is where DistroAV keeps its output settings.
inline config_t *obs_user_config()
{
#if LIBOBS_API_VER >= MAKE_SEMANTIC_VERSION(30, 2, 0)
	return obs_frontend_get_user_config();
#else
	return obs_frontend_get_global_config();
#endif
}

/// Scene item transform accessors.
///
/// OBS 30.1 added obs_transform_info::crop_to_bounds and the _info2 accessors that
/// understand it, and deprecated the originals.
inline void sceneitem_get_transform(const obs_sceneitem_t *item, obs_transform_info *info)
{
#if LIBOBS_API_VER >= MAKE_SEMANTIC_VERSION(30, 1, 0)
	obs_sceneitem_get_info2(item, info);
#else
	obs_sceneitem_get_info(item, info);
#endif
}

inline void sceneitem_set_transform(obs_sceneitem_t *item, const obs_transform_info *info)
{
#if LIBOBS_API_VER >= MAKE_SEMANTIC_VERSION(30, 1, 0)
	obs_sceneitem_set_info2(item, info);
#else
	obs_sceneitem_set_info(item, info);
#endif
}

} // namespace satellite
