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

namespace satellite {

/// Registers "satellite_output", the raw AV output backing the Program and Preview senders.
/// Configured from the Satellite window rather than from a source.
void register_satellite_output();

/// Applies the current Config to the two frontend senders, starting or stopping each as
/// needed. Safe to call repeatedly; called on frontend events and after a settings change.
void update_frontend_outputs();

/// Tears down both frontend senders. Called on module unload and on frontend exit.
void shutdown_frontend_outputs();

} // namespace satellite
