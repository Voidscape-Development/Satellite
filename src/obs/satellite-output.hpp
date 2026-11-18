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

#include <obs.h>

namespace satellite {

class SenderSession;

/// Registers the raw outputs every sender takes its frames through: "satellite_output",
/// video with one mixer track of audio, and "satellite_video_output", video alone.
void register_satellite_output();

/// Feeds a SenderSession from a video_t for as long as it is started.
///
/// This is the only thing that makes OBS produce frames for a sender, and so the only part
/// of one that costs render time: while it is started OBS reads every frame of that video
/// mix back from the GPU. Senders therefore start it when their session is wanted() and stop
/// it when it is not, leaving the session - the feed on the network - up throughout.
///
/// It goes through an obs_output rather than video_output_connect because only an output
/// tells the mix that somebody wants raw frames. A callback connected directly is never
/// called on a view's mix: nothing asks that mix to download anything.
class SenderCapture {
public:
	SenderCapture() = default;
	~SenderCapture() { stop(); }

	SenderCapture(const SenderCapture &) = delete;
	SenderCapture &operator=(const SenderCapture &) = delete;

	/// mixer_track is the zero-based audio track to send with the video, or -1 for none.
	/// The session has to outlive the capture, and so does the video_t: stop() before
	/// tearing down the view that owns it.
	bool start(const char *name, SenderSession &session, video_t *video, int mixer_track);
	void stop();
	bool active() const { return output_ != nullptr; }

	/// Why the last start() failed.
	const std::string &error() const { return error_; }

private:
	obs_output_t *output_ = nullptr;
	std::string error_;
};

/// Applies the current Config to every frontend sender - Program, Preview and each canvas
/// sender - starting or stopping each as needed. Safe to call repeatedly; called on
/// frontend events, on canvas changes, and after a settings change. UI thread only.
void update_frontend_outputs();

/// Tears down every frontend sender. Called on module unload and on frontend exit.
void shutdown_frontend_outputs();

/// What one frontend sender is doing, for the Satellite window.
enum class SenderState {
	Off,     //< not enabled
	Live,    //< on the network, whether or not anybody is receiving it yet
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
