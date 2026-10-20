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

#include <cstdint>
#include <string>
#include <vector>

namespace satellite {

/// Registers "satellite_output", the raw AV output backing the Program and Preview senders.
/// Configured from the Satellite window rather than from a source.
void register_satellite_output();

/// Applies the current Config to every frontend sender - Program, Preview and each canvas
/// sender - starting or stopping each as needed. Safe to call repeatedly; called on
/// frontend events, on canvas changes, and after a settings change. UI thread only.
void update_frontend_outputs();

/// Tears down every frontend sender. Called on module unload and on frontend exit.
void shutdown_frontend_outputs();

/// What one frontend sender is doing, for the Satellite window.
enum class SenderState {
	Off,     //< not enabled
	Live,    //< running
	Waiting, //< enabled, but waiting on something outside Satellite (Studio Mode, a canvas)
	Failed,  //< enabled, and could not start
};

struct SenderStatus {
	SenderState state = SenderState::Off;
	/// Why a sender is Waiting or Failed; empty otherwise.
	std::string detail;
};

SenderStatus program_sender_status();
SenderStatus preview_sender_status();
/// Status of the canvas sender with this CanvasSenderConfig id.
SenderStatus canvas_sender_status(int id);

/// One additional canvas that can be sent, as listed in the Satellite window.
struct CanvasInfo {
	std::string uuid;
	std::string name;
	uint32_t width = 0;
	uint32_t height = 0;
};

/// False when Satellite was built against a libobs without canvas support (before 31.1).
bool canvases_supported();

/// Every canvas other than the main one that currently has video. The main canvas is what
/// the Program sender already carries.
std::vector<CanvasInfo> list_canvases();

} // namespace satellite
